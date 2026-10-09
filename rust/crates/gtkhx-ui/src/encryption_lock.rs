//! The lock beside the header bar's title: shown while the focused
//! connection is encrypted, with a tooltip naming how.

use std::cell::RefCell;
use std::ffi::c_void;

use gtk::glib::translate::IntoGlibPtr;
use gtk::prelude::*;
use gtk4 as gtk;
use gtkhx_core::conn::hx_conn_bridge_handle;
use hxnet::ffi::{connection_encryption, Encryption, HxnetConnection};

use crate::ensure_gtk_init;
use crate::tr::{tr, tr1};

thread_local! {
    static LOCK: RefCell<Option<gtk::Image>> = const { RefCell::new(None) };
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

/// Build the lock, hidden, for toolbar.c to embed beside the title.
///
/// # Safety
/// Must run on the GTK main thread.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_encryption_lock_new() -> *mut gtk::ffi::GtkWidget {
    ensure_gtk_init();
    let lock = gtk::Image::from_icon_name("channel-secure-symbolic");
    lock.set_visible(false);
    LOCK.with(|l| *l.borrow_mut() = Some(lock.clone()));
    lock.upcast::<gtk::Widget>().into_glib_ptr()
}

/// Show the lock for `htlc`'s encryption, or hide it when `htlc` is NULL or
/// its connection runs in the clear.
///
/// # Safety
/// Must run on the GTK main thread; `htlc` is NULL or a live connection.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_encryption_lock_update(htlc: *const c_void) {
    let encryption = if htlc.is_null() {
        None
    } else {
        connection_encryption(hx_conn_bridge_handle(htlc.cast()) as *const HxnetConnection)
    };
    LOCK.with(|l| {
        let Some(lock) = l.borrow().clone() else {
            return;
        };
        let text = encryption.as_ref().map(describe);
        lock.set_tooltip_text(text.as_deref());
        lock.update_property(&[gtk::accessible::Property::Label(
            text.as_deref().unwrap_or(""),
        )]);
        lock.set_visible(text.is_some());
    });
}
