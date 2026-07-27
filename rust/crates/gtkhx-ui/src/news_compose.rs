//! `news_compose` — the 1.5 news-browser compose window (New Post / Reply),
//! ported from `news_browser.c` (`open_compose_window` +
//! `build_reply_context_panel` + `compose_do_post` / `compose_cancel`).
//!
//! A modal `GtkWindow` transient over the toolbar window: subject entry, body
//! `GtkTextView`, and (for a reply) a "Replying to …" context card above the
//! form. Post sends `hx_news15_post_thread` (the Rust `hxnews-send` sender) and
//! refreshes the containing category; Cancel / close just destroys.
//!
//! The C side keeps the selection logic (the toolbar handlers pick the category
//! path + reply target and fire the body prefetch), and two small bridges:
//! `gtkhx_news_refresh_category` (find the category in the tree and refetch it,
//! else root) and `gtkhx_news_node_date_string` (format a post's date the same
//! way the browser's post pane does). Everything else is here.

use std::ffi::{c_char, c_void};

use gtk4 as gtk;
use gtk::glib;
use gtk::prelude::*;
use libadwaita as adw;
use glib::translate::from_glib_none;

use crate::tr::{tr, tr_fmt};

const NB_KIND_POST: i32 = 3;

use hxmodel::news::node::hx_news_node_name;
use hxhandlers::send::news::hx_news15_post_thread;

extern "C" {    // hxnews-model node accessors (for the reply-context card).
    fn hx_news_node_kind(node: *mut c_void) -> i32;
    fn hx_news_node_postid(node: *mut c_void) -> u32;
    fn hx_news_node_sender(node: *mut c_void) -> *const c_char;
    fn hx_news_node_body(node: *mut c_void) -> *const c_char;
    // news_browser.c bridges.
    fn gtkhx_news_refresh_category(path: *const c_char);
    // gtkhx_ui_bridge.c — the focused session's &htlc (single-session).
    fn gtkhx_active_htlc() -> *mut c_void;
    // gtkhx.c — themed .gtkhx-input CSS class + font for the compose body.
    fn gtkhx_apply_input_style(w: *mut gtk::ffi::GtkWidget);
    // toolbar.c — transient-for parent. May be NULL.
    static toolbar_window: *mut gtk::ffi::GtkWidget;
}

/// Open the compose window.
///
/// `category_path` is the category to post into. `reply_to` is NULL for a new
/// post, or the `HxNewsNode *` of the post being replied to (its postid becomes
/// the thread parent and it drives the context card). `prefill_subject` seeds
/// the subject entry ("Re: …" for replies).
///
/// # Safety
/// C-ABI entry from a toolbar handler on the GTK main thread. `category_path` /
/// `prefill_subject` are NUL-terminated C strings; `reply_to` is NULL or a valid
/// `HxNewsNode *` read synchronously here (not retained past this call).
#[no_mangle]
pub unsafe extern "C" fn gtkhx_news_compose_open(
    category_path: *const c_char,
    reply_to: *mut c_void,
    prefill_subject: *const c_char,
) {
    crate::ensure_gtk_init();
    if category_path.is_null() {
        return;
    }
    let category_path = crate::cstr(category_path);
    let prefill = crate::cstr(prefill_subject);
    let is_reply = !reply_to.is_null() && hx_news_node_kind(reply_to.cast()) == NB_KIND_POST;
    let parent_postid = if reply_to.is_null() {
        0
    } else {
        hx_news_node_postid(reply_to.cast())
    };

    let window = gtk::Window::new();
    window.set_title(Some(&if is_reply { tr("Reply") } else { tr("New Post") }));
    window.set_size_request(560, if is_reply { 540 } else { 380 });
    // br->window is a PanelWidget, not a GtkWindow — parent on the toolbar
    // window every panel uses for sub-dialogs.
    let parent: Option<gtk::Window> = if toolbar_window.is_null() {
        None
    } else {
        from_glib_none::<_, gtk::Widget>(toolbar_window)
            .downcast::<gtk::Window>()
            .ok()
    };
    window.set_transient_for(parent.as_ref());
    window.set_modal(true);

    // Header bar: Cancel (start) + Post (end).
    let header = adw::HeaderBar::new();
    header.set_show_start_title_buttons(false);
    header.set_show_end_title_buttons(false);
    let cancel_btn = gtk::Button::with_mnemonic(&tr("_Cancel"));
    let post_btn = gtk::Button::with_mnemonic(&tr("_Post"));
    post_btn.add_css_class("suggested-action");
    header.pack_start(&cancel_btn);
    header.pack_end(&post_btn);
    window.set_titlebar(Some(&header));

    // Layout: optional reply card + subject row + body editor.
    let content = gtk::Box::new(gtk::Orientation::Vertical, 0);
    if is_reply {
        content.append(&build_reply_context_panel(reply_to));
    }

    let form = gtk::Box::new(gtk::Orientation::Vertical, 6);
    form.set_margin_start(12);
    form.set_margin_end(12);
    form.set_margin_top(10);
    form.set_margin_bottom(6);
    let subject_row = gtk::Box::new(gtk::Orientation::Horizontal, 8);
    let subject_lbl = gtk::Label::new(Some(&tr("Subject:")));
    subject_lbl.set_xalign(0.0);
    let subject_entry = gtk::Entry::new();
    subject_entry.set_hexpand(true);
    subject_entry.set_text(&prefill);
    subject_row.append(&subject_lbl);
    subject_row.append(&subject_entry);
    form.append(&subject_row);
    content.append(&form);

    let body_scroll = gtk::ScrolledWindow::new();
    body_scroll.set_policy(gtk::PolicyType::Automatic, gtk::PolicyType::Automatic);
    body_scroll.set_vexpand(true);
    let body_view = gtk::TextView::new();
    body_view.set_wrap_mode(gtk::WrapMode::WordChar);
    body_view.set_margin_start(4);
    body_view.set_margin_end(4);
    body_view.set_margin_top(4);
    body_view.set_margin_bottom(4);
    gtkhx_apply_input_style(body_view.as_ptr() as *mut gtk::ffi::GtkWidget);
    body_scroll.set_child(Some(&body_view));
    content.append(&body_scroll);

    window.set_child(Some(&content));

    // Cancel → close.
    {
        let window = window.clone();
        cancel_btn.connect_clicked(move |_| window.destroy());
    }
    // Post → send + refresh + close.
    {
        let window = window.clone();
        let subject_entry = subject_entry.clone();
        let body_view = body_view.clone();
        post_btn.connect_clicked(move |_| unsafe {
            let subject = subject_entry.text();
            let buf = body_view.buffer();
            let (start, end) = buf.bounds();
            let body = buf.text(&start, &end, false);
            // crate::cs drops interior NULs rather than failing, so a stray NUL
            // in the body can't silently swallow the whole post.
            let p = crate::cs(&category_path);
            let s = crate::cs(subject.as_str());
            let b = crate::cs(body.as_str());
            hx_news15_post_thread(
                gtkhx_active_htlc(),
                p.as_ptr(),
                s.as_ptr(),
                parent_postid,
                b.as_ptr(),
            );
            // Settle: refetch the affected category (or root if it's not
            // currently in the tree). The server pushes no notification.
            gtkhx_news_refresh_category(p.as_ptr());
            window.destroy();
        });
    }

    window.present();
    // New post → focus the subject; reply → focus the body (the subject is
    // already prefilled "Re: …").
    if is_reply {
        body_view.grab_focus();
    } else {
        subject_entry.grab_focus();
    }
}

