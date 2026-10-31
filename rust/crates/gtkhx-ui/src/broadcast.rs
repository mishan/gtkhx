//! Broadcasts, both ways.
//!
//! The composer (ported from the toolbar.c dialog) is an AdwAlertDialog with
//! a single AdwEntryRow, Send (default/suggested) and Cancel responses. Send
//! — or Enter in the entry — sends the text to every user on the server the
//! dialog was opened for, through `hxhandlers::send::user::broadcast`.
//! `gtkhx_broadcast_dialog_open` keeps the C-callback ABI so toolbar.c's
//! Broadcast button connects to it.
//!
//! A broadcast received is logged to the public chat, notified, chimed, and
//! shown as a toast when it is short or in a dialog when it is not
//! (`broadcastmsg`).

use std::ffi::{c_char, c_void, CStr};

use adw::prelude::*;
use glib::translate::from_glib_none;
use gtk::glib;
use gtk4 as gtk;
use libadwaita as adw;

use crate::dock::{self, Bound};
use crate::tr::{tr, tr1};

extern "C" {
    // gtkutil.c — Ctrl+W / Esc close accelerators on a dialog.
    fn gtkhx_dialog_add_close_shortcuts(dialog: *mut gtk::ffi::GtkWidget);

    // toolbar.c — the toolbar window (dialog parent). May be NULL.
    static toolbar_window: *mut gtk::ffi::GtkWidget;

    // gtkhx.c — the tag the client's own lines carry in chat.
    static INFOPREFIX: *const c_char;
    // notify.c / sound.c
    fn gtkhx_notify_broadcast(text: *const c_char);
    fn play_sound(sound: std::ffi::c_int);
}

/// `MSG` (sound.h, pinned in chat.c): broadcasts share the private-message chime.
const SOUND_MSG: std::ffi::c_int = 6;

/// `HX_CHAT_LOG_INFO_COLOR` (gtkhx_log.h, pinned in chat.c): the color of the `[hx]` tag.
const INFO_COLOR: i32 = 43;

/// The longest broadcast shown as a toast.
///
/// Servers send broadcasts for two things: an administrator's announcement
/// ("Server going down in 5 minutes"), which the user should see and
/// acknowledge, and an automated note ("0 command(s) at a time, please."),
/// which is short and not worth a dialog to click through. Length tells them
/// apart well enough; this is the shape of what servers in the wild send.
const TOAST_MAX: usize = 160;

/// Whether a broadcast is short enough for a toast rather than a dialog.
fn is_toast(text: &[u8]) -> bool {
    text.len() <= TOAST_MAX && !text.contains(&b'\n')
}

/// The palette slot a sender's name is drawn in, from their status word, as
/// the user list colors it. A regular user takes the default foreground,
/// which reads on light and dark themes alike.
fn name_color(status: u16) -> i32 {
    match status % 4 {
        1 => 14, // idle: grey
        2 => 4,  // admin: red
        3 => 13, // idle admin: pink
        _ => rotulus::ffi::ROTULUS_COLOR_DEFAULT.into(),
    }
}

/// A sender's name with its control bytes made visible. The session folds
/// only some of them, and one drawn raw into a text layout helps nobody.
fn defang(name: &[u8]) -> Vec<u8> {
    name.iter()
        .map(|&b| if b < 0x20 || b == 0x7f { b'?' } else { b })
        .collect()
}

/// One line into `htlc`'s public chat, tagged `name` in `color`.
unsafe fn log_line(htlc: *mut c_void, name: *const c_char, color: i32, line: &str) {
    let line = crate::cs(line);
    gtkhx_core::session::gtkhx_session_emit_chat_log_line(
        gtkhx_core::session::gtkhx_session_get_default(),
        htlc,
        0,
        name,
        color,
        line.as_ptr(),
    );
}

/// `void broadcastmsg (struct htlc_conn *htlc, const char *sender_name,
/// guint16 sender_color, char *text)` — show a broadcast `htlc` received.
/// `sender_name` is NULL when the server named nobody: older servers, and
/// their automated notes.
///
/// # Safety
/// `htlc` is a live connection; `sender_name` and `text` are NULL or C
/// strings. Main thread.
#[no_mangle]
pub unsafe extern "C" fn broadcastmsg(
    htlc: *mut c_void,
    sender_name: *const c_char,
    sender_color: u16,
    text: *const c_char,
) {
    // Notified first, so the desktop hears of it whichever way it is shown.
    gtkhx_notify_broadcast(text);
    // The `msg` signal the chimes key on is for private messages only.
    play_sound(SOUND_MSG);

    let text = if text.is_null() {
        c""
    } else {
        CStr::from_ptr(text)
    };
    let body = text.to_string_lossy();
    if !body.is_empty() {
        let name = if sender_name.is_null() {
            &b""[..]
        } else {
            CStr::from_ptr(sender_name).to_bytes()
        };
        if name.is_empty() {
            log_line(htlc, INFOPREFIX, INFO_COLOR, &tr1("broadcast: %s\n", &body));
        } else {
            let name = std::ffi::CString::new(defang(name)).unwrap_or_default();
            log_line(
                htlc,
                name.as_ptr(),
                name_color(sender_color),
                &format!("{body}\n"),
            );
        }
    }

    if is_toast(text.to_bytes()) {
        crate::ffi::toolbar_show_toast(text.as_ptr());
        return;
    }

    let dialog = adw::AlertDialog::new(Some(&tr("Broadcast")), None);
    dialog.add_response("ok", &tr("_OK"));
    dialog.set_default_response(Some("ok"));
    dialog.set_close_response("ok");

    let textbox = gtk::TextView::new();
    textbox.set_editable(false);
    textbox.set_cursor_visible(false);
    textbox.set_wrap_mode(gtk::WrapMode::Word);
    crate::ffi::gtkhx_apply_text_style(textbox.as_ptr() as *mut gtk::ffi::GtkWidget);
    textbox.buffer().set_text(&body);

    let scroll = gtk::ScrolledWindow::new();
    scroll.set_policy(gtk::PolicyType::Automatic, gtk::PolicyType::Automatic);
    scroll.set_size_request(300, 220);
    scroll.set_child(Some(&textbox));
    dialog.set_extra_child(Some(&scroll));

    gtkhx_dialog_add_close_shortcuts(dialog.as_ptr() as *mut gtk::ffi::GtkWidget);

    let parent: Option<gtk::Widget> = if toolbar_window.is_null() {
        None
    } else {
        Some(from_glib_none(toolbar_window))
    };
    dialog.present(parent.as_ref());
}

