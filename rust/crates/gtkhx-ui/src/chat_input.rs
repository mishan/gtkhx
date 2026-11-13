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
//!     (`route_line`: chat, a server command, or a client `/command`), clear.
//!     A line said as chat answers the line the reply bar holds.
//!   - **Escape** → cancel a pending reply (`reply_bar`), and only then.
//!   - **Tab / Shift+Tab** → nick completion against the chat's membership
//!     (the C `tab_nick_comp` core — kept in C so the tested completer + the
//!     ambiguous-list `hx_printf` stay put).
//!   - **Up / Down** → history navigation (the Rust `InputHistory` owns the
//!     draft snapshot), but only from the edge of the draft — see
//!     `HISTORY_NAV_KEY`.
//!
//! `gtkhx_chat_input_attach` captures `(sess, cid, history)` once — all three
//! are set on the `gtkhx_chat` before the input is attached and are stable for
//! the controller's (widget's) lifetime, so there's no per-keystroke lookup
//! back into the C view struct.

use std::ffi::{c_char, c_void, CStr};
use std::ptr;

use glib::translate::{from_glib_borrow, from_glib_none};
use gtk::glib;
use gtk::prelude::*;
use gtk4 as gtk;
use hxproto::target::Target;

use crate::tr::tr;
use hxhandlers::send::chat::send_chat_line;
use hxmodel::chat_members::{hx_input_history_down, hx_input_history_record, hx_input_history_up};
use hxmodel::conversation::hx_chat_member_model;

