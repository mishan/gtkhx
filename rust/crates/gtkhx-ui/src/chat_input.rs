//! `chat_input` — the chat / pchat input `GtkTextView` key handler (was
//! `chat.c::chat_input_key_pressed`, the most complex key handler in the tree).
//!
//! GTK 4 widgets don't fire `key-press-event`, so this installs a
//! `GtkEventControllerKey` on the input view. The bindings:
//!
//!   - **Ctrl+K** → open the Connect dialog.
//!   - **Ctrl+U** → discard the draft (undoable in one step).
//!   - **Ctrl+B / Ctrl+I / Ctrl+Shift+C** → wrap the selection in markdown.
//!   - **Shift+Return** → newline (let the view's default insert run).
//!   - **Return** → record the line in history, dispatch it
//!     (`hotline_client_input`, which routes `/commands` and chat), clear.
//!   - **Tab / Shift+Tab** → nick completion against the chat's membership
//!     (the C `tab_nick_comp` core — kept in C so the tested completer + the
//!     ambiguous-list `hx_printf` stay put).
//!   - **Up / Down** → history navigation (the Rust `InputHistory` owns the
//!     draft snapshot).
//!
//! `gtkhx_chat_input_attach` captures `(sess, cid, history)` once — all three
//! are set on the `gtkhx_chat` before the input is attached and are stable for
//! the controller's (widget's) lifetime, so there's no per-keystroke lookup
//! back into the C view struct.

use std::ffi::{c_char, c_void};
use std::ptr;

use gtk4 as gtk;
use gtk::glib;
use gtk::prelude::*;
use glib::translate::{from_glib_borrow, from_glib_none};

use hxmodel::chat_members::{hx_input_history_down, hx_input_history_record, hx_input_history_up};
use hxmodel::conversation::hx_chat_member_model;

extern "C" {
    // The session's htlc. gtkhx_active_htlc (gtkhx_ui_bridge.c) is always
    // compiled — unlike voice_bridge.c's hx_session_htlc, which is gated on
    // HAVE_VOICE and would leave this undefined in -Dvoice=disabled builds.
    // Single-session, so the active htlc *is* this input's session's htlc.
    fn gtkhx_active_htlc() -> *mut c_void;
    fn hotline_client_input(htlc: *mut c_void, s: *mut c_char, cid: u32, style: u16);

    fn chat_with_cid(sess: *mut c_void, cid: u32) -> *mut c_void;
    fn tab_nick_comp(
        sess: *mut c_void,
        member_model: *mut c_void,
        text: *mut c_char,
        reverse: glib::ffi::gboolean,
        pos: i32,
        entry: *mut gtk::ffi::GtkWidget,
    ) -> i32;

    fn create_connect_window(btn: *mut gtk::ffi::GtkWidget, data: *mut c_void);
}

/// The whole buffer as a NUL-terminated, mutable byte buffer — the C send /
/// completion paths take `char*` and may tokenise in place (strtok), so the
/// backing storage must be writable and outlive the call.
fn buffer_cbytes(buf: &gtk::TextBuffer) -> Vec<u8> {
    let text = buf.text(&buf.start_iter(), &buf.end_iter(), false);
    let mut bytes = text.as_bytes().to_vec();
    bytes.push(0);
    bytes
}

/// Replace the input with `s` and put the caret at the end (the shape both
/// history navigation and completion want).
fn set_line(buf: &gtk::TextBuffer, s: &str) {
    buf.set_text(s);
    buf.place_cursor(&buf.end_iter());
}

