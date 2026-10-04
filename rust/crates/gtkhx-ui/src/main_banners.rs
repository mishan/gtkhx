//! The notices across the top of the main window, stacked in one top bar.

use gtk4 as gtk;

use gtk::prelude::*;
use std::cell::OnceCell;

thread_local! {
    static BANNERS: OnceCell<gtk::Box> = const { OnceCell::new() };
}

/// The box of banners for the main window's top bars, built on the first
/// call and the same one after. Each hides itself while it has nothing to say.
///
/// Transfer none: this module keeps the owning reference, without which the
/// box would be destroyed as this function returned.
///
/// # Safety
/// GTK main thread.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_main_banners() -> *mut gtk::ffi::GtkWidget {
    crate::ensure_gtk_init();
    BANNERS.with(|cell| {
        let banners = cell.get_or_init(|| {
            let banners = gtk::Box::new(gtk::Orientation::Vertical, 0);
            banners.append(&crate::updates::banner());
            #[cfg(feature = "voice")]
            banners.append(&crate::screen_share::banner());
            banners
        });
        banners.as_ptr() as *mut gtk::ffi::GtkWidget
    })
}
