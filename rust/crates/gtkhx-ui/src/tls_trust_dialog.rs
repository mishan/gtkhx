//! TLS TOFU trust prompt (ported from `src/tls_trust_dialog.c`).
//!
//! The `GSocketClient::accept-certificate` handler (marshaled to the main
//! thread in network.c) must return TRUE/FALSE before the handshake can
//! proceed, but `AdwAlertDialog`'s response API is async — so we spin a
//! nested `GMainLoop` until the user answers, the same synchronous-from-async
//! pattern as `rc4_dialog.rs` and the C original.
//!
//! The C caller is network.c, so this keeps the `hx_tls_trust_dialog_run_sync`
//! C ABI export. The trust-store / fingerprint logic stays in `tls_trust.c`.

use crate::cstr;
use crate::ffi as cffi;
use crate::tr::{tr, tr1, tr_argv};
use gtk4 as gtk;
use libadwaita as adw;

use adw::prelude::*;
use gtk::glib;
use std::os::raw::{c_char, c_int, c_void};
use std::cell::Cell;
use std::rc::Rc;

extern "C" {
    /// tls_trust.c — the resolved known_hosts path (g_malloc'd; free with
    /// g_free). May be NULL.
    fn hx_tls_trust_known_hosts_path() -> *mut c_char;
}

/// Substitute a host (arg 1) + port (arg 2) into a translated msgid. Routes
/// through `tr_argv` so both plain (`%s:%u`) and positional (`%1$s`/`%2$u`)
/// catalog forms — and reordering — work.
fn host_port(msgid: &str, host: &str, port: u16) -> String {
    tr_argv(msgid, &[host, &port.to_string()])
}

/// `gboolean hx_tls_trust_dialog_run_sync(GtkWindow *parent, const char *host,
/// guint16 port, const char *fingerprint, hx_tls_trust_status status)` —
/// present the TOFU prompt, block until the user answers, return TRUE iff they
/// chose to trust.
///
/// # Safety
/// `host` / `fingerprint` are valid C strings; `parent` is NULL or a valid
/// `GtkWindow`. Must be called on the GTK main thread.
#[no_mangle]
pub unsafe extern "C" fn hx_tls_trust_dialog_run_sync(
    parent: *mut cffi::GtkWindow,
    host: *const c_char,
    port: u16,
    fingerprint: *const c_char,
    status: c_int,
) -> glib::ffi::gboolean {
    if host.is_null() || fingerprint.is_null() {
        return glib::ffi::GFALSE;
    }
    crate::ensure_gtk_init();

    let host = cstr(host);
    let fingerprint = cstr(fingerprint);
    let mismatch = status == cffi::HX_TLS_TRUST_MISMATCH;

    let (title, trust_label) = if mismatch {
        (tr("Certificate changed"), tr("_Trust New Certificate"))
    } else {
        (tr("Unknown server certificate"), tr("_Trust and Connect"))
    };

    let mut body = if mismatch {
        host_port(
            "The TLS certificate for %s:%u doesn't match the one you previously \
             trusted. This usually means the server rotated its certificate, but it \
             can also indicate a man-in-the-middle attack. Verify the fingerprint \
             out-of-band with the server operator before accepting.",
            &host,
            port,
        )
    } else {
        let mut s = host_port(
            "You haven't connected to %s:%u over TLS before. GtkHx will pin this \
             certificate so future connections are silent — but only if you trust it \
             now.",
            &host,
            port,
        );
        s.push_str("\n\n");
        s.push_str(&tr1(
            "If %s later presents the same certificate on a different port (for \
             example the file transfer subchannel), it will be accepted silently \
             without another prompt.",
            &host,
        ));
        s
    };
    body.push_str("\n\n");
    body.push_str(&tr1("Fingerprint:\n%s", &fingerprint));

    let kh = hx_tls_trust_known_hosts_path();
    if !kh.is_null() {
        let kh_s = cstr(kh);
        glib::ffi::g_free(kh as *mut c_void);
        body.push_str("\n\n");
        body.push_str(&tr1("Pinned certificates live in %s.", &kh_s));
    }

    let dialog = adw::AlertDialog::new(Some(&title), Some(&body));
    dialog.add_response("cancel", &tr("_Cancel"));
    dialog.add_response("trust", &trust_label);
    // MISMATCH defaults to Cancel (destructive accept); UNKNOWN defaults to
    // Trust. Cancel is the close-response so Esc / X / WM-close reject.
    dialog.set_default_response(Some(if mismatch { "cancel" } else { "trust" }));
    dialog.set_close_response("cancel");
    dialog.set_response_appearance(
        "trust",
        if mismatch {
            adw::ResponseAppearance::Destructive
        } else {
            adw::ResponseAppearance::Suggested
        },
    );

    let accepted = Rc::new(Cell::new(false));
    let main_loop = glib::MainLoop::new(None, false);
    {
        let accepted = accepted.clone();
        let ml = main_loop.clone();
        dialog.connect_response(None, move |_, resp| {
            // Anything that isn't "trust" (cancel / Esc / close) rejects.
            accepted.set(resp == "trust");
            ml.quit();
        });
    }

    let parent_win: Option<gtk::Window> = if parent.is_null() {
        None
    } else {
        Some(glib::translate::from_glib_none(parent))
    };
    dialog.present(parent_win.as_ref().map(|w| w.upcast_ref::<gtk::Widget>()));

    main_loop.run();

    if accepted.get() {
        glib::ffi::GTRUE
    } else {
        glib::ffi::GFALSE
    }
}
