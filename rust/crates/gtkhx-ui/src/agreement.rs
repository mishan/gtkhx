//! Server agreement window (ported from the `output_agreement` /
//! `concurrence` / `disagreement` block that lived in `src/gtkhx.c`).
//!
//! Exported as `gtkhx_show_agreement`, called by gtkhx.c's
//! `on_agreement_signal` adapter when the `GtkhxSession::agreement`
//! signal fires. The window is stored back into `sess->agreementwin`
//! (via the C bridge) so the disconnect-cleanup path in gtkutil.c can
//! tear it down; Agree sends AGREEMENTAGREE and closes it, Disagree
//! drops the connection (whose disconnect cleanup then closes it).

use crate::ffi as cffi;
use crate::tr::tr;
use gtk4 as gtk;
use libadwaita as adw;

use adw::prelude::*;
use std::ffi::c_char;
use std::os::raw::c_void;

extern "C" {
    // gtkhx_ui_bridge.c — session-scoped agreement actions + window store.
    fn gtkhx_agreement_agree(sess: *mut c_void);
    fn gtkhx_agreement_disagree(sess: *mut c_void);
    fn gtkhx_session_set_agreementwin(sess: *mut c_void, win: *mut cffi::GtkWidget);
}

/// `void gtkhx_show_agreement(session *sess, const char *agreement,
/// guint16 len)`.
///
/// # Safety
/// `agreement` points to `len` bytes; `sess` is a valid `session *`.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_show_agreement(
    sess: *mut c_void,
    agreement: *const c_char,
    len: u16,
) {
    crate::ensure_gtk_init();

    let window = gtk::Window::new();
    window.set_titlebar(Some(&adw::HeaderBar::new()));
    window.set_default_size(460, 540);
    window.set_title(Some(&tr("Agreement")));

    let textview = gtk::TextView::new();
    textview.set_monospace(true);
    textview.set_editable(false);
    textview.set_cursor_visible(false);
    textview.set_wrap_mode(gtk::WrapMode::Word);
    // Follow the active GtkHx theme (same as chat output).
    cffi::gtkhx_apply_text_style(textview.as_ptr() as *mut cffi::GtkWidget);
    textview.set_left_margin(12);
    textview.set_right_margin(12);
    textview.set_top_margin(12);
    textview.set_bottom_margin(12);
    let text = if agreement.is_null() || len == 0 {
        String::new()
    } else {
        let bytes = std::slice::from_raw_parts(agreement as *const u8, len as usize);
        String::from_utf8_lossy(bytes).into_owned()
    };
    textview.buffer().set_text(&text);

    let scroll = gtk::ScrolledWindow::new();
    scroll.set_policy(gtk::PolicyType::Automatic, gtk::PolicyType::Automatic);
    scroll.set_vexpand(true);
    scroll.set_child(Some(&textview));

    // Adwaita action-class pill buttons: Agree accent, Disagree red.
    let agreebtn = gtk::Button::with_label(&tr("Agree"));
    agreebtn.add_css_class("suggested-action");
    agreebtn.add_css_class("pill");
    let disagreebtn = gtk::Button::with_label(&tr("Disagree"));
    disagreebtn.add_css_class("destructive-action");
    disagreebtn.add_css_class("pill");

    {
        // sess is a `*mut c_void` (Copy); the handlers run on the main
        // thread so no Send is required and we can capture it directly
        // (no usize round-trip that would lose pointer provenance).
        let win = window.clone();
        agreebtn.connect_clicked(move |_| {
            unsafe {
                gtkhx_agreement_agree(sess);
                gtkhx_session_set_agreementwin(sess, std::ptr::null_mut());
            }
            win.destroy();
        });
    }
    // Disagree drops the connection; its disconnect cleanup (gtkutil.c)
    // closes this window via sess->agreementwin.
    disagreebtn.connect_clicked(move |_| unsafe { gtkhx_agreement_disagree(sess) });

    // Clear sess->agreementwin whenever the window goes away — via Agree,
    // via disconnect cleanup, or via the user closing it directly (window
    // controls / Esc). Without this, a manual close would leave
    // sess->agreementwin dangling at a freed GtkWindow and the later
    // disconnect cleanup (gtkutil.c: destroy-then-NULL) would type-check and
    // destroy a stale pointer. Idempotent with the Agree/cleanup NULL-sets.
    // sess is the process-lifetime session singleton, so it's safe to touch
    // from the destroy handler (including at exit); no thread-local here, so
    // no AccessError concern like the About / User Editor windows have.
    window.connect_destroy(move |_| unsafe {
        gtkhx_session_set_agreementwin(sess, std::ptr::null_mut());
    });

    let vbox = gtk::Box::new(gtk::Orientation::Vertical, 0);
    let hbox = gtk::Box::new(gtk::Orientation::Horizontal, 12);
    hbox.set_halign(gtk::Align::End);
    hbox.set_margin_start(12);
    hbox.set_margin_end(12);
    hbox.set_margin_top(12);
    hbox.set_margin_bottom(12);
    // Disagree on the left, Agree on the right (affirmative where Enter is).
    hbox.append(&disagreebtn);
    hbox.append(&agreebtn);
    vbox.append(&scroll);
    vbox.append(&hbox);
    window.set_child(Some(&vbox));

    // Enter activates Agree (the read-only text view doesn't consume it).
    agreebtn.set_receives_default(true);
    window.set_default_widget(Some(&agreebtn));

    cffi::init_keyaccel(window.as_ptr() as *mut cffi::GtkWidget);
    // Hand the window to the session so disconnect cleanup can close it.
    gtkhx_session_set_agreementwin(sess, window.as_ptr() as *mut cffi::GtkWidget);
    window.present();
}
