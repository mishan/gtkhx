//! `news_tree` — the 1.5 news-browser tree view: the `GtkTreeListModel`'s
//! child-model function and the `GtkListView` row factory (setup / bind /
//! unbind + lazy-fetch-on-expand), ported from `news_browser.c`
//! (`news_node_create_child_model` + `on_factory_setup/bind/unbind` +
//! `on_row_expanded`).
//!
//! The C side keeps the ~15 lines that stitch these together (create the
//! `root_store`, wrap the returned tree model in a `GtkSingleSelection`, drop
//! the returned factory into a `GtkListView`) because `root_store` /
//! `tree_model` / `selection` are read from a dozen other spots in the browser.
//! The selection-changed handler also stays C (it drives `render_selected_post`
//! / `sync_action_buttons`, which are the content side). Everything the factory
//! needs from the browser goes through two small C bridges:
//! `gtkhx_news_icon_for_kind` (the per-kind row icon) and
//! `gtkhx_news_fetch_for_expanded` (fire the DIRLIST / CATLIST on first expand).

use std::ffi::c_void;

use gtk4 as gtk;
use gtk::gio;
use gtk::glib;
use gtk::prelude::*;
use glib::translate::{from_glib_full, from_glib_none, IntoGlibPtr};

// NB_KIND_POST from the NB_KIND_* enum in news_browser.c (FOLDER=1,
// CATEGORY=2, POST=3). Only POST is inspected here — the create-child-model
// decision special-cases posts; the folder/category fetch dispatch lives in
// the C bridge (gtkhx_news_fetch_for_expanded).
const NB_KIND_POST: i32 = 3;

extern "C" {
    // hxnews-model C ABI — node accessors (the browser externs these too).
    fn hx_news_node_kind(node: *mut c_void) -> i32;
    fn hx_news_node_name(node: *mut c_void) -> *const std::ffi::c_char;
    fn hx_news_node_children(node: *mut c_void) -> *mut gio::ffi::GListStore;
    fn hx_news_node_ensure_children(node: *mut c_void) -> *mut gio::ffi::GListStore;
    // news_browser.c bridges — the browser's per-kind icon + the
    // lazy-fetch-on-expand dispatch (keeps fetch_dirlist / fetch_catlist +
    // the pending-request tables C).
    fn gtkhx_news_icon_for_kind(
        browser: *mut c_void,
        kind: i32,
    ) -> *mut gtk::gdk::ffi::GdkPaintable;
    fn gtkhx_news_fetch_for_expanded(browser: *mut c_void, node: *mut c_void);
}

/// `GtkTreeListModelCreateModelFunc`: given a row's `HxNewsNode`, decide whether
/// it's a leaf (return `None`) or expandable (return its children store).
///
/// Folders / categories are always expandable — an empty children store is
/// lazily allocated so the expander appears, and the fetch fires on first
/// expand. A post is expandable only if it already has replies (the CATLIST
/// threading walker populated its children in the same reply).
fn create_child_model(item: &glib::Object) -> Option<gio::ListModel> {
    let node = item.as_ptr() as *mut c_void;
    unsafe {
        if hx_news_node_kind(node) == NB_KIND_POST {
            let ch = hx_news_node_children(node);
            if ch.is_null() {
                return None;
            }
            let store: gio::ListStore = from_glib_none(ch);
            return if store.n_items() > 0 {
                Some(store.upcast())
            } else {
                None
            };
        }
        let store: gio::ListStore = from_glib_none(hx_news_node_ensure_children(node));
        Some(store.upcast())
    }
}

/// Build the browser's `GtkTreeListModel`. Consumes one ref of `root_model`
/// (transfer full), exactly like the `gtk_tree_list_model_new` call it replaces
/// — the C caller's `root_store` field is left as a borrowed alias into the
/// tree model's owned ref.
///
/// # Safety
/// `root_model` is a valid `GListModel *` whose ref is transferred here.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_news_build_tree_model(
    root_model: *mut gio::ffi::GListModel,
) -> *mut gtk::ffi::GtkTreeListModel {
    crate::ensure_gtk_init();
    let root: gio::ListModel = from_glib_full(root_model);
    let model = gtk::TreeListModel::new(
        root, /* passthrough */ false, /* autoexpand */ false,
        |item| create_child_model(item),
    );
    model.into_glib_ptr()
}

