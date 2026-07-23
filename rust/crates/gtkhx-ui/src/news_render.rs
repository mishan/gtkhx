//! `news_render` — the 1.5 news-browser right-pane post rendering + the
//! selection breadcrumb, ported from `news_browser.c` (`render_selected_post`
//! and `update_breadcrumb`).
//!
//! Both are called from the C selection handler / refresh / thread-reply paths,
//! which still own the browser struct + its widget fields. Those C functions
//! stay as thin one-line delegators that pass the relevant widgets down here;
//! the rendering logic lives in Rust. Cache-miss body fetch routes back through
//! the `gtkhx_news_fetch_thread` bridge (keeping `fetch_thread` + the
//! pending-request tables C); the post date is formatted by the shared
//! `gtkhx_news_node_date_string` bridge (over C's `post_date_format`).

use std::ffi::{c_char, c_void};

use gtk4 as gtk;
use gtk::glib;
use gtk::prelude::*;
use glib::translate::from_glib_none;

use crate::tr::{tr, tr_fmt};

const NB_KIND_POST: i32 = 3;

extern "C" {
    // hxnews-model node accessors.
    fn hx_news_node_kind(node: *mut c_void) -> i32;
    fn hx_news_node_name(node: *mut c_void) -> *const c_char;
    fn hx_news_node_sender(node: *mut c_void) -> *const c_char;
    fn hx_news_node_body(node: *mut c_void) -> *const c_char;
    // news_browser.c bridges.
    fn gtkhx_news_fetch_thread(browser: *mut c_void, node: *mut c_void);
    // gtkurl.c — (re)tag URLs in a text view after a buffer mutation.
    fn gtkurl_textview_apply_tags(tv: *mut gtk::ffi::GtkTextView);
}

/// Render the currently-selected post into the right pane. `node` is the
/// browser's `selected_post` (NULL / non-post → the empty-state placeholder).
/// On a body cache-miss it shows "Loading…" and fires the GETTHREAD fetch.
///
/// # Safety
/// C-ABI entry on the GTK main thread. The widget pointers are the browser's
/// live `post_view` / `subject_label` / `meta_label` / `header_strip`; `node`
/// is NULL or a valid `HxNewsNode *`; `browser` is the `gnews_browser *`.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_news_render_post(
    browser: *mut c_void,
    post_view: *mut gtk::ffi::GtkWidget,
    subject_label: *mut gtk::ffi::GtkLabel,
    meta_label: *mut gtk::ffi::GtkLabel,
    header_strip: *mut gtk::ffi::GtkWidget,
    node: *mut c_void,
) {
    crate::ensure_gtk_init();
    // Bail rather than abort the process if the widget wiring ever drifts — a
    // panic can't unwind across this extern "C" boundary.
    let Ok(post_view) =
        from_glib_none::<_, gtk::Widget>(post_view).downcast::<gtk::TextView>()
    else {
        return;
    };
    let subject_label: gtk::Label = from_glib_none(subject_label);
    let meta_label: gtk::Label = from_glib_none(meta_label);
    let header_strip: gtk::Widget = from_glib_none(header_strip);
    let buffer = post_view.buffer();

    if node.is_null() || hx_news_node_kind(node) != NB_KIND_POST {
        header_strip.set_visible(false);
        buffer.set_text(&tr("Select a post in the tree to view it here."));
        gtkurl_textview_apply_tags(post_view.as_ptr());
        return;
    }

    // Header strip: subject + "<sender> — <date>".
    let name = crate::cstr(hx_news_node_name(node));
    let subject_display = if name.is_empty() {
        tr("(no subject)")
    } else {
        name
    };
    subject_label.set_text(&subject_display);

    let sender = crate::cstr(hx_news_node_sender(node));
    let sender = if sender.is_empty() { "?".to_string() } else { sender };
    let date = crate::hl_date::news_node_date_string(node).unwrap_or_default();
    // Single translatable msgid with positional args (was the C
    // _("%1$s — %2$s")) so a translation can reorder sender / date.
    meta_label.set_text(&tr_fmt("%1$s — %2$s", &[&sender, &date]));
    header_strip.set_visible(true);

    // Body — cached, or "Loading…" while the GETTHREAD fetch is in flight.
    let body_ptr = hx_news_node_body(node);
    if body_ptr.is_null() {
        buffer.set_text(&tr("Loading…"));
        gtkhx_news_fetch_thread(browser, node);
    } else {
        buffer.set_text(&crate::cstr(body_ptr));
    }
    // Tag URLs so the hover-cursor + right-click popup have anchors.
    gtkurl_textview_apply_tags(post_view.as_ptr());
}

/// Rebuild the "/foo/bar" breadcrumb from the current selection and return the
/// selected leaf node (borrowed — the tree owns the ref), or NULL if nothing is
/// selected. The C caller uses the leaf to update `selected_post`.
///
/// # Safety
/// C-ABI entry on the GTK main thread. `breadcrumb` / `selection` / `tree_model`
/// are the browser's live widgets.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_news_update_breadcrumb(
    breadcrumb: *mut gtk::ffi::GtkLabel,
    selection: *mut gtk::ffi::GtkSingleSelection,
    tree_model: *mut gtk::ffi::GtkTreeListModel,
) -> *mut c_void {
    crate::ensure_gtk_init();
    let breadcrumb: gtk::Label = from_glib_none(breadcrumb);
    let selection: gtk::SingleSelection = from_glib_none(selection);
    let tree_model: gtk::TreeListModel = from_glib_none(tree_model);

    let pos = selection.selected();
    let row = if pos == gtk::ffi::GTK_INVALID_LIST_POSITION {
        None
    } else {
        tree_model.item(pos).and_downcast::<gtk::TreeListRow>()
    };
    let Some(row) = row else {
        breadcrumb.set_text("/");
        return std::ptr::null_mut();
    };

    // Walk upward from the selected leaf, prepending each name.
    let mut names: Vec<String> = Vec::new();
    let mut leaf: *mut c_void = std::ptr::null_mut();
    let mut cur = Some(row);
    while let Some(c) = cur {
        if let Some(node) = c.item() {
            if leaf.is_null() {
                // The first node visited is the selected leaf.
                leaf = node.as_ptr() as *mut c_void;
            }
            names.insert(0, crate::cstr(hx_news_node_name(node.as_ptr() as *mut c_void)));
        }
        cur = c.parent();
    }

    breadcrumb.set_text(&format!("/{}", names.join("/")));
    leaf
}