/// Wrap the selection in `delim`, or unwrap it if it is already
/// wrapped; with no selection, insert the pair and sit between them.
///
/// Toggling matters more than it looks: without it, Ctrl+B on text you
/// just bolded gives `****text****`, and the only way back is to hunt
/// down four asterisks by hand.
fn wrap_selection(buf: &gtk::TextBuffer, delim: &str) {
    // GtkTextBuffer offsets are in characters, not bytes.
    let dlen = delim.chars().count() as i32;

    buf.begin_user_action();
    match buf.selection_bounds() {
        Some((mut start, mut end)) => {
            let text = buf.text(&start, &end, false).to_string();
            let at = start.offset();
            let (body, wrapped) = match strip_wrap(&text, delim) {
                Some(inner) => (inner.to_string(), false),
                None => (format!("{delim}{text}{delim}"), true),
            };
            buf.delete(&mut start, &mut end);
            buf.insert(&mut start, &body);
            // Keep the *content* selected either way, so a second press
            // round-trips exactly rather than drifting by a delimiter.
            let inner_start = at + if wrapped { dlen } else { 0 };
            let inner_len = body.chars().count() as i32 - if wrapped { 2 * dlen } else { 0 };
            buf.select_range(
                &buf.iter_at_offset(inner_start),
                &buf.iter_at_offset(inner_start + inner_len),
            );
        }
        None => {
            let at = buf.cursor_position();
            let mut it = buf.iter_at_offset(at);
            buf.insert(&mut it, &format!("{delim}{delim}"));
            buf.place_cursor(&buf.iter_at_offset(at + dlen));
        }
    }
    buf.end_user_action();
}

/// The inside of `text` if it is exactly `delim...delim`, else None.
fn strip_wrap<'a>(text: &'a str, delim: &str) -> Option<&'a str> {
    let n = delim.len();
    if text.len() >= 2 * n && text.starts_with(delim) && text.ends_with(delim) {
        Some(&text[n..text.len() - n])
    } else {
        None
    }
}

/// Live markdown tinting for the compose box.
///
/// Purely a hint: the delimiters are dimmed and their contents shown in
/// the style they will render as, so you can see that `**this**` took
/// before you send it. Nothing here changes the text, and the text is
/// what goes on the wire.
fn apply_tint(buf: &gtk::TextBuffer) {
    let tt = buf.tag_table();
    if tt.lookup("md-delim").is_none() {
        // Typed builder, not `create_tag`'s `&[(name, value)]` slice.
        // The stringly-typed form looks up the property at runtime and
        // *panics* if it doesn't exist — and a panic in a GTK signal
        // callback is a non-unwinding one, so it aborts the process
        // rather than failing the call. The first cut of this used an
        // `alpha` property that GtkTextTag does not have, and every
        // keystroke in the chat input killed GtkHx. With the builder a
        // wrong property is a compile error.
        for t in [
            // Mid grey rather than an alpha on the theme foreground:
            // GtkTextTag has no alpha property, and grey is legible
            // against both the light and the dark input background.
            gtk::TextTag::builder().name("md-delim").foreground("#888888").build(),
            gtk::TextTag::builder().name("md-bold").weight(700).build(),
            gtk::TextTag::builder()
                .name("md-italic")
                .style(gtk::pango::Style::Italic)
                .build(),
            gtk::TextTag::builder().name("md-code").family("monospace").build(),
        ] {
            tt.add(&t);
        }
    }

    // Hold the tag objects rather than their names: `*_by_name` is the
    // same runtime-lookup shape that caused the abort above, and there
    // is no reason to keep one instance of it around.
    let Some(delim) = tt.lookup("md-delim") else { return };
    let Some(bold) = tt.lookup("md-bold") else { return };
    let Some(italic) = tt.lookup("md-italic") else { return };
    let Some(code) = tt.lookup("md-code") else { return };

    let (start, end) = buf.bounds();
    for t in [&delim, &bold, &italic, &code] {
        buf.remove_tag(t, &start, &end);
    }

    let text = buf.text(&start, &end, false).to_string();
    if text.is_empty() {
        return;
    }
    // GtkTextBuffer indexes by character; the scanner reports bytes.
    let char_of: std::collections::HashMap<usize, i32> = text
        .char_indices()
        .enumerate()
        .map(|(ci, (bi, _))| (bi, ci as i32))
        .chain(std::iter::once((text.len(), text.chars().count() as i32)))
        .collect();

    for sp in hxchat_layout::scan_delims(&text) {
        let (Some(&a), Some(&b)) = (char_of.get(&sp.start), char_of.get(&sp.end)) else {
            continue;
        };
        let (ia, ib) = (buf.iter_at_offset(a), buf.iter_at_offset(b));
        let tag = if sp.delim {
            &delim
        } else if sp.attrs.contains(hxchat_layout::Attrs::BOLD) {
            &bold
        } else if sp.attrs.contains(hxchat_layout::Attrs::CODE) {
            &code
        } else {
            &italic
        };
        buf.apply_tag(tag, &ia, &ib);
    }
}

