//! About dialog (ported from `src/about.c`).
//!
//! Exports the C ABI entry point `create_about_window` that toolbar.c
//! calls. Keeps the GtkHx-original "logo + custom credits" feel (rather
//! than AdwAboutDialog's rigid slots): a centered logo, version/tagline/
//! copyright labels, and a scrolled monospace credits view.

use crate::ffi as cffi;
use gtk4 as gtk;
use libadwaita as adw;

use adw::prelude::*;
use gtk::gdk;
use gtk::glib;
use std::cell::RefCell;
use std::os::raw::c_void;

thread_local! {
    static ABOUT: RefCell<Option<gtk::Window>> = const { RefCell::new(None) };
}

const CREDITS: &str = "     Main Developer: Misha Nasledov\n\
                       \x20                    <misha@nasledov.com>\n\
                       \n\
                       \x20     hx Developers: Ryan Nielsen\n\
                       \x20                    <ran@krazynet.com>\n\
                       \x20                    David Raufeisen\n\
                       \x20                    <david@fortyoz.org>\n\
                       \n\
                       \x20    Special Thanks: Aaron Lehmann\n\
                       \x20                    apocalypse\n\
                       \x20                    Philip Neustrom\n\
                       \x20                    Jonathan C. Sitte\n\
                       \x20                    Jean-Sebastien Hubert\n\
                       \x20                    Hotline Wiki Discord\n";

/// `void create_about_window(GtkWidget *widget, gpointer data)` — both
/// args unused (menu/toolbar action shape).
#[no_mangle]
pub extern "C" fn create_about_window(_widget: *mut cffi::GtkWidget, _data: *mut c_void) {
    // Already open → just raise it.
    if let Some(win) = ABOUT.with_borrow(|w| w.clone()) {
        win.present();
        return;
    }
    crate::ensure_gtk_init();

    let window = gtk::Window::new();
    window.set_title(Some(&crate::tr::tr("About GtkHx")));
    window.set_resizable(false);
    window.set_default_size(480, 680);
    window.set_titlebar(Some(&adw::HeaderBar::new()));

    // Transient for the active window so it centers over the app.
    let ap = unsafe { cffi::gtkhx_active_window() };
    if !ap.is_null() {
        let parent: gtk::Window = unsafe { glib::translate::from_glib_none(ap) };
        window.set_transient_for(Some(&parent));
    }
    // try_with (not with_borrow_mut): on Ctrl-Q the C hx_quit calls exit(),
    // whose TLS destructors drop this thread-local, disposing the window and
    // firing "destroy" — at which point ABOUT is mid-destruction and a
    // plain access would panic (AccessError) and abort across the FFI.
    window.connect_destroy(|_| {
        let _ = ABOUT.try_with(|c| *c.borrow_mut() = None);
    });

    let bx = gtk::Box::new(gtk::Orientation::Vertical, 8);
    bx.set_margin_top(18);
    bx.set_margin_bottom(18);
    bx.set_margin_start(18);
    bx.set_margin_end(18);

    // Logo — GtkPicture at natural size (the resource is 400x200; GtkImage
    // would shrink it to icon size). can-shrink off pins the real size.
    // The logo is a bundled gresource, so from_resource always succeeds.
    let tex = gdk::Texture::from_resource("/com/nasledov/gtkhx/pixmaps/gtkhx.png");
    let logo = gtk::Picture::for_paintable(&tex);
    logo.set_can_shrink(false);
    logo.set_halign(gtk::Align::Center);
    bx.append(&logo);

    let title = gtk::Label::new(None);
    title.set_markup(&format!(
        "<span size=\"x-large\" weight=\"bold\">GtkHx {}</span>",
        glib::markup_escape_text(cffi::VERSION)
    ));
    title.set_halign(gtk::Align::Center);
    title.set_margin_top(6);
    bx.append(&title);

    let subtitle = gtk::Label::new(Some(&crate::tr::tr("A GTK+ Hotline client")));
    subtitle.add_css_class("dim-label");
    subtitle.set_halign(gtk::Align::Center);
    bx.append(&subtitle);

    let copyright = gtk::Label::new(Some(&crate::tr::tr("Copyright © 2000–2026 Misha Nasledov")));
    copyright.add_css_class("dim-label");
    copyright.set_halign(gtk::Align::Center);
    copyright.set_margin_top(6);
    bx.append(&copyright);

    let scrolled = gtk::ScrolledWindow::new();
    scrolled.set_policy(gtk::PolicyType::Never, gtk::PolicyType::Automatic);
    scrolled.set_size_request(-1, 180);
    scrolled.set_vexpand(true);
    scrolled.set_margin_top(12);

    let credits = gtk::TextView::new();
    credits.set_editable(false);
    credits.set_cursor_visible(false);
    credits.set_monospace(true);
    credits.add_css_class("view");
    credits.buffer().set_text(CREDITS);
    scrolled.set_child(Some(&credits));
    bx.append(&scrolled);

    window.set_child(Some(&bx));
    unsafe { cffi::init_keyaccel_dialog(window.as_ptr() as *mut cffi::GtkWidget) };

    ABOUT.with_borrow_mut(|w| *w = Some(window.clone()));
    window.present();
}
