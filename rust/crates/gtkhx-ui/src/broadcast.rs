//! Broadcast composer + wire sender (ported from the toolbar.c dialog +
//! msg.c's `hx_send_broadcast`).
//!
//! The server-wide broadcast dialog: an AdwAlertDialog with a single
//! AdwEntryRow, Send (default/suggested) and Cancel responses. Send — or
//! Enter in the entry — encodes the text for the wire and emits
//! HTLC_HDR_MSG_BROADCAST. `gtkhx_broadcast_dialog_open` keeps the C-callback
//! ABI so toolbar.c's Broadcast button connects to it.
//!
//! The whole build flow is native Rust (hotline_proto::build); what stays C
//! behind the FFI seam is the send-path infrastructure: the Mac-Roman/UTF-8
//! wire text encoder (`gtkhx_text_for_wire`, text_util.c), the task table
//! (`task_new`), the write primitive (`hlwrite_chunks`, network.c), and the
//! active-session predicates (gtkhx_ui_bridge.c).

use std::ffi::{c_char, c_void};
use std::os::raw::c_int;

use adw::prelude::*;
use glib::translate::from_glib_none;
use gtk::glib;
use gtk4 as gtk;
use libadwaita as adw;

use hotline_proto::build::{build_broadcast_chunks, BroadcastRequest, HxChunk};
use hotline_proto::messages::ClientHdr;

use crate::tr::tr;

/// `rcv_task_fn`; broadcast registers a no-reply task (fn NULL in the C).
type RcvTaskFn = unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void);

use hxtext::gtkhx_text_for_wire;

extern "C" {
    // tasks.c / network.c — the send primitives (rcv fn NULL = no reply task).
    fn task_new(
        htlc: *mut c_void,
        rcv: Option<RcvTaskFn>,
        ptr: *mut c_void,
        data: *mut c_void,
        str_: *const c_char,
    ) -> *mut c_void;
    fn hlwrite_chunks(htlc: *mut c_void, ty: u32, flag: u32, chunks: *const HxChunk, hc: c_int);

    // gtkhx_ui_bridge.c — active-session accessors.
    fn gtkhx_active_htlc() -> *mut c_void;
    fn gtkhx_active_connected() -> glib::ffi::gboolean;
    fn gtkhx_active_text_encoding() -> glib::ffi::gboolean;

    // gtkutil.c — Ctrl+W / Esc close accelerators on a dialog.
    fn gtkhx_dialog_add_close_shortcuts(dialog: *mut gtk::ffi::GtkWidget);

    // toolbar.c — the toolbar window (dialog parent). May be NULL.
    static toolbar_window: *mut gtk::ffi::GtkWidget;
}

/// Encode + emit a broadcast. Mirrors `msg.c::hx_send_broadcast`: clamp the
/// byte count to the u16 wire limit, walk back to the last complete UTF-8
/// codepoint (so the wire encoder never sees a split sequence), encode for
/// the wire, then build the single BODY chunk + task + write.
fn send_broadcast(text: &str) {
    if text.is_empty() {
        return;
    }
    // Clamp to the u16 HTLC_DATA_MSG length (0xfffe, matching the C caller).
    let mut safe = text.as_bytes();
    if safe.len() > 0xfffe {
        safe = &safe[..0xfffe];
    }
    // Trim a clamp that split a multi-byte sequence to the last valid
    // codepoint boundary (str::from_utf8 gives the valid_up_to offset).
    let safe = match std::str::from_utf8(safe) {
        Ok(_) => safe,
        Err(e) => &safe[..e.valid_up_to()],
    };
    if safe.is_empty() {
        return;
    }

    unsafe {
        // The dialog may have outlived the connection (dropped between open and
        // Send/Enter). If we're no longer connected, hlwrite_chunks would no-op
        // on the unset htlc->fd but task_new would still register a phantom
        // task — so bail before either. Mirrors the connected gate the C send
        // path relies on.
        if gtkhx_active_connected() == glib::ffi::GFALSE {
            return;
        }
        let htlc = gtkhx_active_htlc();
        let utf8_mode = gtkhx_active_text_encoding();

        // Wire text (UTF-8 or Mac Roman); owns a g_malloc'd buffer.
        let mut wire_len: usize = 0;
        let wire = gtkhx_text_for_wire(
            safe.as_ptr() as *const c_char,
            safe.len(),
            utf8_mode,
            glib::ffi::GTRUE, // is_body
            &mut wire_len,
        );
        if wire.is_null() {
            return;
        }

        // Single BODY chunk borrowing the wire buffer (kept alive until the
        // hlwrite_chunks read below, then freed).
        let wire_slice = std::slice::from_raw_parts(wire as *const u8, wire_len);
        let mut chunks = [HxChunk::EMPTY; 1];
        let hc = build_broadcast_chunks(&BroadcastRequest { body: wire_slice }, &mut chunks);
        if hc > 0 {
            // No-reply task (rcv fn NULL) registered before the write — see
            // hx_send_msg: task_new snapshots htlc->trans before hlpack bumps it.
            task_new(
                htlc,
                None,
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                c"broadcast".as_ptr(),
            );
            hlwrite_chunks(
                htlc,
                ClientHdr::MsgBroadcast as u32,
                0,
                chunks.as_ptr(),
                hc as c_int,
            );
        }
        glib::ffi::g_free(wire as *mut c_void);
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
    if gtkhx_active_connected() == glib::ffi::GFALSE {
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
                send_broadcast(entry.text().as_str());
            }
        });
    }
    // AdwEntryRow swallows Enter for its own signal (bypassing the default
    // response) — bridge it to the same send + close.
    {
        let dialog = dialog.downgrade();
        entry.connect_entry_activated(move |entry| {
            send_broadcast(entry.text().as_str());
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