extern "C" {
    // gtkhx_ui_bridge.c, always compiled — unlike voice_bridge.c's
    // hx_session_htlc, which is gated on HAVE_VOICE.
    fn gtkhx_session_htlc(sess: *mut c_void) -> *mut c_void;
    fn hx_command(htlc: *mut c_void, s: *mut c_char, cid: u32);

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

/// Commands the server runs itself (hxd's and mhxd's chat commands): a line
/// starting with one goes out as chat, slash and all.
const SERVER_COMMANDS: &[&str] = &[
    "0wn",
    "access",
    "away",
    "alert",
    "broadcast",
    "color",
    "g0away",
    "maltbl",
    "mon",
    "users",
    "version",
    "visible",
];

#[derive(Debug, PartialEq)]
enum Route<'a> {
    Chat(&'a [u8]),
    /// A command the server runs, sent as chat.
    Server(&'a [u8]),
    Command(&'a [u8]),
}

/// Where a typed line goes. `//` escapes the slash, so `//exec ls` sends the
/// server `/exec ls` instead of running `/exec` here.
fn route_line(line: &[u8]) -> Option<Route<'_>> {
    match line {
        [] => None,
        [b'/', b'/', ..] => Some(Route::Chat(&line[1..])),
        [b'/', rest @ ..] => {
            let word = rest
                .split(u8::is_ascii_whitespace)
                .next()
                .unwrap_or_default();
            if word.is_empty() {
                Some(Route::Chat(line))
            } else if SERVER_COMMANDS.iter().any(|c| c.as_bytes() == word) {
                Some(Route::Server(line))
            } else {
                Some(Route::Command(rest))
            }
        }
        _ => Some(Route::Chat(line)),
    }
}

/// Sends a line from `buffer_cbytes`, typed in `input`, wherever
/// `route_line` says: to `chat`, answering the line `input`'s reply bar
/// holds where it is said as chat, or to `command`. A server's command, or
/// a client's, leaves the reply for the next line.
fn dispatch_line(
    input: &gtk::TextView,
    line: &[u8],
    chat: impl FnOnce(&CStr, Option<&Target>),
    command: impl FnOnce(&CStr),
) {
    let text = line.strip_suffix(&[0]).unwrap_or(line);
    // Every route is a tail of the line, so the buffer's own NUL ends it.
    let tail =
        |s: &[u8]| CStr::from_bytes_until_nul(&line[text.len() - s.len()..]).unwrap_or_default();
    match route_line(text) {
        Some(Route::Chat(s)) => chat(tail(s), crate::reply_bar::take(input).as_ref()),
        Some(Route::Server(s)) => chat(tail(s), None),
        Some(Route::Command(s)) => command(tail(s)),
        None => {}
    }
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

/// Set while Up/Down are stepping through history; cleared the moment the
/// caret moves for any other reason.
///
/// Up only *enters* history from the start of the draft, so that in a
/// multi-line message Up is an ordinary "move up a line" and there is a
/// way to reach the text you want to edit. But leaving it at that would
/// make stepping back through several entries impossible: a recall puts
/// the caret at the end of the text it just inserted, so the next Up
/// would move the caret instead of continuing. Hence the flag — once
/// you are navigating you keep navigating, and anything that moves the
/// caret (typing, clicking, an arrow key we let through) ends it.
const HISTORY_NAV_KEY: &str = "gtkhx-chat-input-history-nav";

fn history_nav(buf: &gtk::TextBuffer) -> bool {
    unsafe { buf.data::<bool>(HISTORY_NAV_KEY).map(|p| *p.as_ref()) }.unwrap_or(false)
}

fn set_history_nav(buf: &gtk::TextBuffer, on: bool) {
    unsafe { buf.set_data(HISTORY_NAV_KEY, on) };
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
            // against both the light and the dark input background. Not
            // under high contrast, where it falls short; the delimiters
            // keep the text color there.
            gtk::TextTag::builder()
                .name("md-delim")
                .foreground("#888888")
                .foreground_set(!libadwaita::StyleManager::default().is_high_contrast())
                .build(),
            gtk::TextTag::builder().name("md-bold").weight(700).build(),
            gtk::TextTag::builder()
                .name("md-italic")
                .style(gtk::pango::Style::Italic)
                .build(),
            gtk::TextTag::builder()
                .name("md-code")
                .family("monospace")
                .build(),
        ] {
            tt.add(&t);
        }
    }

    // Hold the tag objects rather than their names: `*_by_name` is the
    // same runtime-lookup shape that caused the abort above, and there
    // is no reason to keep one instance of it around.
    let Some(delim) = tt.lookup("md-delim") else {
        return;
    };
    let Some(bold) = tt.lookup("md-bold") else {
        return;
    };
    let Some(italic) = tt.lookup("md-italic") else {
        return;
    };
    let Some(code) = tt.lookup("md-code") else {
        return;
    };

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

    for sp in rotulus_layout::scan_delims(&text) {
        let (Some(&a), Some(&b)) = (char_of.get(&sp.start), char_of.get(&sp.end)) else {
            continue;
        };
        let (ia, ib) = (buf.iter_at_offset(a), buf.iter_at_offset(b));
        let tag = if sp.delim {
            &delim
        } else if sp.attrs.contains(rotulus_layout::Attrs::BOLD) {
            &bold
        } else if sp.attrs.contains(rotulus_layout::Attrs::CODE) {
            &code
        } else {
            &italic
        };
        buf.apply_tag(tag, &ia, &ib);
    }
}

/// Alt or Shift held — a chord the history keys must leave alone.
fn has_chord_modifier(state: gtk::gdk::ModifierType) -> bool {
    state.intersects(gtk::gdk::ModifierType::ALT_MASK | gtk::gdk::ModifierType::SHIFT_MASK)
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
    let nav = history_nav(&buf);

    if state.contains(gtk::gdk::ModifierType::CONTROL_MASK) {
        // Ctrl chords are exclusive — no send/complete/history under Ctrl.
        if keyval == Key::k || keyval == Key::K {
            unsafe { create_connect_window(ptr::null_mut(), sess) };
            return glib::Propagation::Stop;
        }
        // Ctrl+U discards the draft. Delegates to the application
        // action rather than repeating it here: toolbar.c's
        // on_action_clear_input already handles every focused text input
        // in the app, and a second copy of "clear the thing" is how the
        // two drift.
        //
        // Handled here at all — rather than left to the app accel — so
        // it is certain to fire over a focused GtkTextView regardless of
        // what GTK binds Ctrl+U to, and returning Stop keeps the accel
        // from running a second time.
        if keyval == Key::u || keyval == Key::U {
            if let Some(app) = gtk::gio::Application::default() {
                app.activate_action("clear-input", None);
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
            let line = buffer_cbytes(&buf);
            unsafe {
                hx_input_history_record(history, line.as_ptr() as *const c_char);
                // The input's own connection, which a line typed just as the
                // focus moves to another tab still belongs to.
                let htlc = gtkhx_session_htlc(sess);
                dispatch_line(
                    view,
                    &line,
                    |s, reply| send_chat_line(htlc, s, cid, 0, reply),
                    |s| hx_command(htlc, s.as_ptr().cast_mut(), cid),
                );
            }
            let (mut s, mut e) = (buf.start_iter(), buf.end_iter());
            buf.delete(&mut s, &mut e);
            view.set_editable(true);
            glib::Propagation::Stop
        }

        Key::Escape => on_escape(view),

        Key::Tab | Key::ISO_Left_Tab => {
            let reverse =
                keyval == Key::ISO_Left_Tab || state.contains(gtk::gdk::ModifierType::SHIFT_MASK);
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
            // Only from the very start of the draft, or when already
            // stepping. Otherwise this is a cursor movement and the view
            // should have it. Never with Alt or Shift held: those chords
            // belong to others — Alt+Shift+arrows move the pane
            // (the dock), Shift extends a selection.
            if (point != 0 && !nav) || has_chord_modifier(state) {
                return glib::Propagation::Proceed;
            }
            let cur = buffer_cbytes(&buf);
            let mut nt: *mut c_char = ptr::null_mut();
            let got =
                unsafe { hx_input_history_up(history, cur.as_ptr() as *const c_char, &mut nt) };
            if got != glib::ffi::GFALSE {
                if !nt.is_null() {
                    let s: String = unsafe { from_glib_none(nt) };
                    set_line(&buf, &s);
                    unsafe { glib::ffi::g_free(nt as *mut c_void) };
                }
                // After set_line: placing the caret fires mark-set,
                // which clears the flag we are setting here.
                set_history_nav(&buf, true);
                return glib::Propagation::Stop;
            }
            glib::Propagation::Proceed
        }

        Key::Down => {
            // Mirror of Up: only from the very end of the draft.
            if (point != buf.char_count() && !nav) || has_chord_modifier(state) {
                return glib::Propagation::Proceed;
            }
            let mut nt: *mut c_char = ptr::null_mut();
            let got = unsafe { hx_input_history_down(history, &mut nt) };
            if got != glib::ffi::GFALSE {
                if !nt.is_null() {
                    let s: String = unsafe { from_glib_none(nt) };
                    set_line(&buf, &s);
                    unsafe { glib::ffi::g_free(nt as *mut c_void) };
                }
                // After set_line: placing the caret fires mark-set,
                // which clears the flag we are setting here.
                set_history_nav(&buf, true);
                return glib::Propagation::Stop;
            }
            glib::Propagation::Proceed
        }

        _ => glib::Propagation::Proceed,
    }
}

/// Escape in `input` cancels a pending reply, and only then is taken: it
/// is otherwise whoever's else.
fn on_escape(input: &gtk::TextView) -> glib::Propagation {
    match crate::reply_bar::escape(input) {
        true => glib::Propagation::Stop,
        false => glib::Propagation::Proceed,
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
    tv.update_property(&[gtk::accessible::Property::Label(&tr("Chat message"))]);

    let ctrl = gtk::EventControllerKey::new();
    ctrl.connect_key_pressed(move |c, keyval, _keycode, state| {
        match c.widget().and_then(|w| w.downcast::<gtk::TextView>().ok()) {
            Some(view) => on_key(&view, sess, cid, history, keyval, state),
            None => glib::Propagation::Proceed,
        }
    });
    tv.add_controller(ctrl);

    // Any caret move ends a history-navigation session. `mark-set` on
    // the insert mark rather than a key handler, because clicking into
    // the draft has to end it too, and a key handler would never see
    // that.
    tv.buffer().connect_mark_set(|buf, _iter, mark| {
        if mark.name().as_deref() == Some("insert") {
            set_history_nav(buf, false);
        }
    });

    // Live markdown tinting. `changed` rather than a key handler, so
    // paste, emoji typeahead and history recall all tint too.
    tv.buffer().connect_changed(apply_tint);
}

#[cfg(test)]
pub(crate) mod tests {
    use super::{dispatch_line, on_escape, route_line, Route};
    use gtk::glib;
    use gtk::prelude::*;
    use gtk4 as gtk;
    use hxmodel::replies::Quote;
    use hxproto::target::Target;
    use rotulus::RotulusView;

    fn text(s: &std::ffi::CStr) -> String {
        s.to_string_lossy().into_owned()
    }

    /// Where `line` typed in `input` goes: to chat, with the line it
    /// answers, or to a command.
    fn sent(input: &gtk::TextView, line: &str) -> (&'static str, String, Option<Target>) {
        let mut line = line.as_bytes().to_vec();
        line.push(0);
        let got = std::cell::RefCell::new(None);
        dispatch_line(
            input,
            &line,
            |s, reply| *got.borrow_mut() = Some(("chat", text(s), reply.cloned())),
            |s| *got.borrow_mut() = Some(("command", text(s), None)),
        );
        got.into_inner().expect("sent nowhere")
    }

    /// Reply shows the bar over the input, which takes the keyboard; the
    /// next line said as chat answers the line, and the bar goes. Escape,
    /// the bar's button, a cleared view and a closed window let go of it;
    /// a server's or a client's command keeps it for the next line; and
    /// Reply on another row replaces it.
    pub(crate) fn check_a_reply_goes_with_the_next_line_said() {
        use crate::reply_bar;
        let view = RotulusView::new();
        let input = gtk::TextView::new();
        let bar = reply_bar::build(&view, input.upcast_ref());
        let column = gtk::Box::new(gtk::Orientation::Vertical, 0);
        column.append(&view);
        column.append(&bar);
        column.append(&input);
        let window = gtk::Window::new();
        window.set_child(Some(&column));
        window.present();
        let label = bar.first_child().and_downcast::<gtk::Label>().unwrap();
        let cancel = bar.last_child().and_downcast::<gtk::Button>().unwrap();

        let to = Target::Line { channel: 0, id: 5 };
        let quote = Quote::new("bob", "has anyone <tried> it");
        let start = |to: &Target| reply_bar::start(&view, to.clone(), &quote);
        let slot = crate::chat_output::nick_color(&view, "bob");
        let rotulus_layout::ColorRef::Palette(i) = slot else {
            panic!("{slot:?}")
        };
        let mut palette = [gtk::gdk::RGBA::TRANSPARENT; rotulus::view::PALETTE_COLS];
        palette[usize::from(i)] = gtk::gdk::RGBA::BLUE;
        view.set_palette(&palette);
        // The bar's background is the light window's.
        let style = libadwaita::StyleManager::default();
        style.set_color_scheme(libadwaita::ColorScheme::ForceLight);
        assert!(!bar.is_visible());
        start(&to);
        assert!(bar.is_visible());
        assert_eq!(label.text(), "Replying to bob: has anyone <tried> it");
        let ink = view.text_color(slot);
        assert_ne!(ink, view.text_color(rotulus_layout::ColorRef::Default));
        let drawn = || -> Vec<_> {
            (label.layout().attributes().unwrap().attributes())
                .into_iter()
                .filter_map(|a| a.downcast::<gtk::pango::AttrColor>().ok())
                .filter(|a| a.type_() == gtk::pango::AttrType::Foreground)
                .map(|a| {
                    let c = a.color();
                    let range = a.start_index()..a.end_index();
                    (range, [c.red(), c.green(), c.blue()].map(|v| v >> 8))
                })
                .collect()
        };
        let want = [ink.red(), ink.green(), ink.blue()].map(|v| (v * 255.0).round() as u16);
        let at = "Replying to ".len() as u32;
        assert_eq!(drawn(), [(at..at + 3, want)], "the nick's color");
        // White reads on a black view, but not on the bar.
        palette[rotulus::view::PAL_BG] = gtk::gdk::RGBA::BLACK;
        palette[usize::from(i)] = gtk::gdk::RGBA::WHITE;
        view.set_palette(&palette);
        start(&to);
        style.set_color_scheme(libadwaita::ColorScheme::Default);
        assert_eq!(drawn(), [], "a nick the bar can't show in its color");
        let bold = (label.layout().attributes().unwrap().attributes())
            .into_iter()
            .filter(|a| a.type_() == gtk::pango::AttrType::Weight)
            .map(|a| (a.start_index(), a.end_index()))
            .collect::<Vec<_>>();
        assert_eq!(bold, [(at, at + 3)], "still bold");
        assert_eq!(label.text(), "Replying to bob: has anyone <tried> it");
        assert_eq!(
            gtk::prelude::GtkWindowExt::focus(&window).as_ref(),
            Some(input.upcast_ref())
        );

        // The line a jump marked goes with the bar.
        let marked = view.append(rotulus::view::plain_message("marked"));
        view.set_spotlight(Some(marked));
        for (case, line, want) in [
            ("a server command", "/away", ("chat", "/away")),
            ("a client command", "/me waves", ("command", "me waves")),
        ] {
            assert_eq!(sent(&input, line), (want.0, want.1.into(), None), "{case}");
            assert!(bar.is_visible(), "{case}: kept");
            assert_eq!(view.spotlight(), Some(marked), "{case}: kept");
        }
        assert_eq!(
            sent(&input, "//exec yes"),
            ("chat", "/exec yes".into(), Some(to.clone()))
        );
        assert!(!bar.is_visible(), "sent");
        assert_eq!(view.spotlight(), None, "sent");
        assert_eq!(sent(&input, "and"), ("chat", "and".into(), None));

        let escape = || on_escape(&input);
        start(&to);
        assert_eq!(escape(), glib::Propagation::Stop, "pending");
        assert!(!bar.is_visible(), "escaped");
        assert_eq!(escape(), glib::Propagation::Proceed, "nothing pending");

        let other = Target::Ephemeral { chat: 0, id: 2 };
        start(&to);
        start(&other);
        assert_eq!(sent(&input, "y"), ("chat", "y".into(), Some(other)));

        let clicked = || cancel.emit_clicked();
        let cleared = || crate::chat_output::clear(&view);
        for (case, end) in [
            ("the button", &clicked as &dyn Fn()),
            ("a cleared view", &cleared),
        ] {
            start(&to);
            end();
            assert!(!bar.is_visible(), "{case}");
            assert_eq!(sent(&input, "x"), ("chat", "x".into(), None), "{case}");
        }

        start(&to);
        window.destroy();
        drop((window, column, bar, label, cancel));
        assert!(!reply_bar::offered(&view), "closed");
        assert_eq!(sent(&input, "x"), ("chat", "x".into(), None), "closed");
    }

    #[test]
    fn lines_route_to_chat_server_or_client() {
        let cases: &[(&str, Option<Route>)] = &[
            ("", None),
            ("hello", Some(Route::Chat(b"hello"))),
            ("/", Some(Route::Chat(b"/"))),
            ("/ hi", Some(Route::Chat(b"/ hi"))),
            ("/exec ls ~", Some(Route::Command(b"exec ls ~"))),
            ("/exec", Some(Route::Command(b"exec"))),
            ("//exec ls ~", Some(Route::Chat(b"/exec ls ~"))),
            ("/away", Some(Route::Server(b"/away"))),
            ("/visible on", Some(Route::Server(b"/visible on"))),
            ("/away\nback soon", Some(Route::Server(b"/away\nback soon"))),
            // A server command matches as a whole word, not as a prefix.
            ("/a b", Some(Route::Command(b"a b"))),
            ("/awayx", Some(Route::Command(b"awayx"))),
            ("/me waves", Some(Route::Command(b"me waves"))),
        ];
        for (line, want) in cases {
            assert_eq!(route_line(line.as_bytes()), *want, "{line:?}");
        }
    }
}
