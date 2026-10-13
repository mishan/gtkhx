//! Screen-reader announcements: things worth hearing that happen away from
//! the keyboard focus, such as a private message, a mention or a dropped
//! connection.

use std::ffi::c_char;

use gtk4 as gtk;

/// Announce `text` from `widget`'s window, or from the application's active
/// window when there is no widget. `assertive` interrupts what the screen
/// reader is saying; otherwise it waits its turn.
pub(crate) fn announce(widget: Option<&gtk::Widget>, text: &str, assertive: bool) {
    use gtk::prelude::*;
    if text.is_empty() {
        return;
    }
    let from = widget.cloned().or_else(|| {
        gtk::gio::Application::default()
            .and_downcast::<gtk::Application>()
            .and_then(|app| app.active_window())
            .map(|w| w.upcast())
    });
    if let Some(w) = from {
        w.announce(
            text,
            if assertive {
                gtk::AccessibleAnnouncementPriority::High
            } else {
                gtk::AccessibleAnnouncementPriority::Medium
            },
        );
    }
}

/// `void gtkhx_announce (const char *text, gboolean assertive)` — announce
/// from the active window.
///
/// # Safety
/// `text` is NULL or a NUL-terminated string. Main thread only.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_announce(text: *const c_char, assertive: glib::ffi::gboolean) {
    announce(None, &crate::cstr(text), assertive != 0);
}

/// `void gtkhx_announce_line (const char *sender, const char *body,
/// gboolean private_msg)` — announce an incoming private message, or a chat
/// line that mentions us, with who it is from.
///
/// # Safety
/// `sender` and `body` are NULL or NUL-terminated strings. Main thread only.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_announce_line(
    sender: *const c_char,
    body: *const c_char,
    private_msg: glib::ffi::gboolean,
) {
    let args = [crate::cstr(sender), crate::cstr(body)];
    let args = [args[0].as_str(), args[1].as_str()];
    let text = if private_msg != 0 {
        crate::tr::tr_argv("Message from %s: %s", &args)
    } else if args[0].is_empty() {
        // A line with no sender (an emote or a server line).
        crate::tr::tr1("You were mentioned: %s", args[1])
    } else {
        crate::tr::tr_argv("%s mentioned you: %s", &args)
    };
    announce(None, &text, false);
}