/// Build the "Replying to …" context card: sender/date meta line, the original
/// subject, and a capped-height preview of the original body. `reply_to` is a
/// `NB_KIND_POST` node, read synchronously.
unsafe fn build_reply_context_panel(reply_to: *mut c_void) -> gtk::Box {
    let outer = gtk::Box::new(gtk::Orientation::Vertical, 4);
    outer.add_css_class("card");
    outer.set_margin_start(12);
    outer.set_margin_end(12);
    outer.set_margin_top(10);
    outer.set_margin_bottom(4);

    let header_row = gtk::Box::new(gtk::Orientation::Vertical, 0);
    header_row.set_margin_start(8);
    header_row.set_margin_end(8);
    header_row.set_margin_top(6);

    let sender = crate::cstr(hx_news_node_sender(reply_to.cast()));
    let sender = if sender.is_empty() { "?".to_string() } else { sender };
    let date = crate::hl_date::news_node_date_string(reply_to).unwrap_or_default();
    // Single translatable msgid with positional args (was the C
    // _("Replying to %1$s — %2$s")) so translators keep one string + can
    // reorder for their word order.
    let meta = tr_fmt("Replying to %1$s — %2$s", &[&sender, &date]);
    let meta_lbl = gtk::Label::new(Some(&meta));
    meta_lbl.set_xalign(0.0);
    meta_lbl.set_ellipsize(gtk::pango::EllipsizeMode::End);
    meta_lbl.add_css_class("dim-label");
    meta_lbl.add_css_class("caption");

    let name = crate::cstr(hx_news_node_name(reply_to.cast()));
    let subject_display = if name.is_empty() {
        tr("(no subject)")
    } else {
        name
    };
    let subj_lbl = gtk::Label::new(Some(subject_display.as_str()));
    subj_lbl.set_xalign(0.0);
    subj_lbl.set_wrap(true);
    subj_lbl.set_wrap_mode(gtk::pango::WrapMode::WordChar);
    subj_lbl.add_css_class("heading");

    header_row.append(&meta_lbl);
    header_row.append(&subj_lbl);
    outer.append(&header_row);

    // Body preview — scrollable, height-capped so a long original doesn't
    // crowd out the reply box.
    let body_scroll = gtk::ScrolledWindow::new();
    body_scroll.set_policy(gtk::PolicyType::Never, gtk::PolicyType::Automatic);
    body_scroll.set_max_content_height(140);
    body_scroll.set_propagate_natural_height(true);
    let body_view = gtk::TextView::new();
    body_view.set_editable(false);
    body_view.set_cursor_visible(false);
    body_view.set_wrap_mode(gtk::WrapMode::WordChar);
    body_view.set_margin_start(8);
    body_view.set_margin_end(8);
    body_view.set_margin_top(2);
    body_view.set_margin_bottom(6);
    body_view.add_css_class("dim-label");

    let body_ptr = hx_news_node_body(reply_to.cast());
    // Not cached → the user clicked Reply before the GETTHREAD reply landed;
    // show a placeholder rather than block.
    let body_text = if body_ptr.is_null() {
        tr("(original post body not loaded — open the post first to fetch it)")
    } else {
        crate::cstr(body_ptr)
    };
    body_view.buffer().set_text(&body_text);
    body_scroll.set_child(Some(&body_view));
    outer.append(&body_scroll);

    outer
}
