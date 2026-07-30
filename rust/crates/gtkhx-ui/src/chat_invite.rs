//! `chat_invite` — the incoming chat-invitation dialog (was
//! `chat.c::output_chat_invitation` + `chat_invite_response`).
//!
//! An `AdwAlertDialog` with Decline (close / Esc) and Join (suggested +
//! default) responses. Join sends `CHAT_JOIN`, Decline sends `CHAT_DECLINE` —
//! both through the Rust `hxchat-send` senders (`hx_chat_join` /
//! `hx_reject_chat`), resolved at the final C link like the other cross-crate
//! senders. The whole invite flow (dialog + both senders) is now Rust.

use std::ffi::{c_char, c_void};

use gtk4 as gtk;
use gtk::glib;
use libadwaita as adw;
use adw::prelude::*;
use glib::translate::from_glib_none;

use crate::tr::tr;

use hxhandlers::send::chat::{hx_chat_join, hx_reject_chat};

extern "C" {    // gtkutil.c — Ctrl+W / Esc close accelerators on a dialog.
    fn gtkhx_dialog_add_close_shortcuts(dialog: *mut gtk::ffi::GtkWidget);
    // gtkhx_ui_bridge.c — the chat window of `htlc`'s session (parent), may be
    // NULL. Scoped to the invited session, not the active one.
    fn gtkhx_htlc_chat_window(htlc: *mut c_void) -> *mut gtk::ffi::GtkWidget;
}

/// `void output_chat_invitation(struct htlc_conn *htlc, guint32 cid, char *name)`
/// — present the incoming private-chat invitation dialog.
///
/// # Safety
/// C-ABI entry from the model-side signal adapter, on the GTK main thread.
/// `htlc` is a valid session htlc; `name` is NULL or a NUL-terminated C string.
#[no_mangle]
pub unsafe extern "C" fn output_chat_invitation(
    htlc: *mut c_void,
    cid: u32,
    name: *const c_char,
) {
    crate::ensure_gtk_init();

    let name = crate::cstr(name);
    let body = format!(
        "{} {}: 0x{:08x}",
        name,
        tr("invites you to private chat"),
        cid
    );

    let dialog = adw::AlertDialog::new(Some(&tr("Chat Invitation")), Some(&body));
    dialog.add_response("decline", &tr("_Decline"));
    dialog.add_response("join", &tr("_Join"));
    dialog.set_response_appearance("join", adw::ResponseAppearance::Suggested);
    dialog.set_default_response(Some("join"));
    dialog.set_close_response("decline");

    gtkhx_dialog_add_close_shortcuts(dialog.as_ptr() as *mut gtk::ffi::GtkWidget);

    // `htlc` is a stable session-embedded pointer; capture it + cid for the
    // deferred response. AdwAlertDialog fires "response" exactly once (the
    // close response on Esc / dismiss), so there's no separate free bookkeeping
    // — the old C ctx heap struct is gone.
    dialog.connect_response(None, move |_dlg, response| unsafe {
        if response == "join" {
            hx_chat_join(htlc, cid);
        } else {
            hx_reject_chat(htlc, cid);
        }
    });

    let parent_ptr = gtkhx_htlc_chat_window(htlc);
    let parent: Option<gtk::Widget> = if parent_ptr.is_null() {
        None
    } else {
        Some(from_glib_none(parent_ptr))
    };
    dialog.present(parent.as_ref());
}
