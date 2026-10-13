//! Beside the header bar's title: a spinner while the focused connection is
//! connecting and logging in, then a lock while it is encrypted, with a
//! tooltip naming how.

use std::cell::RefCell;
use std::ffi::{c_int, c_void};

use gtk::glib::translate::IntoGlibPtr;
use gtk::prelude::*;
use gtk4 as gtk;
use gtkhx_core::conn::hx_conn_bridge_handle;
use hxnet::ffi::{connection_encryption, Encryption, HxnetConnection};
use libadwaita as adw;

use crate::ensure_gtk_init;
use crate::tr::{tr, tr1};

#[derive(Clone)]
struct Slot {
    slot: gtk::Box,
    spinner: adw::Spinner,
    lock: gtk::Image,
}

thread_local! {
    static SLOT: RefCell<Option<Slot>> = const { RefCell::new(None) };
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
    let slot = gtk::Box::new(gtk::Orientation::Horizontal, 0);
    slot.append(&spinner);
    slot.append(&lock);
    slot.set_visible(false);
    SLOT.with(|s| {
        *s.borrow_mut() = Some(Slot {
            slot: slot.clone(),
            spinner,
            lock,
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
    let connecting = status == -1 || status == 1;
    // Not at "Connected to": the TLS certificate prompt may still be up.
    let encryption = if htlc.is_null() || status != 2 {
        None
    } else {
        connection_encryption(hx_conn_bridge_handle(htlc.cast()) as *const HxnetConnection)
    };
    SLOT.with(|s| {
        let Some(Slot {
            slot,
            spinner,
            lock,
        }) = s.borrow().clone()
        else {
            return;
        };
        let text = encryption.as_ref().map(describe);
        lock.set_tooltip_text(text.as_deref());
        lock.update_property(&[gtk::accessible::Property::Label(
            text.as_deref().unwrap_or(""),
        )]);
        lock.set_visible(text.is_some());
        spinner.set_visible(connecting);
        slot.set_visible(connecting || text.is_some());
    });
}