/// Send a broadcast to `conn`, if it is still connected: the dialog can
/// outlive it. Clamped to what the wire's u16 length holds, cut back to the
/// last whole character so the encoder never sees a split one.
fn send_broadcast(conn: Bound, text: &str) {
    let mut end = text.len().min(0xfffe);
    while !text.is_char_boundary(end) {
        end -= 1;
    }
    let text = &text[..end];
    if text.is_empty() {
        return;
    }
    if let Some(htlc) = dock::live_htlc(conn) {
        unsafe { hxhandlers::send::user::broadcast(htlc, text) };
    }
}

/// `void gtkhx_broadcast_dialog_open(GtkButton *btn, gpointer user_data)` —
/// the toolbar Broadcast button callback. Presents the composer.
///
/// # Safety
/// Called on the GTK main thread as a GTK signal callback.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_broadcast_dialog_open(
    _btn: *mut gtk::ffi::GtkButton,
    _user_data: *mut c_void,
) {
    crate::ensure_gtk_init();
    let conn = dock::bind(dock::active_key());
    if dock::live_htlc(conn).is_none() {
        return;
    }

    let dialog = adw::AlertDialog::new(
        Some(&tr("Broadcast")),
        Some(&tr("Send a broadcast message to every user on the server.")),
    );
    dialog.add_response("cancel", &tr("_Cancel"));
    dialog.add_response("send", &tr("_Send"));
    dialog.set_response_appearance("send", adw::ResponseAppearance::Suggested);
    dialog.set_default_response(Some("send"));
    dialog.set_close_response("cancel");

    let prefs_grp = adw::PreferencesGroup::new();
    let entry = adw::EntryRow::new();
    entry.set_title(&tr("Message"));
    prefs_grp.add(&entry);
    dialog.set_extra_child(Some(&prefs_grp));

    gtkhx_dialog_add_close_shortcuts(dialog.as_ptr() as *mut gtk::ffi::GtkWidget);

    // Send response → encode + emit.
    {
        let entry = entry.clone();
        dialog.connect_response(None, move |_dlg, response| {
            if response == "send" {
                send_broadcast(conn, entry.text().as_str());
            }
        });
    }
    // AdwEntryRow swallows Enter for its own signal (bypassing the default
    // response) — bridge it to the same send + close.
    {
        let dialog = dialog.downgrade();
        entry.connect_entry_activated(move |entry| {
            send_broadcast(conn, entry.text().as_str());
            if let Some(d) = dialog.upgrade() {
                d.close();
            }
        });
    }

    let parent: Option<gtk::Widget> = if toolbar_window.is_null() {
        None
    } else {
        Some(from_glib_none(toolbar_window))
    };
    dialog.present(parent.as_ref());
    entry.grab_focus();
}

#[cfg(test)]
mod tests {
    use super::{defang, is_toast};

    #[test]
    fn a_short_line_is_a_toast_and_anything_else_a_dialog() {
        let cases: &[(&[u8], bool)] = &[
            (b"", true),
            (b"0 command(s) at a time, please.", true),
            (&[b'x'; 160], true),
            (&[b'x'; 161], false),
            (b"Server going down\nin 5 minutes", false),
            (b"\n", false),
        ];
        for &(text, toast) in cases {
            assert_eq!(is_toast(text), toast, "{:?}", String::from_utf8_lossy(text));
        }
    }

    #[test]
    fn control_bytes_in_a_name_show_as_question_marks() {
        let cases: &[(&[u8], &[u8])] = &[
            (b"admin", b"admin"),
            (b"a\x03b\x1fc\x7f", b"a?b?c?"),
            (b"caf\xc3\xa9 ~", b"caf\xc3\xa9 ~"),
            (b"", b""),
        ];
        for &(raw, want) in cases {
            assert_eq!(defang(raw), want);
        }
    }
}
