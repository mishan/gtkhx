//! Private Chat window — Phase R5.15 gtk4-rs content port.
//!
//! A private chat is a tab inside the Chat panel's `AdwTabView`.
//! `create_pchat_window` builds the tab's content tree here in gtk4-rs — a
//! subject bar over a horizontal pane whose left side is the chat column
//! (xtext output + auto-grow input with emoji/inline-media buttons) and whose
//! right side is the user sidebar — then adds the tab via `chat_tabs.c`.
//!
//! The C-coupled leaves stay C behind a small seam (mirrors `msg.rs`):
//! `gtkhx_pchat_new` makes the gchat model + the leaf widgets (xtext output,
//! input with its key controller, subject entry, inline-media button), and
//! `gtkhx_pchat_user_sidebar` builds the whole user sidebar (the HxUserListView
//! + the six action buttons wired to the C `view_*_btn` handlers + the voice
//! panel). Rust reads the leaf widgets via `hx_gchat_*` accessors, assembles
//! the frame/box/pane skeleton, and writes back the window via a setter.

use std::ffi::c_void;

use gtk4 as gtk;
use gtk::glib;
use gtk::prelude::*;
use glib::translate::from_glib_none;

use crate::tr::tr;

/// Opaque C `struct gtkhx_chat *` / `struct chat *` / `struct htlc_conn *`.
type Gchat = c_void;

extern "C" {
    fn gtkhx_pchat_new(htlc: *mut c_void, chat: *mut c_void) -> *mut Gchat;
    fn gtkhx_pchat_user_sidebar(htlc: *mut c_void, chat: *mut c_void)
        -> *mut gtk::ffi::GtkWidget;
    fn hx_chat_cid(chat: *mut c_void) -> u32;
    fn hx_gchat_output(g: *mut Gchat) -> *mut gtk::ffi::GtkWidget;
    fn hx_gchat_vscroll(g: *mut Gchat) -> *mut gtk::ffi::GtkWidget;
    fn hx_gchat_input(g: *mut Gchat) -> *mut gtk::ffi::GtkWidget;
    fn hx_gchat_subject(g: *mut Gchat) -> *mut gtk::ffi::GtkWidget;
    fn hx_gchat_media_btn(g: *mut Gchat) -> *mut gtk::ffi::GtkWidget;
    fn hx_gchat_set_window(g: *mut Gchat, w: *mut gtk::ffi::GtkWidget);

    fn hx_emoji_button_new(target: *mut gtk::ffi::GtkWidget) -> *mut gtk::ffi::GtkWidget;
    fn hx_emoji_typeahead_attach(target: *mut gtk::ffi::GtkWidget);
    fn gtkhx_chat_tabs_add_pchat(
        content: *mut gtk::ffi::GtkWidget,
        cid: u32,
        title: *const std::ffi::c_char,
    ) -> *mut c_void;
    fn gtkhx_chat_tabs_raise_pchat(cid: u32);
    fn init_keyaccel(widget: *mut gtk::ffi::GtkWidget);
}

/// `*mut GtkWidget` for a gtk-rs widget.
fn wptr<W: IsA<gtk::Widget>>(w: &W) -> *mut gtk::ffi::GtkWidget {
    w.as_ref().as_ptr()
}

