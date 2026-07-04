//! Chat window — Phase R5.13 shell + the public-chat content build (gtk4-rs).
//!
//! The public-chat panel is docked into the toolbar window's CENTER area. This
//! module owns both the *window shell* (raise-if-open, dock registration via
//! `dock_bridge.c`, post-embed lifecycle) and, since the content port, the
//! *content tree*: the subject entry over the xtext output + auto-grow input
//! (with emoji / inline-media buttons), all inside the `AdwTabView` that hosts
//! the per-conversation private-chat / private-message tabs.
//!
//! The C-coupled leaves stay C behind a small seam (mirrors `pchat.rs`):
//! `gtkhx_chat_build_leaves` makes the subject entry, the input text view (+ its
//! key controller / qdata read by the C input handler), and the inline-media
//! button; the xtext output + scrollbar were made by `create_chat` at session
//! init. Rust reads every leaf via the `hx_gchat_*` accessors, assembles the
//! frame/box/tab skeleton, and writes the window back via a setter. The tab
//! view itself is `chat_tabs.c`'s singleton (`gtkhx_chat_tabs_init`).
//!
//! Private chats and private messages are tabs *inside* this one panel
//! (`chat_tabs.c` / AdwTabView), not separate dock panels — so there is a
//! single dock registration and the dynamic-panel path isn't needed here.

use std::ffi::c_void;

use gtk4 as gtk;
use gtk::glib;
use gtk::prelude::*;
use glib::translate::{from_glib_none, IntoGlibPtr};
use libadwaita as adw;

use crate::dock;
use crate::tr::tr;

/// Opaque C `session *` / `struct gtkhx_chat *`.
type Session = c_void;
type Gchat = c_void;

/// Stable panel id — matches `HX_PANEL_ID_CHAT` (`panel_registry.h`).
const HX_ID_CHAT: &str = "chat";

extern "C" {
    /// Make the public-chat gchat's C-coupled leaf widgets (subject / input /
    /// media button) + store them on the gchat; returns it, or NULL if there's
    /// no public-chat gchat yet.
    fn gtkhx_chat_build_leaves(sess: *mut Session) -> *mut Gchat;
    /// Carry the session onto the dock panel, mark it open, focus the input.
    fn gtkhx_chat_after_embed(sess: *mut Session);
    /// Panel-content "destroy" handler: null the gchat's widget pointers.
    fn gtkhx_chat_clear_content_ptrs(gchat: *mut Gchat);

    fn hx_gchat_output(g: *mut Gchat) -> *mut gtk::ffi::GtkWidget;
    fn hx_gchat_vscroll(g: *mut Gchat) -> *mut gtk::ffi::GtkWidget;
    fn hx_gchat_input(g: *mut Gchat) -> *mut gtk::ffi::GtkWidget;
    fn hx_gchat_subject(g: *mut Gchat) -> *mut gtk::ffi::GtkWidget;
    fn hx_gchat_media_btn(g: *mut Gchat) -> *mut gtk::ffi::GtkWidget;
    fn hx_gchat_set_window(g: *mut Gchat, w: *mut gtk::ffi::GtkWidget);

    fn hx_emoji_button_new(target: *mut gtk::ffi::GtkWidget) -> *mut gtk::ffi::GtkWidget;
    fn hx_emoji_typeahead_attach(target: *mut gtk::ffi::GtkWidget);

    /// chat_tabs.c — the singleton AdwTabView + the pinned public-chat tab.
    fn gtkhx_chat_tabs_init() -> *mut gtk::ffi::GtkWidget;
    fn gtkhx_chat_tabs_add_public(content: *mut gtk::ffi::GtkWidget, title: *const std::ffi::c_char);
}

/// `*mut GtkWidget` for a gtk-rs widget.
fn wptr<W: IsA<gtk::Widget>>(w: &W) -> *mut gtk::ffi::GtkWidget {
    w.as_ref().as_ptr()
}