fn on_key(
    view: &gtk::TextView,
    sess: *mut c_void,
    cid: u32,
    history: *mut c_void,
    keyval: gtk::gdk::Key,
    state: gtk::gdk::ModifierType,
) -> glib::Propagation {
    use gtk::gdk::Key;
    let buf = view.buffer();
    let point = buf.cursor_position();

    if state.contains(gtk::gdk::ModifierType::CONTROL_MASK) {
        // Ctrl chords are exclusive — no send/complete/history under Ctrl.
        if keyval == Key::k || keyval == Key::K {
            unsafe { create_connect_window(ptr::null_mut(), sess) };
            return glib::Propagation::Stop;
        }
        // Ctrl+U discards the draft, as it does in a shell and in most
        // chat clients.
        //
        // The whole buffer, not readline's kill-to-start-of-line: this
        // is a compose box, and Shift+Return means a multi-line draft is
        // one message rather than several lines. Clearing to the cursor
        // would leave half a message behind, which is not what anyone
        // pressing it wants.
        //
        // Wrapped in a user action so Ctrl+Z brings it back in one step —
        // a discard with no undo is a bad trade for a key this easy to
        // hit by accident.
        if keyval == Key::u || keyval == Key::U {
            let (mut start, mut end) = buf.bounds();
            if start != end {
                buf.begin_user_action();
                buf.delete(&mut start, &mut end);
                buf.end_user_action();
            }
            return glib::Propagation::Stop;
        }
        // Markdown compose affordances. These only touch the text you
        // are typing: markdown is transmitted literally (the Hotline
        // wire format has no styling concept at all), so `**bold**`
        // goes out as `**bold**` and other GtkHx clients render it.
        let shift = state.contains(gtk::gdk::ModifierType::SHIFT_MASK);
        let delim = match keyval {
            Key::b | Key::B if !shift => Some("**"),
            Key::i | Key::I if !shift => Some("*"),
            Key::c | Key::C if shift => Some("`"),
            _ => None,
        };
        if let Some(d) = delim {
            wrap_selection(&buf, d);
            return glib::Propagation::Stop;
        }
        return glib::Propagation::Proceed;
    }

    if state.contains(gtk::gdk::ModifierType::SHIFT_MASK) && keyval == Key::Return {
        // Shift+Return inserts a newline — let the view's default run.
        return glib::Propagation::Proceed;
    }

    match keyval {
        Key::Return => {
            view.set_editable(false);
            let mut line = buffer_cbytes(&buf);
            unsafe {
                hx_input_history_record(history, line.as_ptr() as *const c_char);
                let htlc = gtkhx_active_htlc();
                hotline_client_input(htlc, line.as_mut_ptr() as *mut c_char, cid, 0);
            }
            let (mut s, mut e) = (buf.start_iter(), buf.end_iter());
            buf.delete(&mut s, &mut e);
            view.set_editable(true);
            glib::Propagation::Stop
        }

        Key::Tab | Key::ISO_Left_Tab => {
            let reverse = keyval == Key::ISO_Left_Tab
                || state.contains(gtk::gdk::ModifierType::SHIFT_MASK);
            let mut text = buffer_cbytes(&buf);
            unsafe {
                let chat = chat_with_cid(sess, cid);
                let mm = if chat.is_null() {
                    ptr::null_mut()
                } else {
                    hx_chat_member_model(chat.cast())
                };
                // tab_nick_comp rewrites the buffer + caret itself.
                tab_nick_comp(
                    sess,
                    mm,
                    text.as_mut_ptr() as *mut c_char,
                    reverse as glib::ffi::gboolean,
                    point,
                    view.as_ptr() as *mut gtk::ffi::GtkWidget,
                );
            }
            view.grab_focus();
            glib::Propagation::Stop
        }

        Key::Up => {
            let cur = buffer_cbytes(&buf);
            let mut nt: *mut c_char = ptr::null_mut();
            let got = unsafe {
                hx_input_history_up(history, cur.as_ptr() as *const c_char, &mut nt)
            };
            if got != glib::ffi::GFALSE {
                if !nt.is_null() {
                    let s: String = unsafe { from_glib_none(nt) };
                    set_line(&buf, &s);
                    unsafe { glib::ffi::g_free(nt as *mut c_void) };
                }
                return glib::Propagation::Stop;
            }
            glib::Propagation::Proceed
        }

        Key::Down => {
            let mut nt: *mut c_char = ptr::null_mut();
            let got = unsafe { hx_input_history_down(history, &mut nt) };
            if got != glib::ffi::GFALSE {
                if !nt.is_null() {
                    let s: String = unsafe { from_glib_none(nt) };
                    set_line(&buf, &s);
                    unsafe { glib::ffi::g_free(nt as *mut c_void) };
                }
                return glib::Propagation::Stop;
            }
            glib::Propagation::Proceed
        }

        _ => glib::Propagation::Proceed,
    }
}

