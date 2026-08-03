//! Private Message window — Phase R5.14 gtk4-rs content port.
//!
//! A PM is a tab inside the Chat panel's `AdwTabView`. `create_msgwin` builds
//! the tab's content tree here in gtk4-rs — the output frame (the C xtext +
//! its scrollbar), the auto-grow input with its emoji button, and the
//! recipient info pane — then adds the tab via `chat_tabs.c`.
//!
//! The genuinely-C leaves stay C behind FFI: `create_msg` makes the model +
//! the xtext output / input widgets; the emoji button + typeahead, the info
//! pane repopulation (`msgwin_refresh_user_info`), the tab management
//! (`chat_tabs.c`), and the keyaccel controllers are C helpers. The layout
//! assembly + the info-pane widgets are Rust; the `msgwin` struct stays C and
//! Rust reads/writes its widget slots through small accessors/setters.

use std::ffi::{c_char, c_void};

use glib::translate::from_glib_none;
use gtk::glib;
use gtk::pango;
use gtk::prelude::*;
use gtk4 as gtk;

/// Opaque C `struct msgwin *`.
type Msgwin = c_void;

extern "C" {
    fn create_msg(sess: *mut c_void, uid: u16, name: *mut c_char) -> *mut Msgwin;
    fn hx_msgwin_outputbuf(msg: *mut Msgwin) -> *mut gtk::ffi::GtkWidget;
    fn hx_msgwin_vscroll(msg: *mut Msgwin) -> *mut gtk::ffi::GtkWidget;
    fn hx_msgwin_inputbuf(msg: *mut Msgwin) -> *mut gtk::ffi::GtkWidget;
    fn hx_msgwin_set_window(msg: *mut Msgwin, w: *mut gtk::ffi::GtkWidget);
    fn hx_msgwin_set_info_image(msg: *mut Msgwin, w: *mut gtk::ffi::GtkWidget);
    fn hx_msgwin_set_info_label(msg: *mut Msgwin, w: *mut gtk::ffi::GtkWidget);

    fn hx_emoji_button_new(target: *mut gtk::ffi::GtkWidget) -> *mut gtk::ffi::GtkWidget;
    fn hx_emoji_typeahead_attach(target: *mut gtk::ffi::GtkWidget);
    fn msgwin_refresh_user_info(msg: *mut Msgwin);
    fn gtkhx_chat_tabs_add_msg(
        content: *mut gtk::ffi::GtkWidget,
        uid: u16,
        title: *const c_char,
    ) -> *mut c_void;
    fn gtkhx_chat_tabs_raise_msg(uid: u16);
    fn init_keyaccel(widget: *mut gtk::ffi::GtkWidget);
}

/// `*mut GtkWidget` for a gtk-rs widget.
fn wptr<W: IsA<gtk::Widget>>(w: &W) -> *mut gtk::ffi::GtkWidget {
    w.as_ref().as_ptr()
}

