//! The broadcast composer (ported from the toolbar.c dialog): an
//! AdwAlertDialog with a single AdwEntryRow, Send (default/suggested) and
//! Cancel responses. Send — or Enter in the entry — sends the text to every
//! user on the server the dialog was opened for, through
//! `hxhandlers::send::user::broadcast`. `gtkhx_broadcast_dialog_open` keeps
//! the C-callback ABI so toolbar.c's Broadcast button connects to it.

use std::ffi::c_void;

use adw::prelude::*;
use glib::translate::from_glib_none;
use gtk::glib;
use gtk4 as gtk;
use libadwaita as adw;

use crate::dock::{self, Bound};
use crate::tr::tr;

extern "C" {
    // gtkutil.c — Ctrl+W / Esc close accelerators on a dialog.
    fn gtkhx_dialog_add_close_shortcuts(dialog: *mut gtk::ffi::GtkWidget);

    // toolbar.c — the toolbar window (dialog parent). May be NULL.
    static toolbar_window: *mut gtk::ffi::GtkWidget;
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