/// Build the `GtkListView` row factory (setup / bind / unbind). `browser` is the
/// opaque `gnews_browser *`, captured for the icon + expand bridges; it outlives
/// the factory. Returned transfer-full (the `GtkListView` takes ownership).
///
/// # Safety
/// `browser` is a valid `gnews_browser *` for the browser's lifetime.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_news_build_factory(
    browser: *mut c_void,
) -> *mut gtk::ffi::GtkListItemFactory {
    crate::ensure_gtk_init();
    // `browser` is a raw `*mut c_void` — `Copy`, so it's captured by the `move`
    // closures directly. (No `usize` round-trip: that would strip the pointer's
    // provenance under Rust's strict-provenance model.)

    let factory = gtk::SignalListItemFactory::new();

    // setup: one expander → box[ icon, label ] per row widget.
    factory.connect_setup(|_, obj| {
        let Some(item) = obj.downcast_ref::<gtk::ListItem>() else {
            return;
        };
        let boxw = gtk::Box::new(gtk::Orientation::Horizontal, 6);
        let icon = gtk::Image::new();
        // GtkImage clamps a paintable to its icon-size (~16px); force the pixel
        // size so the row uses the upscaled render.
        icon.set_pixel_size(24);
        let label = gtk::Label::new(None);
        label.set_xalign(0.0);
        label.set_ellipsize(gtk::pango::EllipsizeMode::End);
        boxw.append(&icon);
        boxw.append(&label);
        let expander = gtk::TreeExpander::new();
        expander.set_child(Some(&boxw));
        item.set_child(Some(&expander));
    });

    // bind: point the expander at the row, fill icon + label from the node, and
    // wire a one-shot lazy fetch on first expand.
    factory.connect_bind(move |_, obj| {
        let Some(item) = obj.downcast_ref::<gtk::ListItem>() else {
            return;
        };
        let Some(expander) = item.child().and_downcast::<gtk::TreeExpander>() else {
            return;
        };
        let row = item.item().and_downcast::<gtk::TreeListRow>();
        expander.set_list_row(row.as_ref());

        let (icon, label) = match expander_icon_label(&expander) {
            Some(pair) => pair,
            None => return,
        };

        let node = row.as_ref().and_then(|r| r.item());
        let Some(node) = node else {
            icon.clear();
            label.set_text("");
            return;
        };

        let paintable = unsafe {
            let p = gtkhx_news_icon_for_kind(
                browser,
                hx_news_node_kind(node.as_ptr() as *mut c_void),
            );
            if p.is_null() {
                None
            } else {
                Some(from_glib_none::<_, gtk::gdk::Paintable>(p))
            }
        };
        match paintable {
            Some(p) => icon.set_paintable(Some(&p)),
            None => icon.clear(),
        }
        label.set_text(&unsafe { crate::cstr(hx_news_node_name(node.as_ptr() as *mut c_void)) });

        // Lazy fetch: fire DIRLIST / CATLIST the first time this row's expander
        // opens. Connection lives for this binding only — unbind disconnects.
        if let Some(row) = row {
            let handler = row.connect_expanded_notify(move |row| {
                if !row.is_expanded() {
                    return;
                }
                if let Some(node) = row.item() {
                    unsafe {
                        gtkhx_news_fetch_for_expanded(
                            browser,
                            node.as_ptr() as *mut c_void,
                        );
                    }
                }
            });
            unsafe { item.set_data(EXP_KEY, (row, handler)) };
        }
    });

    // unbind: disconnect the notify::expanded handler + detach the row.
    factory.connect_unbind(|_, obj| {
        let Some(item) = obj.downcast_ref::<gtk::ListItem>() else {
            return;
        };
        if let Some(data) =
            unsafe { item.steal_data::<(gtk::TreeListRow, glib::SignalHandlerId)>(EXP_KEY) }
        {
            let (row, handler) = data;
            row.disconnect(handler);
        }
        if let Some(expander) = item.child().and_downcast::<gtk::TreeExpander>() {
            expander.set_list_row(None);
        }
    });

    factory.upcast::<gtk::ListItemFactory>().into_glib_ptr()
}

/// qdata key for the per-binding `(row, notify::expanded handler)` stash. Uses
/// gtk-rs `set_data` (boxes the Rust tuple) — set and stolen only on the Rust
/// side, so it never has to interop with C `g_object_set_data`.
const EXP_KEY: &str = "gtkhx-news-expanded";

/// Recover the icon + label from an expander built by `setup`
/// (expander → box → [icon, label]).
fn expander_icon_label(expander: &gtk::TreeExpander) -> Option<(gtk::Image, gtk::Label)> {
    let boxw = expander.child()?;
    let icon = boxw.first_child().and_downcast::<gtk::Image>()?;
    let label = icon.next_sibling().and_downcast::<gtk::Label>()?;
    Some((icon, label))
}