/// Return a freshly-built widget with a floating reference (matching a GTK C
/// constructor; `dock_bridge`'s embed sinks it).
unsafe fn into_floating_ptr<W: IsA<gtk::Widget>>(w: W) -> *mut gtk::ffi::GtkWidget {
    let ptr = w.upcast::<gtk::Widget>().into_glib_ptr();
    glib::gobject_ffi::g_object_force_floating(ptr as *mut glib::gobject_ffi::GObject);
    ptr
}

/// Build the public-chat panel content in gtk4-rs. Replacement for the old
/// `chat.c::gtkhx_chat_build_content`; returns a still-floating container (or
/// NULL when there's no public-chat gchat).
///
/// # Safety
/// `sess` is a valid `session *` (or NULL); called on the GTK main thread.
unsafe fn build_content(sess: *mut Session) -> *mut gtk::ffi::GtkWidget {
    let gchat = gtkhx_chat_build_leaves(sess);
    if gchat.is_null() {
        return std::ptr::null_mut();
    }

    // Leaf widgets: output/vscroll from create_chat, the rest from build_leaves.
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
    let out_hbox = gtk::Box::new(gtk::Orientation::Horizontal, 0);
    output.set_hexpand(true);
    output.set_vexpand(true);
    out_hbox.append(&output);
    out_hbox.append(&vscroll);
    let outputframe = gtk::Frame::new(None);
    outputframe.set_child(Some(&out_hbox));
    outputframe.set_vexpand(true);

    // Input frame: auto-grow input (28..120 px) + emoji button + inline-media.
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

    // The public chat goes into a pinned tab at position 0 of the shared
    // AdwTabView; per-conversation tabs (pchat / PM) append alongside it. A
    // small autohiding AdwTabBar sits above the view.
    let tab_view: gtk::Widget = from_glib_none(gtkhx_chat_tabs_init());
    let tab_bar = adw::TabBar::new();
    if let Some(tv) = tab_view.downcast_ref::<adw::TabView>() {
        tab_bar.set_view(Some(tv));
    }
    tab_bar.set_autohide(true);

    let ctitle = crate::cs(&tr("Chat"));
    gtkhx_chat_tabs_add_public(wptr(&vbox), ctitle.as_ptr());

    let panel_box = gtk::Box::new(gtk::Orientation::Vertical, 0);
    panel_box.append(&tab_bar);
    tab_view.set_vexpand(true);
    panel_box.append(&tab_view);

    // notify.c walks back from the public chat to a focusable widget via
    // gchat->window — point it at the content box, not the dock panel (which the
    // Rust shell owns). window_is_active() checks GTK_IS_WINDOW(panel_box) →
    // false, so notifications always fire for public chat, as before.
    hx_gchat_set_window(gchat, wptr(&panel_box));
    let gchat_addr = gchat as usize;
    panel_box.connect_destroy(move |_| unsafe {
        gtkhx_chat_clear_content_ptrs(gchat_addr as *mut Gchat);
    });

    into_floating_ptr(panel_box)
}

/// Open (or raise) the Chat panel. C ABI replacement for the old
/// `chat.c::create_chat_window`. `parent` is vestigial (the panel is a
/// resident of the toolbar dock, not reparented); `data` is the `session *`.
///
/// # Safety
/// `data` is a valid `session *` (or NULL); called on the GTK main thread.
#[no_mangle]
pub unsafe extern "C" fn create_chat_window(_parent: *mut c_void, data: *mut c_void) {
    crate::ensure_gtk_init();

    // Re-open → re-attach + raise the existing panel, don't rebuild.
    if dock::raise_if_open(HX_ID_CHAT) {
        return;
    }

    let sess = data as *mut Session;
    let content = build_content(sess);
    if content.is_null() {
        return;
    }

    // On failure the bridge has already destroyed `content` (firing the
    // clear-ptrs destroy handler); skip the post-embed lifecycle.
    if dock::embed(
        HX_ID_CHAT,
        dock::KIND_CENTER,
        dock::AREA_CENTER,
        "Chat",
        "user-available-symbolic",
        content,
    ) {
        gtkhx_chat_after_embed(sess);
    }
}
