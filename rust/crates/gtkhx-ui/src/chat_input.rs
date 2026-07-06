//! `chat_input` — the chat / pchat input `GtkTextView` key handler (was
//! `chat.c::chat_input_key_pressed`, the most complex key handler in the tree).
//!
//! GTK 4 widgets don't fire `key-press-event`, so this installs a
//! `GtkEventControllerKey` on the input view. The bindings:
//!
//!   - **Ctrl+K** → open the Connect dialog.
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

extern "C" {
    fn hx_input_history_record(hist: *mut c_void, line: *const c_char);
    fn hx_input_history_up(
        hist: *mut c_void,
        current: *const c_char,
        out: *mut *mut c_char,
    ) -> glib::ffi::gboolean;
    fn hx_input_history_down(hist: *mut c_void, out: *mut *mut c_char) -> glib::ffi::gboolean;

    // The session's htlc. gtkhx_active_htlc (gtkhx_ui_bridge.c) is always
    // compiled — unlike voice_bridge.c's hx_session_htlc, which is gated on
    // HAVE_VOICE and would leave this undefined in -Dvoice=disabled builds.
    // Single-session, so the active htlc *is* this input's session's htlc.
    fn gtkhx_active_htlc() -> *mut c_void;
    fn hotline_client_input(htlc: *mut c_void, s: *mut c_char, cid: u32, style: u16);

    fn chat_with_cid(sess: *mut c_void, cid: u32) -> *mut c_void;
    fn hx_chat_member_model(chat: *mut c_void) -> *mut c_void;
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
                    hx_chat_member_model(chat)
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
}