/// Build a Private Chat tab. C ABI replacement for the old
/// `chat.c::create_pchat_window`; returns the `struct gtkhx_chat *`.
///
/// # Safety
/// `htlc` / `chat` are valid C pointers; called on the GTK main thread.
#[no_mangle]
pub unsafe extern "C" fn create_pchat_window(htlc: *mut c_void, chat: *mut c_void) -> *mut c_void {
    crate::ensure_gtk_init();

    let gchat = gtkhx_pchat_new(htlc, chat);
    if gchat.is_null() {
        return std::ptr::null_mut();
    }
    let cid = hx_chat_cid(chat);

    // Leaf widgets from the C helper (still floating; append/set_child sink).
    let output: gtk::Widget = from_glib_none(hx_gchat_output(gchat));
    let vscroll: gtk::Widget = from_glib_none(hx_gchat_vscroll(gchat));
    let input: gtk::Widget = from_glib_none(hx_gchat_input(gchat));
    let subject: gtk::Widget = from_glib_none(hx_gchat_subject(gchat));
    let media: gtk::Widget = from_glib_none(hx_gchat_media_btn(gchat));

    // Subject bar.
    let subj_hbox = gtk::Box::new(gtk::Orientation::Horizontal, 0);
    subject.set_hexpand(true);
    subj_hbox.append(&subject);
    let subj_frame = gtk::Frame::new(None);
    subj_frame.set_child(Some(&subj_hbox));

    // Output frame: xtext + its scrollbar.
    let pchat_hbox = gtk::Box::new(gtk::Orientation::Horizontal, 0);
    output.set_hexpand(true);
    output.set_vexpand(true);
    pchat_hbox.append(&output);
    pchat_hbox.append(&vscroll);
    let outputframe = gtk::Frame::new(None);
    outputframe.set_child(Some(&pchat_hbox));
    outputframe.set_vexpand(true);

    // Input frame: auto-grow input + emoji button + inline-media button.
    let input_scroll = gtk::ScrolledWindow::new();
    input_scroll.set_policy(gtk::PolicyType::Never, gtk::PolicyType::Automatic);
    input_scroll.set_propagate_natural_height(true);
    input_scroll.set_min_content_height(28);
    input_scroll.set_max_content_height(120);
    input_scroll.set_child(Some(&input));

    let input_hbox = gtk::Box::new(gtk::Orientation::Horizontal, 0);
    input_scroll.set_hexpand(true);
    input_hbox.append(&input_scroll);
    let emoji: gtk::Widget = from_glib_none(hx_emoji_button_new(wptr(&input)));
    emoji.set_valign(gtk::Align::End);
    input_hbox.append(&emoji);
    hx_emoji_typeahead_attach(wptr(&input));
    media.set_valign(gtk::Align::End);
    input_hbox.append(&media);

    let inputframe = gtk::Frame::new(None);
    inputframe.set_child(Some(&input_hbox));
    inputframe.set_vexpand(false);

    // Chat column: output over input, under the subject bar.
    let vstack = gtk::Box::new(gtk::Orientation::Vertical, 4);
    vstack.append(&outputframe);
    vstack.append(&inputframe);

    let vbox = gtk::Box::new(gtk::Orientation::Vertical, 4);
    vbox.set_margin_start(5);
    vbox.set_margin_end(5);
    vbox.set_margin_top(5);
    vbox.set_margin_bottom(5);
    vbox.append(&subj_frame);
    vstack.set_vexpand(true);
    vbox.append(&vstack);

    // User sidebar (C helper) on the right of the pane.
    let user_sidebar: gtk::Widget = from_glib_none(gtkhx_pchat_user_sidebar(htlc, chat));

    let hpane = gtk::Paned::new(gtk::Orientation::Horizontal);
    hpane.set_start_child(Some(&vbox));
    hpane.set_end_child(Some(&user_sidebar));
    hpane.set_position(435);

    // gchat->window = the tab content (the pane). notify.c only needs a widget
    // in the panel's tree; the input carries the "sess"/"gchat" qdata the key
    // handler reads (set in gtkhx_pchat_new), so the pane needs no qdata.
    hx_gchat_set_window(gchat, wptr(&hpane));

    let title = format!("{}: 0x{:08x}", tr("Private Chat"), cid);
    let ctitle = crate::cs(&title);
    gtkhx_chat_tabs_add_pchat(wptr(&hpane), cid, ctitle.as_ptr());
    gtkhx_chat_tabs_raise_pchat(cid);
    init_keyaccel(wptr(&hpane));
    input.grab_focus();

    gchat
}
