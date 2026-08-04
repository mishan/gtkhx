//! Legacy-RC4 bookmark replacement picker (ported from
//! `src/bookmark_rc4_dialog.c`).
//!
//! Shown when a bookmark still carries the retired RC4 cipher byte. The
//! AdwAlertDialog response API is async, but the callers (the Connect
//! dialog's bookmark-open path and the Bookmarks manager) need a
//! synchronous answer to decide whether to proceed with the connection or
//! abandon it — so we spin a nested `GMainLoop` until the response fires,
//! the same synchronous-from-async pattern the C original (and
//! `tls_trust_dialog.c`) used.
//!
//! Both callers are Rust, so this is a plain `pub` fn — no C ABI export.

use crate::ffi as cffi;
use crate::tr::{tr, tr1};
use gtk4 as gtk;
use hxbookmarks::cipher;
use libadwaita as adw;

use adw::prelude::*;
use gtk::glib;
use std::cell::Cell;
use std::rc::Rc;

/// Present the RC4-replacement picker over `parent` (may be NULL), block
/// until the user answers, persist the choice to the named bookmark, and
/// return the chosen stable cipher byte — or -1 on cancel / Esc / close.
/// An empty `name` means "no on-disk bookmark to name or rewrite" (the
/// last-connection-derived caller).
///
/// # Safety
/// `parent` must be either NULL or a valid pointer to a live
/// `GtkWindow`; it is borrowed (not consumed) for the lifetime of the
/// dialog and is only dereferenced when non-NULL.
pub unsafe fn run_sync(parent: *mut cffi::GtkWindow, name: &str) -> i32 {
    crate::ensure_gtk_init();

    // Body text: reuse the exact C msgids (including the `%s`) so the
    // existing translation catalog still resolves, then substitute the
    // bookmark name in ourselves.
    let mut body = if name.is_empty() {
        tr(
            "This connection was saved with RC4 as its HOPE cipher. GtkHx no longer \
            offers RC4 — it's a known-broken stream cipher and the \"Secure\" label \
            it used to wear was misleading.",
        )
    } else {
        tr1(
            "The bookmark \"%s\" was saved with RC4 as its HOPE cipher. GtkHx no longer \
             offers RC4 — it's a known-broken stream cipher and the \"Secure\" label \
             it used to wear was misleading.",
            name,
        )
    };
    body.push_str("\n\n");
    // Two things this paragraph used to get wrong, both of which every
    // translator hit: it named an option called "No cipher" that appears
    // nowhere (the button below says "Connect without encryption", and the
    // combo's first row says "Off"), and its last clause — "change the
    // bookmark's cipher from the connection's cipher" — was garbled enough
    // that translations had to guess at the intent.
    body.push_str(&tr(
        "Pick a replacement cipher and the bookmark will be rewritten so this prompt \
         doesn't appear again. \"Connect without encryption\" sends the connection in \
         plaintext — that's less secure than Blowfish or ChaCha20-Poly1305, but at \
         least you'll know the connection isn't protected. The server has to support \
         whichever cipher you pick; if the negotiation fails, change the connection's \
         cipher in Settings → Connections and try again.",
    ));

    let dialog = adw::AlertDialog::new(Some(&tr("Replace RC4 cipher")), Some(&body));

    // Order matches the "increasingly cautious" ranking: ChaCha20 → Blowfish
    // → None → Cancel. ChaCha20 (strongest) is the suggested default; Cancel
    // is the close-response so Esc / X / WM-close land there.
    dialog.add_response("chacha20", &tr("Use _ChaCha20-Poly1305"));
    dialog.add_response("blowfish", &tr("Use _Blowfish"));
    dialog.add_response("none", &tr("Connect without _encryption"));
    dialog.add_response("cancel", &tr("Ca_ncel"));
    dialog.set_default_response(Some("chacha20"));
    dialog.set_close_response("cancel");
    dialog.set_response_appearance("chacha20", adw::ResponseAppearance::Suggested);
    // "Connect without encryption" gets the destructive tint so it isn't
    // picked by reflex.
    dialog.set_response_appearance("none", adw::ResponseAppearance::Destructive);

    let result = Rc::new(Cell::new(-1i32));
    let main_loop = glib::MainLoop::new(None, false);
    {
        let result = result.clone();
        let ml = main_loop.clone();
        dialog.connect_response(None, move |_, resp| {
            result.set(match resp {
                "none" => cipher::NONE as i32,
                "blowfish" => cipher::BLOWFISH as i32,
                "chacha20" => cipher::CHACHA20_POLY1305 as i32,
                // "cancel", Esc, X-close, WM-close — anything that isn't a
                // positive selection — folds to cancel.
                _ => -1,
            });
            ml.quit();
        });
    }

    let parent_win: Option<gtk::Window> = if parent.is_null() {
        None
    } else {
        Some(unsafe { glib::translate::from_glib_none(parent) })
    };
    dialog.present(parent_win.as_ref().map(|w| w.upcast_ref::<gtk::Widget>()));

    // Spin until the response handler quits the loop.
    main_loop.run();

    let chosen = result.get();
    persist_choice(name, chosen);
    chosen
}

/// Best-effort: write the chosen cipher byte back to the bookmark in the
/// store so a subsequent open doesn't re-prompt. Silent no-op on empty name /
/// cancel / a name that isn't in the store — there's nothing actionable
/// mid-connection.
fn persist_choice(name: &str, chosen: i32) {
    if name.is_empty() || chosen < 0 {
        return;
    }
    let mut store = crate::bookmark_store::load();
    if let Some(bm) = store.bookmarks.iter_mut().find(|b| b.name == name) {
        bm.cipher = chosen as u8;
        let _ = crate::bookmark_store::save(&store);
    }
}
