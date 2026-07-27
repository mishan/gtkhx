//! TLS TOFU trust prompt (ported from `src/tls_trust_dialog.c`).
//!
//! The trust brain (classify / seams / path / decide / pin) lives in the
//! `hxtls-trust` crate; this module is only the Adwaita prompt + the worker→main
//! marshalling. `hxtls-trust`'s `decide` runs on the hxnet (tokio) verify
//! thread and, when it needs to ask the user, calls the callback we install via
//! [`gtkhx_tls_prompt_install`]. That callback ([`prompt_trampoline`]) hops to
//! the GTK main thread and spins a nested `GMainLoop` until the user answers
//! (the same synchronous-from-async pattern as `rc4_dialog.rs`), since the
//! verify path must return trust/reject synchronously.

use crate::cstr;
use crate::tr::{tr, tr1, tr_argv};
use gtk4 as gtk;
use libadwaita as adw;

use adw::prelude::*;
use gtk::glib;
use std::cell::Cell;
use std::os::raw::{c_char, c_int};
use std::rc::Rc;

/// The MISMATCH status the dialog's tone branches on. Matches the
/// `hxtls-trust` `TrustStatus` discriminant (Trusted=0 / Unknown=1 /
/// Mismatch=2), passed through as an `int` by the registered prompt callback.
const STATUS_MISMATCH: c_int = 2;

use hxtls_trust::ffi::hx_tls_trust_set_prompt;

/// The callback signature `hxtls-trust` calls. See its `PromptFn`.
type PromptFn = extern "C" fn(
    host: *const c_char,
    port: u16,
    fingerprint: *const c_char,
    status: c_int,
    known_hosts: *const c_char,
) -> c_int;

/// `void gtkhx_tls_prompt_install (void)` — register the Adwaita TOFU prompt
/// with `hxtls-trust`. Called once from C at UI init (before any connect).
#[no_mangle]
pub extern "C" fn gtkhx_tls_prompt_install() {
    unsafe { hx_tls_trust_set_prompt(Some(prompt_trampoline)) };
}

/// Called by `hxtls-trust::decide` on the verify (worker) thread. Marshal the
/// dialog to the GTK main thread and block until answered. `MainContext::invoke`
/// runs the closure directly when we're already on the main thread and queues it
/// otherwise; the unbounded channel + `recv` covers both without deadlock.
extern "C" fn prompt_trampoline(
    host: *const c_char,
    port: u16,
    fingerprint: *const c_char,
    status: c_int,
    known_hosts: *const c_char,
) -> c_int {
    let host = unsafe { cstr(host) };
    let fingerprint = unsafe { cstr(fingerprint) };
    let known_hosts = if known_hosts.is_null() {
        None
    } else {
        Some(unsafe { cstr(known_hosts) })
    };
    let mismatch = status == STATUS_MISMATCH;

    let (tx, rx) = std::sync::mpsc::channel::<bool>();
    glib::MainContext::default().invoke(move || {
        let accepted = run_dialog(&host, port, &fingerprint, mismatch, known_hosts.as_deref());
        let _ = tx.send(accepted);
    });
    c_int::from(rx.recv().unwrap_or(false))
}

/// Substitute a host (arg 1) + port (arg 2) into a translated msgid. Routes
/// through `tr_argv` so both plain (`%s:%u`) and positional (`%1$s`/`%2$u`)
/// catalog forms — and reordering — work.
fn host_port(msgid: &str, host: &str, port: u16) -> String {
    tr_argv(msgid, &[host, &port.to_string()])
}

/// Present the TOFU prompt, block until the user answers, return `true` iff they
/// chose to trust. Runs on the GTK main thread; parents to the app's active
/// window when there is one.
fn run_dialog(
    host: &str,
    port: u16,
    fingerprint: &str,
    mismatch: bool,
    known_hosts: Option<&str>,
) -> bool {
    crate::ensure_gtk_init();

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
            host,
            port,
        )
    } else {
        let mut s = host_port(
            "You haven't connected to %s:%u over TLS before. GtkHx will pin this \
             certificate so future connections are silent — but only if you trust it \
             now.",
            host,
            port,
        );
        s.push_str("\n\n");
        s.push_str(&tr1(
            "If %s later presents the same certificate on a different port (for \
             example the file transfer subchannel), it will be accepted silently \
             without another prompt.",
            host,
        ));
        s
    };
    body.push_str("\n\n");
    body.push_str(&tr1("Fingerprint:\n%s", fingerprint));

    if let Some(kh) = known_hosts {
        body.push_str("\n\n");
        body.push_str(&tr1("Pinned certificates live in %s.", kh));
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

    let parent_win: Option<gtk::Window> = gtk::gio::Application::default()
        .and_downcast::<gtk::Application>()
        .and_then(|app| app.active_window());
    dialog.present(parent_win.as_ref().map(|w| w.upcast_ref::<gtk::Widget>()));

    main_loop.run();

    accepted.get()
}