/// Build a Private Message tab. C ABI replacement for the old
/// `msg.c::create_msgwin`; returns the `struct msgwin *` (from `create_msg`).
///
/// `sess` picks which session's PM-window table the new window is filed in.
/// A uid is only unique within a connection, so it cannot be the whole key.
///
/// # Safety
/// `name` is a valid C string; called on the GTK main thread.
#[no_mangle]
pub unsafe extern "C" fn create_msgwin(
    sess: *mut c_void,
    uid: u16,
    name: *mut c_char,
) -> *mut c_void {
    crate::ensure_gtk_init();

    let msg = create_msg(sess, uid, name);
    if msg.is_null() {
        return std::ptr::null_mut();
    }

    // Leaf widgets built by create_msg (still floating; append/set_child sink
    // them). from_glib_none refs; the append below claims the sink and the
    // wrapper's drop releases our ref — net identical to the old C packing.
    let output: gtk::Widget = from_glib_none(hx_msgwin_outputbuf(msg));
    let vscroll: gtk::Widget = from_glib_none(hx_msgwin_vscroll(msg));
    let input: gtk::Widget = from_glib_none(hx_msgwin_inputbuf(msg));

    // Output frame: xtext + its scrollbar.
    let hbox = gtk::Box::new(gtk::Orientation::Horizontal, 0);
    output.set_hexpand(true);
    output.set_vexpand(true);
    hbox.append(&output);
    hbox.append(&vscroll);
    let outputframe = gtk::Frame::new(None);
    outputframe.set_child(Some(&hbox));

    // Auto-grow input (1-line min, 5-line max) + emoji button alongside.
    let input_scroll = gtk::ScrolledWindow::new();
    input_scroll.set_policy(gtk::PolicyType::Never, gtk::PolicyType::Automatic);
    input_scroll.set_overflow(gtk::Overflow::Visible);
    input_scroll.set_propagate_natural_height(true);
    input_scroll.set_min_content_height(28);
    input_scroll.set_max_content_height(120);
    input_scroll.set_child(Some(&input));

    let input_hbox = gtk::Box::new(gtk::Orientation::Horizontal, 0);
    input_scroll.set_hexpand(true);
    input_hbox.append(&input_scroll);
    let emoji_btn: gtk::Widget = from_glib_none(hx_emoji_button_new(wptr(&input)));
    emoji_btn.set_valign(gtk::Align::End);
    input_hbox.append(&emoji_btn);
    hx_emoji_typeahead_attach(wptr(&input));

    let inputframe = gtk::Frame::new(None);
    inputframe.set_child(Some(&input_hbox));

    let vpane = gtk::Box::new(gtk::Orientation::Vertical, 4);
    outputframe.set_vexpand(true);
    inputframe.set_vexpand(false);
    vpane.append(&outputframe);
    vpane.append(&inputframe);
    vpane.set_margin_start(5);
    vpane.set_margin_end(5);
    vpane.set_margin_top(5);
    vpane.set_margin_bottom(5);

    // Recipient info pane (icon + markup name/status). Created in Rust; stored
    // on the msgwin so msgwin_refresh_user_info can repopulate them.
    let info_image = gtk::Image::new();
    info_image.set_pixel_size(32);
    info_image.set_size_request(32, 32);
    let info_label = gtk::Label::new(None);
    info_label.set_xalign(0.0);
    info_label.set_yalign(0.5);
    info_label.set_use_markup(true);
    info_label.set_ellipsize(pango::EllipsizeMode::End);
    info_label.set_hexpand(true);
    hx_msgwin_set_info_image(msg, wptr(&info_image));
    hx_msgwin_set_info_label(msg, wptr(&info_label));

    let info_box = gtk::Box::new(gtk::Orientation::Horizontal, 10);
    info_box.set_margin_start(10);
    info_box.set_margin_end(10);
    info_box.set_margin_top(6);
    info_box.set_margin_bottom(4);
    info_box.append(&info_image);
    info_box.append(&info_label);

    let outer_vbox = gtk::Box::new(gtk::Orientation::Vertical, 0);
    outer_vbox.append(&info_box);
    outer_vbox.append(&gtk::Separator::new(gtk::Orientation::Horizontal));
    vpane.set_vexpand(true);
    outer_vbox.append(&vpane);

    // msg->window is the tab content; stash msg on it via the same raw qdata
    // the C code used ("msg" key), so the close dispatcher path is unchanged.
    hx_msgwin_set_window(msg, wptr(&outer_vbox));
    let key = c"msg".as_ptr();
    glib::gobject_ffi::g_object_set_data(outer_vbox.as_ptr() as *mut _, key, msg);

    // Add the tab, populate the info pane, surface it, wire accelerators.
    let title = format!("{} ({})", crate::cstr(name as *const c_char), uid);
    let ctitle = crate::cs(&title);
    gtkhx_chat_tabs_add_msg(wptr(&outer_vbox), uid, ctitle.as_ptr());

    msgwin_refresh_user_info(msg);
    gtkhx_chat_tabs_raise_msg(uid);
    init_keyaccel(wptr(&outer_vbox));
    input.grab_focus();

    msg
}
