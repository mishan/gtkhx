//! What a preview window shows when it has only part of a file, or none:
//! how much was left out, and a Download for all of it. The window itself is
//! `preview.c`'s.

use std::ffi::c_void;

use gtk::glib;
use gtk::glib::translate::from_glib_none;
use gtk4 as gtk;
use libadwaita as adw;
use libadwaita::prelude::*;

use crate::tr::{tr, tr_argv};

/// Download the whole file; false when that can't be done any more.
type DownloadFn = unsafe extern "C" fn(*mut c_void) -> glib::ffi::gboolean;

/// Fill `window` for a preview that streams `shown` bytes of a `size`-byte
/// file: `body`, the viewer, under a banner saying how much it shows, or, when
/// `body` is NULL, a page saying the file is too large. Download closes the
/// window once it has started.
///
/// # Safety
/// Main thread. `window` is a valid `GtkWindow*`, `body` NULL or a valid
/// `GtkWidget*`, and `data` valid for `download` while the window is open.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_preview_show_part(
    window: *mut gtk::ffi::GtkWindow,
    body: *mut gtk::ffi::GtkWidget,
    shown: u64,
    size: u64,
    download: Option<DownloadFn>,
    data: *mut c_void,
) {
    crate::ensure_gtk_init();
    let window: gtk::Window = from_glib_none(window);
    let body: Option<gtk::Widget> = (!body.is_null()).then(|| from_glib_none(body));

    let start = {
        let window = window.downgrade();
        move || {
            let Some(window) = window.upgrade() else {
                return;
            };
            if download.is_some_and(|f| f(data) != glib::ffi::GFALSE) {
                window.close();
            } else {
                window.error_bell();
            }
        }
    };
    let size_str = iec(size);

    let Some(body) = body else {
        let button = gtk::Button::builder()
            .label(tr("Download"))
            .halign(gtk::Align::Center)
            .css_classes(["pill", "suggested-action"])
            .build();
        button.connect_clicked(move |_| start());
        let page = adw::StatusPage::builder()
            .title(tr("Too Large to Preview"))
            // TRANSLATORS: %s is the file's size, such as "700.0 MiB".
            .description(tr_argv(
                "At %s, this file is too large to preview.",
                &[&size_str],
            ))
            .child(&button)
            .build();
        window.set_child(Some(&page));
        return;
    };

    // TRANSLATORS: The preview shows the start of a long file. The first %s
    // is how much it shows, the second the whole file's size.
    let text = tr_argv("Showing the first %s of %s", &[&iec(shown), &size_str]);
    let banner = adw::Banner::builder()
        .title(text)
        .button_label(tr("Download"))
        .revealed(true)
        .build();
    banner.connect_button_clicked(move |_| start());
    body.set_vexpand(true);
    let column = gtk::Box::new(gtk::Orientation::Vertical, 0);
    column.append(&banner);
    column.append(&body);
    window.set_child(Some(&column));
}

/// A size as the file browser shows it.
fn iec(size: u64) -> glib::GString {
    glib::format_size_full(size, glib::FormatSizeFlags::IEC_UNITS)
}
