//! Beside the header bar's title: a spinner while the focused connection is
//! connecting and logging in, then a lock while it is encrypted, with a
//! tooltip naming how, and a fingerprint while it is proved as an
//! identity, with a tooltip naming which.

use std::cell::{Cell, RefCell};
use std::ffi::{c_int, c_void};

use gtk::glib::translate::IntoGlibPtr;
use gtk::prelude::*;
use gtk4 as gtk;
use hxkeys::Proof;
use hxnet::ffi::Encryption;
use libadwaita as adw;

use crate::ensure_gtk_init;
use crate::tr::{tr, tr1, tr_argv};

#[derive(Clone)]
struct Slot {
    slot: gtk::Box,
    spinner: adw::Spinner,
    lock: gtk::Image,
    proved: gtk::Image,
}

thread_local! {
    static SLOT: RefCell<Option<Slot>> = const { RefCell::new(None) };
    /// The connection and status last shown, to show again once its proof
    /// is told.
    static SHOWN: Cell<(*const c_void, c_int)> = const { Cell::new((std::ptr::null(), 0)) };
}

fn describe(e: &Encryption) -> String {
    match e {
        Encryption::Tls(n) => format!(
            "{}\n{}",
            tr1("Encrypted with TLS %s", &n.version),
            tr1("Cipher suite: %s", &n.suite)
        ),
        Encryption::Hope(cipher) => format!(
            "{}\n{}",
            tr("Encrypted with HOPE secure login"),
            tr1("Cipher: %s", cipher)
        ),
    }
}

/// Build the slot, hidden, for toolbar.c to embed beside the title.
///
/// # Safety
/// Must run on the GTK main thread.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_encryption_lock_new() -> *mut gtk::ffi::GtkWidget {
    ensure_gtk_init();
    let spinner = adw::Spinner::new();
    spinner.set_valign(gtk::Align::Center);
    spinner.set_tooltip_text(Some(&tr("Connecting")));
    spinner
        .upcast_ref::<gtk::Widget>()
        .update_property(&[gtk::accessible::Property::Label(&tr("Connecting"))]);
    let lock = gtk::Image::from_icon_name("channel-secure-symbolic");
    let proved = gtk::Image::from_icon_name("auth-fingerprint-symbolic");
    proved.set_visible(false);
    let slot = gtk::Box::new(gtk::Orientation::Horizontal, 0);
    slot.append(&spinner);
    slot.append(&lock);
    slot.append(&proved);
    slot.set_visible(false);
    SLOT.with(|s| {
        *s.borrow_mut() = Some(Slot {
            slot: slot.clone(),
            spinner,
            lock,
            proved,
        })
    });
    slot.upcast::<gtk::Widget>().into_glib_ptr()
}

/// Show the spinner while `status` (set_status_bar's: -1 connecting, 1
/// connected, 2 logged in, 0 not connected) is short of logged in, and the
/// lock for `htlc`'s encryption once it is; neither when it is not connected
/// or runs in the clear.
///
/// # Safety
/// Must run on the GTK main thread; `htlc` is NULL or a live connection.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_encryption_lock_update(htlc: *const c_void, status: c_int) {
    SHOWN.set((htlc, status));
    let connecting = status == -1 || status == 1;
    // Not at "Connected to": the TLS certificate prompt may still be up.
    let (encryption, proof) = if htlc.is_null() || status != 2 {
        (None, None)
    } else {
        read(htlc)
    };
    show(
        connecting,
        encryption.as_ref().map(describe),
        proof.as_ref().and_then(proved_as),
    );
}

/// How `htlc` is encrypted, and how its proof ended.
///
/// # Safety
/// `htlc` is a live connection, on the main thread.
#[cfg(not(test))]
unsafe fn read(htlc: *const c_void) -> (Option<Encryption>, Option<Proof>) {
    use gtkhx_core::conn::hx_conn_bridge_handle;
    use hxnet::ffi::{connection_encryption, connection_proof, HxnetConnection};
    let handle = hx_conn_bridge_handle(htlc.cast()) as *const HxnetConnection;
    (connection_encryption(handle), connection_proof(handle))
}

