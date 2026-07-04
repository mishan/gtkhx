//! User Info result window (ported from `gtkhx.c::output_user_info`).
//!
//! The "Get User Info" reply: a small read-only text window showing the
//! server's profile blurb for a user. Fired from the `user-info`
//! GtkhxSession signal — the `on_user_info_signal` adapter in gtkhx.c calls
//! this `#[no_mangle]` export, so the C side links unchanged.

use std::ffi::c_char;

use gtk4 as gtk;
use gtk::prelude::*;

use crate::ffi as cffi;
use crate::tr::tr_argv;

/// `void output_user_info(guint16 uid, const char *nam, const char *info,
/// guint16 len)` — present the read-only user-info window. No-op for an empty
/// blurb (matches the C `len > 0` guard).
///
/// # Safety
/// `nam` is NULL or a valid C string; `info` is NULL or valid for `len`
/// bytes. Main thread only.
#[no_mangle]
pub unsafe extern "C" fn output_user_info(
    uid: u16,
    nam: *const c_char,
    info: *const c_char,
    len: u16,
) {
    if len == 0 {
        return;
    }
    crate::ensure_gtk_init();

    let window = gtk::Window::new();
    window.set_size_request(260, 250);
    let name = crate::cstr(nam);
    window.set_title(Some(&tr_argv(
        "User Info: %1$s (%2$u)",
        &[&name, &uid.to_string()],
    )));

    let text_view = gtk::TextView::new();
    text_view.set_editable(false);
    text_view.set_cursor_visible(false);
    // `info` is `len` raw Hotline-text bytes; the C set the buffer straight
    // from them. Render lossily as UTF-8 so a non-UTF-8 blurb can't trip a
    // GtkTextBuffer validity critical.
    let bytes = if info.is_null() {
        &[][..]
    } else {
        std::slice::from_raw_parts(info as *const u8, len as usize)
    };
    text_view.buffer().set_text(&String::from_utf8_lossy(bytes));

    let scroll = gtk::ScrolledWindow::new();
    scroll.set_policy(gtk::PolicyType::Automatic, gtk::PolicyType::Automatic);
    scroll.set_child(Some(&text_view));
    window.set_child(Some(&scroll));

    // GtkWindow* → GtkWidget* (same object); the C helper adds the Esc-close
    // accelerator. GTK keeps the mapped toplevel alive after present(), so the
    // Rust wrapper dropping here doesn't destroy it (same as agreement.rs).
    cffi::init_keyaccel(window.as_ptr() as *mut cffi::GtkWidget);
    window.present();
}