/// Set once on the input `GtkTextView` after its controller is installed, so a
/// repeat `gtkhx_chat_input_attach` is a no-op instead of stacking controllers.
const ATTACHED_KEY: &str = "gtkhx-chat-input-attached";

/// Install the chat-input key handler on `view` (a `GtkTextView`). `sess` is
/// the owning session, `cid` the chat id the input belongs to, and `history`
/// its `InputHistory` handle — all borrowed for the widget's lifetime.
///
/// # Safety
/// `view` is a valid `GtkTextView*`; `sess` / `history` are valid for as long
/// as the input widget lives (they hang off the same `gtkhx_chat`).
#[no_mangle]
pub unsafe extern "C" fn gtkhx_chat_input_attach(
    view: *mut gtk::ffi::GtkWidget,
    sess: *mut c_void,
    cid: u32,
    history: *mut c_void,
) {
    if view.is_null() {
        return;
    }
    crate::ensure_gtk_init();

    // BORROW the pointer — do NOT use `from_glib_none`. At attach time the input
    // GtkTextView is freshly `gtk_text_view_new()`d and still carries its
    // floating reference (it isn't parented until later, in chat.rs
    // build_content). glib-rs `from_glib_none` sinks floating references, so the
    // owned wrapper would hold the only ref and, on drop at the end of this
    // function, finalize the widget out from under its C caller. `from_glib_borrow`
    // neither refs nor sinks, so the widget's lifetime stays with C.
    let w = unsafe { from_glib_borrow::<_, gtk::Widget>(view) };
    let Some(tv) = w.downcast_ref::<gtk::TextView>() else {
        return;
    };
    // Idempotence sentinel — a second attach (e.g. a reconnect rebuild) would
    // stack another controller and double every send / history step.
    if tv.data::<bool>(ATTACHED_KEY).is_some() {
        return;
    }
    tv.set_data(ATTACHED_KEY, true);

    let ctrl = gtk::EventControllerKey::new();
    ctrl.connect_key_pressed(move |c, keyval, _keycode, state| {
        match c.widget().and_then(|w| w.downcast::<gtk::TextView>().ok()) {
            Some(view) => on_key(&view, sess, cid, history, keyval, state),
            None => glib::Propagation::Proceed,
        }
    });
    tv.add_controller(ctrl);

    // Live markdown tinting. `changed` rather than a key handler, so
    // paste, emoji typeahead and history recall all tint too.
    tv.buffer().connect_changed(apply_tint);
}