/// What the test says of `htlc`, which is no connection.
#[cfg(test)]
unsafe fn read(htlc: *const c_void) -> (Option<Encryption>, Option<Proof>) {
    (
        None,
        tests::TOLD.with_borrow(|t| t.get(&(htlc as usize)).cloned()),
    )
}

/// `htlc`'s proof was told: show it, if `htlc` is the connection shown.
pub(crate) fn proof_told(htlc: *const c_void) {
    let (shown, status) = SHOWN.get();
    if !htlc.is_null() && shown == htlc {
        // SAFETY: the signal names a live connection, on the main thread.
        unsafe { gtkhx_encryption_lock_update(htlc, status) };
    }
}

fn proved_as(p: &Proof) -> Option<String> {
    match p {
        Proof::Proved { identity, name, .. } => Some(tr_argv(
            // TRANSLATORS: %1$s is the identity's name, %2$s the start of
            // its fingerprint.
            "Proved as %1$s (%2$s)",
            &[name, &identity.short()],
        )),
        _ => None,
    }
}

fn show(connecting: bool, encryption: Option<String>, proved: Option<String>) {
    SLOT.with(|s| {
        let Some(Slot {
            slot,
            spinner,
            lock,
            proved: badge,
        }) = s.borrow().clone()
        else {
            return;
        };
        for (image, text) in [(&lock, &encryption), (&badge, &proved)] {
            image.set_tooltip_text(text.as_deref());
            image.update_property(&[gtk::accessible::Property::Label(
                text.as_deref().unwrap_or(""),
            )]);
            image.set_visible(text.is_some());
        }
        spinner.set_visible(connecting);
        slot.set_visible(connecting || encryption.is_some() || proved.is_some());
    });
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use gtk::glib::translate::FromGlibPtrFull;
    use hxkeys::Fingerprint;
    use std::collections::HashMap;

    thread_local! {
        /// How each pretend connection's proof ended.
        pub(super) static TOLD: RefCell<HashMap<usize, Proof>> = RefCell::new(HashMap::new());
    }

    /// The connection shown, once logged in and proved as an identity, shows
    /// its fingerprint badge named for the identity, as soon as the proof is
    /// told; another connection's proof, one not logged in, and one not
    /// proved show none.
    pub(crate) fn check_proved_badge() {
        let slot: gtk::Widget = unsafe { gtk::Widget::from_glib_full(gtkhx_encryption_lock_new()) };
        let badge = SLOT.with_borrow(|s| s.as_ref().unwrap().proved.clone());
        let (a, b, c) = (
            0x10 as *const c_void,
            0x20 as *const c_void,
            0x30 as *const c_void,
        );
        let fp = Fingerprint([7; 32]);
        let proved = Proof::Proved {
            identity: fp,
            device: fp,
            name: "alice".into(),
        };

        unsafe { gtkhx_encryption_lock_update(a, 2) };
        assert!(!badge.is_visible(), "not proved yet");
        TOLD.with_borrow_mut(|t| t.insert(b as usize, proved.clone()));
        proof_told(b);
        assert!(!badge.is_visible(), "another connection's proof");
        TOLD.with_borrow_mut(|t| t.insert(a as usize, proved));
        proof_told(a);
        assert!(slot.is_visible() && badge.is_visible(), "told");
        let named = format!("Proved as alice ({})", fp.short());
        assert_eq!(badge.tooltip_text().as_deref(), Some(named.as_str()));

        unsafe { gtkhx_encryption_lock_update(a, 0) };
        assert!(!badge.is_visible(), "not connected");
        unsafe { gtkhx_encryption_lock_update(c, 2) };
        assert!(!badge.is_visible() && !slot.is_visible(), "not proved");
    }
}
