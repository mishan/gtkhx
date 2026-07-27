//! Create-Post composer (ported from `news.c`).
//!
//! The "Post News" window for the flat (1.0/1.2) news board: a modal
//! GtkTextView with a Cancel/Post header bar. Cancel or Esc close it;
//! Post or Ctrl+Enter send the body via the C wire sender `hx_post_news`
//! and close. `create_post_window` keeps its C ABI so the toolbar's Post
//! button links unchanged.
//!
//! The wire sender (`hx_post_news`) stays C — it builds the chunks + drives
//! the task/write path, same seam the voice senders use.

use std::cell::RefCell;
use std::ffi::{c_char, c_void};

use gtk4 as gtk;
use gtk::glib;
use gtk::prelude::*;
use libadwaita as adw;
use glib::translate::from_glib_none;

use crate::ffi as cffi;
use crate::tr::tr;

/// Opaque C `session *` (unused — single-session; the post uses the active
/// htlc, matching the C which took `&sess->htlc`).
type Session = c_void;

thread_local! {
    /// The open Post window + its body view, or None. Mirrors the C
    /// `post_window` / `postprompt` globals + the single-instance guard.
    static POST: RefCell<Option<(gtk::Window, gtk::TextView)>> = const { RefCell::new(None) };
}

use hxhandlers::send::news::hx_post_news;

extern "C" {    /// gtkhx_ui_bridge.c — the focused session's `&htlc` (single-session).
    fn gtkhx_active_htlc() -> *mut c_void;
    /// gtkhx.c — apply the themed `.gtkhx-text` CSS class + font.
    fn gtkhx_apply_text_style(w: *mut gtk::ffi::GtkWidget);
    /// toolbar.c — the toolbar window (transient-for parent). May be NULL.
    static toolbar_window: *mut gtk::ffi::GtkWidget;
}

/// Close + forget the Post window.
fn close_post() {
    if let Some((win, _)) = POST.with(|p| p.borrow_mut().take()) {
        win.destroy();
    }
}

/// Read the body, send it, and close. Mirrors `news.c::post_news`: LF→CR, and
/// a trailing CR is zeroed in place (length unchanged — the historical
/// behaviour), then handed to `hx_post_news`.
fn do_post() {
    let tv = match POST.with(|p| p.borrow().clone()) {
        Some((_, tv)) => tv,
        None => return,
    };
    let buf = tv.buffer();
    let (start, end) = buf.bounds();
    let mut bytes = buf.text(&start, &end, false).to_string().into_bytes();
    let len = bytes.len();
    for b in bytes.iter_mut() {
        if *b == b'\n' {
            *b = b'\r';
        }
    }
    if len > 0 && bytes[len - 1] == b'\r' {
        bytes[len - 1] = 0;
    }
    unsafe {
        hx_post_news(
            gtkhx_active_htlc(),
            bytes.as_ptr() as *const c_char,
            len as u16,
        );
    }
    close_post();
}

/// `void create_post_window(GtkWidget *widget, gpointer data)` — open (or
/// re-present) the Post News composer. Toolbar "Post" button callback.
///
/// # Safety
/// Called on the GTK main thread as a GTK signal callback.
#[no_mangle]
pub unsafe extern "C" fn create_post_window(
    _widget: *mut gtk::ffi::GtkWidget,
    _data: *mut Session,
) {
    crate::ensure_gtk_init();

    // Single instance: re-present rather than stacking a second window.
    if let Some((win, _)) = POST.with(|p| p.borrow().clone()) {
        win.present();
        return;
    }

    let window = gtk::Window::new();
    window.set_title(Some(&tr("Post News")));
    window.set_default_size(540, 380);
    if !toolbar_window.is_null() {
        let parent: gtk::Window = from_glib_none(toolbar_window as *mut gtk::ffi::GtkWindow);
        window.set_transient_for(Some(&parent));
    }
    window.set_modal(true);

    // Header: Cancel (start, plain) + Post (end, suggested). Hide the default
    // window controls — Cancel closes.
    let header = adw::HeaderBar::new();
    header.set_show_start_title_buttons(false);
    header.set_show_end_title_buttons(false);
    let cancel = gtk::Button::with_mnemonic(&tr("_Cancel"));
    cancel.connect_clicked(|_| close_post());
    let post_btn = gtk::Button::with_mnemonic(&tr("_Post"));
    post_btn.add_css_class("suggested-action");
    post_btn.connect_clicked(|_| do_post());
    header.pack_start(&cancel);
    header.pack_end(&post_btn);
    window.set_titlebar(Some(&header));

    // Body: a framed, generously-margined GtkTextView.
    let tv = gtk::TextView::new();
    tv.set_editable(true);
    tv.set_wrap_mode(gtk::WrapMode::Word);
    tv.set_top_margin(8);
    tv.set_bottom_margin(8);
    tv.set_left_margin(8);
    tv.set_right_margin(8);
    gtkhx_apply_text_style(tv.as_ptr() as *mut gtk::ffi::GtkWidget);

    let scroll = gtk::ScrolledWindow::new();
    scroll.set_policy(gtk::PolicyType::Automatic, gtk::PolicyType::Automatic);
    scroll.set_child(Some(&tv));
    scroll.set_hexpand(true);
    scroll.set_vexpand(true);
    scroll.add_css_class("card");

    let content = gtk::Box::new(gtk::Orientation::Vertical, 0);
    content.set_margin_start(12);
    content.set_margin_end(12);
    content.set_margin_top(12);
    content.set_margin_bottom(12);
    content.append(&scroll);
    window.set_child(Some(&content));

    // Esc = Cancel, Ctrl+Enter = Post.
    let kc = gtk::EventControllerKey::new();
    kc.connect_key_pressed(|_, keyval, _, state| {
        if keyval == gtk::gdk::Key::Escape {
            close_post();
            return glib::Propagation::Stop;
        }
        if (keyval == gtk::gdk::Key::Return || keyval == gtk::gdk::Key::KP_Enter)
            && state.contains(gtk::gdk::ModifierType::CONTROL_MASK)
        {
            do_post();
            return glib::Propagation::Stop;
        }
        glib::Propagation::Proceed
    });
    window.add_controller(kc);

    // WM-close (X) clears the single-instance guard.
    window.connect_close_request(|_| {
        POST.with(|p| *p.borrow_mut() = None);
        glib::Propagation::Proceed
    });

    POST.with(|p| *p.borrow_mut() = Some((window.clone(), tv.clone())));

    cffi::init_keyaccel(window.as_ptr() as *mut cffi::GtkWidget);
    window.present();
    tv.grab_focus();
}
