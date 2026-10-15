//! Create-Post composer (ported from `news.c`).
//!
//! The "Post News" window for the flat (1.0/1.2) news board: a modal
//! GtkTextView with a Cancel/Post header bar. Cancel or Esc close it;
//! Post or Ctrl+Enter send the body via `hx_post_news` and close.
//!
//! One per connection, posting to the board it was opened from: the focus
//! can move to another tab while it is open (a second launch's `--server`,
//! a tracker connect), and the post still belongs to the first. It closes
//! with its connection's tab.

use std::cell::RefCell;
use std::collections::HashMap;
use std::ffi::{c_char, c_void};

use glib::translate::from_glib_none;
use gtk::glib;
use gtk::prelude::*;
use gtk4 as gtk;
use libadwaita as adw;

use crate::dock::{self, Bound, ConnKey};
use crate::ffi as cffi;
use crate::tr::tr;

/// Opaque C `session *`.
type Session = c_void;

thread_local! {
    /// Each connection's open Post window and its body view.
    static POST: RefCell<HashMap<ConnKey, (gtk::Window, gtk::TextView)>> =
        RefCell::new(HashMap::new());
}

use hxhandlers::send::news::hx_post_news;

extern "C" {
    /// gtkhx.c — apply the themed `.gtkhx-text` CSS class + font.
    fn gtkhx_apply_text_style(w: *mut gtk::ffi::GtkWidget);
    /// toolbar.c — the toolbar window (transient-for parent). May be NULL.
    static toolbar_window: *mut gtk::ffi::GtkWidget;
}

/// Close `conn`'s Post window; its close handler forgets it.
fn close_post(conn: ConnKey) {
    if let Some((win, _)) = POST.with_borrow(|p| p.get(&conn).cloned()) {
        win.close();
    }
}

/// Read the body, send it, and close. Mirrors `news.c::post_news`: LF→CR, and
/// a trailing CR is zeroed in place (length unchanged — the historical
/// behaviour), then handed to `hx_post_news`.
fn do_post(conn: Bound) {
    let Some((_, tv)) = POST.with_borrow(|p| p.get(&conn.conn()).cloned()) else {
        return;
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
    // Not once the connection has gone, or connected to another server; the
    // window stays, with the draft, rather than throwing it away.
    let Some(htlc) = dock::live_htlc(conn) else {
        let text = crate::cs(&tr("Not posted: the connection this was for has closed"));
        unsafe { cffi::toolbar_show_toast(text.as_ptr()) };
        return;
    };
    unsafe { hx_post_news(htlc, bytes.as_ptr() as *const c_char, len as u16) };
    close_post(conn.conn());
}

/// `void create_post_window(GtkWidget *widget, gpointer data)` — open (or
/// re-present) `data`'s Post News composer.
///
/// # Safety
/// Called on the GTK main thread as a GTK signal callback; `data` is a
/// `session *`.
#[no_mangle]
pub unsafe extern "C" fn create_post_window(_widget: *mut gtk::ffi::GtkWidget, data: *mut Session) {
    crate::ensure_gtk_init();

    let conn = dock::bind(dock::key_for_session(data));
    let key = conn.conn();
    // One per connection: re-present rather than stacking a second window.
    if let Some((win, _)) = POST.with_borrow(|p| p.get(&key).cloned()) {
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
    cancel.connect_clicked(move |_| close_post(key));
    let post_btn = gtk::Button::with_mnemonic(&tr("_Post"));
    post_btn.add_css_class("suggested-action");
    post_btn.connect_clicked(move |_| do_post(conn));
    header.pack_start(&cancel);
    header.pack_end(&post_btn);
    window.set_titlebar(Some(&header));

    // Body: a framed, generously-margined GtkTextView.
    let tv = gtk::TextView::new();
    tv.update_property(&[gtk::accessible::Property::Label(&tr("Message"))]);
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
    kc.connect_key_pressed(move |_, keyval, _, state| {
        if keyval == gtk::gdk::Key::Escape {
            close_post(key);
            return glib::Propagation::Stop;
        }
        if (keyval == gtk::gdk::Key::Return || keyval == gtk::gdk::Key::KP_Enter)
            && state.contains(gtk::gdk::ModifierType::CONTROL_MASK)
        {
            do_post(conn);
            return glib::Propagation::Stop;
        }
        glib::Propagation::Proceed
    });
    window.add_controller(kc);

    // Every way it closes — Cancel, Post, the window's own close, or its
    // connection's tab — comes through here.
    window.connect_close_request(move |_| {
        POST.with_borrow_mut(|p| p.remove(&key));
        glib::Propagation::Proceed
    });

    POST.with_borrow_mut(|p| p.insert(key, (window.clone(), tv.clone())));
    dock::close_with(key, &window);

    cffi::init_keyaccel(window.as_ptr() as *mut cffi::GtkWidget);
    window.present();
    tv.grab_focus();
}
