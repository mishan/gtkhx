//! News browser (1.5) — the unified threaded-news window, ported from
//! `news_browser.c`.
//!
//! One process-singleton browser docked into the toolbar window's CENTER area.
//! The `NewsBrowser` thread-local owns the whole content tree (the
//! `GtkTreeListModel` view, the two-pane layout, the cached row icons) plus the
//! RPC state (the in-flight DIRLIST / CATLIST fetch tables). Everything the
//! model needs from the C session — the live htlc version + access bitmap, the
//! `gnews_folder` / `gnews_catalog` request carriers, the post-date formatter,
//! the row-icon loader — comes through thin C leaves in `news_recv_bridge.c`.
//!
//! The tree factory + child-model (`news_tree`), post rendering + breadcrumb
//! (`news_render`), compose window (`news_compose`), and create / delete dialogs
//! (`news_dialogs`) live in their own sibling modules; this module stitches them
//! together and drives the fetch / reply / refresh flow. The only C left in the
//! news path is `rcv.c`'s generic reply dispatch, which hands the parsed
//! carriers to the `gnews_browser_handle_*` entries below.

use std::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::ffi::{c_char, c_int, c_void};

use gtk4 as gtk;
use gtk::gdk;
use gtk::gio;
use gtk::glib;
use gtk::pango;
use gtk::prelude::*;
use glib::translate::{from_glib, from_glib_full, from_glib_none, IntoGlibPtr};
use libadwaita as adw;

use crate::dock;
use crate::tr::tr;

/// Stable panel id — matches `HX_PANEL_ID_NEWS15` (`panel_registry.h`).
const HX_ID_NEWS15: &str = "news15";

/// Node kinds (mirror the `NB_KIND_*` enum in the hxnews-model crate).
const NB_KIND_FOLDER: i32 = 1;
const NB_KIND_CATEGORY: i32 = 2;
const NB_KIND_POST: i32 = 3;

/// `GtkhxScaleArea::GTKHX_SCALE_WINDOW_BUTTONS` (`gtkhx_theme.h`).
const GTKHX_SCALE_WINDOW_BUTTONS: i32 = 1;

/// `GtkhxConnectionState` values (`gtkhx_session.h`).
const GTKHX_CONNECTION_DISCONNECTED: u32 = 0;
const GTKHX_CONNECTION_LOGIN_READY: u32 = 4;

/// Access-bitmap indices (`hl_access.h`).
const HL_ACCESS_READ_NEWS: i32 = 20;
const HL_ACCESS_POST_NEWS: i32 = 21;
const HL_ACCESS_DELETE_ARTICLES: i32 = 33;
const HL_ACCESS_CREATE_CATEGORIES: i32 = 34;
const HL_ACCESS_DELETE_CATEGORIES: i32 = 35;
const HL_ACCESS_CREATE_NEWS_BUNDLES: i32 = 36;
const HL_ACCESS_DELETE_NEWS_BUNDLES: i32 = 37;

use gtkhx_session::gtkhx_session_get_default;
use hotline_proto::ffi::{gtkhx_proto_catlist_free, gtkhx_proto_dirlist_free};
use hxconn::{hx_conn_access_has, hx_conn_access_permits};
use hxmodel::news::node::{hx_news_build_category_tree_from_catlist, hx_news_build_dirlist_from_dirlist, hx_news_node_body_fetching, hx_news_node_children, hx_news_node_get_type, hx_news_node_loaded, hx_news_node_name, hx_news_node_set_body_fetching, hx_news_node_set_loaded};
use hxhandlers::recv::news::carrier::{gnews_catalog_free, gnews_catalog_new, gnews_catalog_parsed, gnews_folder_free, gnews_folder_new, gnews_folder_parsed, news_post_body, news_post_free, news_post_target};
use hxhandlers::send::news::{hx_news15_cat_list, hx_news15_fldr_list, hx_news15_get_post};

extern "C" {    fn hx_news_node_kind(node: *mut c_void) -> i32;
    fn hx_news_node_postid(node: *mut c_void) -> u32;
    fn hx_news_node_path(node: *mut c_void) -> *const c_char;
    fn hx_news_node_mime_type(node: *mut c_void) -> *const c_char;
    fn hx_news_node_body(node: *mut c_void) -> *const c_char;
    fn hx_news_node_set_body(node: *mut c_void, s: *const c_char);

    // ---- hxconn: read the active connection's version / access bits directly
    // (over gtkhx_active_htlc), rather than through news-specific C wrappers ----
    fn hx_conn_version(htlc: *mut c_void) -> u16;
    // ---- news_recv_bridge.c: session / carrier / leaf helpers ----
    fn gtkhx_active_connected() -> glib::ffi::gboolean;
    fn gtkhx_active_htlc() -> *mut c_void;
    /// Load a chrome icon resource through the theme resolver (gtkhx_icon.c),
    /// transfer-full GdkPixbuf.
    fn gtkhx_icon_load(resource: *const c_char) -> *mut gtk::gdk_pixbuf::ffi::GdkPixbuf;

    // ---- C UI leaves (gtkhx_pixmap_button / apply-style / init_keyaccel come
    // from crate::ffi) ----
    fn gtkurl_textview_install(tv: *mut gtk::ffi::GtkTextView);
    fn gtkurl_textview_apply_tags(tv: *mut gtk::ffi::GtkTextView);

    // ---- panel registry + session singleton ----
    fn hx_panel_registry_lookup(id: *const c_char) -> *mut c_void;
}

// The one live browser (`None` while no News panel exists) + the in-flight
// fetch tables.
thread_local! {
    static NEWS_BROWSER: RefCell<Option<NewsBrowser>> = const { RefCell::new(None) };
    // In-flight DIRLIST / CATLIST fetches: carrier-stub ptr → the reffed target
    // node whose children store receives the entries (None = root fetch,
    // populate root_store). The held glib::Object keeps the target alive until
    // the reply lands, exactly like the C g_object_ref.
    static PENDING_DIRLISTS: RefCell<HashMap<usize, Option<glib::Object>>> =
        RefCell::new(HashMap::new());
    static PENDING_CATLISTS: RefCell<HashMap<usize, Option<glib::Object>>> =
        RefCell::new(HashMap::new());
}

/// The browser's widgets + cached icons. Mutable per-selection state
/// (`selected_post`, the session handler id) sits in interior-mutable cells so
/// callbacks reach it through a shared `&NewsBrowser`.
struct NewsBrowser {
    /// Content box; also the dialog-parent / keyaccel anchor.
    window: gtk::Widget,
    root_store: gio::ListStore,
    tree_model: gtk::TreeListModel,
    selection: gtk::SingleSelection,
    icon_folder: Option<gdk::Paintable>,
    icon_category: Option<gdk::Paintable>,
    icon_post: Option<gdk::Paintable>,
    post_view: gtk::TextView,
    subject_label: gtk::Label,
    meta_label: gtk::Label,
    header_strip: gtk::Widget,
    breadcrumb: gtk::Label,
    btn_new_folder: gtk::Widget,
    btn_new_category: gtk::Widget,
    btn_new_post: gtk::Widget,
    btn_reply: gtk::Widget,
    btn_delete: gtk::Widget,
    disconnected_banner: adw::Banner,
    /// Weak — the tree store owns the ref. NULL for folder / category / empty.
    selected_post: Cell<*mut c_void>,
    conn_handler: RefCell<Option<glib::SignalHandlerId>>,
}

/// Run `f` against the live browser, if any.
fn with_browser<R>(f: impl FnOnce(&NewsBrowser) -> R) -> Option<R> {
    NEWS_BROWSER.with(|b| b.borrow().as_ref().map(f))
}

// ---------- Icons ----------

fn load_icon(resource: &str) -> Option<gdk::Paintable> {
    let res = crate::cs(resource);
    let pb_ptr = unsafe { gtkhx_icon_load(res.as_ptr()) };
    if pb_ptr.is_null() {
        return None;
    }
    // Transfer-full: gtkhx_icon_load returns a pixbuf with one ref we own.
    let pb: gtk::gdk_pixbuf::Pixbuf = unsafe { from_glib_full(pb_ptr) };
    // Upscale 1.5x with nearest-neighbour to keep the pixel-art crisp, then wrap
    // as a GdkTexture (paintable) — matches gtkhx_pixmap_button's pixbuf→texture
    // path, which renders where gtk_image_set_from_resource goes blank for our
    // XPM/PNG pixmaps.
    let w = pb.width() * 3 / 2;
    let h = pb.height() * 3 / 2;
    let scaled = pb.scale_simple(w, h, gtk::gdk_pixbuf::InterpType::Nearest)?;
    Some(gdk::Texture::for_pixbuf(&scaled).upcast())
}

// ---------- Selection → breadcrumb / render / buttons ----------

/// Leaf node of the current selection (borrowed — the store owns the ref), or
/// NULL if nothing is selected.
fn selected_node(br: &NewsBrowser) -> *mut c_void {
    let pos = br.selection.selected();
    if pos == gtk::ffi::GTK_INVALID_LIST_POSITION {
        return std::ptr::null_mut();
    }
    let Some(row) = br.tree_model.item(pos).and_downcast::<gtk::TreeListRow>() else {
        return std::ptr::null_mut();
    };
    match row.item() {
        Some(node) => node.as_ptr() as *mut c_void,
        None => std::ptr::null_mut(),
    }
}

fn update_breadcrumb(br: &NewsBrowser) -> *mut c_void {
    unsafe {
        crate::news_render::gtkhx_news_update_breadcrumb(
            br.breadcrumb.as_ptr(),
            br.selection.as_ptr(),
            br.tree_model.as_ptr(),
        )
    }
}

fn render_selected_post(br: &NewsBrowser) {
    unsafe {
        crate::news_render::gtkhx_news_render_post(
            std::ptr::null_mut(),
            br.post_view.as_ptr() as *mut gtk::ffi::GtkWidget,
            br.subject_label.as_ptr(),
            br.meta_label.as_ptr(),
            br.header_strip.as_ptr(),
            br.selected_post.get(),
        );
    }
}

fn on_selection_changed() {
    with_browser(|br| {
        let leaf = update_breadcrumb(br);
        let is_post = !leaf.is_null() && unsafe { hx_news_node_kind(leaf) } == NB_KIND_POST;
        br.selected_post.set(if is_post { leaf } else { std::ptr::null_mut() });
        render_selected_post(br);
        sync_action_buttons(br);
    });
}

// ---------- Header-bar action buttons ----------

fn sync_action_buttons(br: &NewsBrowser) {
    unsafe {
        let node = selected_node(br);
        let kind = if node.is_null() { 0 } else { hx_news_node_kind(node.cast()) };

        br.btn_new_folder
            .set_visible(kind == 0 || kind == NB_KIND_FOLDER);
        br.btn_new_category
            .set_visible(kind == 0 || kind == NB_KIND_FOLDER);
        br.btn_new_post
            .set_visible(kind == NB_KIND_CATEGORY || kind == NB_KIND_POST);
        br.btn_reply.set_visible(kind == NB_KIND_POST);
        br.btn_delete.set_visible(
            kind == NB_KIND_FOLDER || kind == NB_KIND_CATEGORY || kind == NB_KIND_POST,
        );

        let htlc = gtkhx_active_htlc();
        br.btn_new_folder
            .set_sensitive(hx_conn_access_has(htlc.cast(), HL_ACCESS_CREATE_NEWS_BUNDLES) != 0);
        br.btn_new_category
            .set_sensitive(hx_conn_access_has(htlc.cast(), HL_ACCESS_CREATE_CATEGORIES) != 0);
        br.btn_new_post
            .set_sensitive(hx_conn_access_has(htlc.cast(), HL_ACCESS_POST_NEWS) != 0);
        br.btn_reply
            .set_sensitive(hx_conn_access_has(htlc.cast(), HL_ACCESS_POST_NEWS) != 0);

        let delete_bit = match kind {
            NB_KIND_FOLDER => HL_ACCESS_DELETE_NEWS_BUNDLES,
            NB_KIND_CATEGORY => HL_ACCESS_DELETE_CATEGORIES,
            NB_KIND_POST => HL_ACCESS_DELETE_ARTICLES,
            _ => -1,
        };
        br.btn_delete
            .set_sensitive(delete_bit >= 0 && hx_conn_access_has(htlc.cast(), delete_bit) != 0);
    }
}

// ---------- RPC dispatch ----------

/// Whether it's worth speaking the 1.5 threaded-news protocol: server version
/// >= 150 AND read-news permission (an empty legacy access map is permitted; the
/// version gate is what excludes 1.0/1.2 servers that reject NEWSDIRLIST).
unsafe fn threaded_news_available() -> bool {
    let htlc = gtkhx_active_htlc();
    hx_conn_version(htlc.cast()) >= 150 && hx_conn_access_permits(htlc.cast(), HL_ACCESS_READ_NEWS) != 0
}

/// Fire NEWSDIRLIST. `target` NULL = root fetch (populate `root_store`).
fn fetch_dirlist(target: *mut c_void) {
    unsafe {
        if !threaded_news_available() {
            return;
        }
        // Pass the node's path pointer straight through — HxNewsNode paths are
        // byte-oriented (may be non-UTF8), so a cstr()/cs() round-trip would be
        // lossy. Only the root case synthesizes "/" (path_to_hldir derefs it
        // unconditionally, so it can't be NULL).
        let path = if !target.is_null() && !hx_news_node_path(target.cast()).is_null() {
            hx_news_node_path(target.cast())
        } else {
            c"/".as_ptr()
        };
        let stub = gnews_folder_new(path);
        let val: Option<glib::Object> = if target.is_null() {
            None
        } else {
            Some(from_glib_none(target as *mut glib::gobject_ffi::GObject))
        };
        PENDING_DIRLISTS.with(|t| t.borrow_mut().insert(stub as usize, val));

        // Mark loaded before the wire call so a quick collapse+reexpand doesn't
        // re-fire; the reply appends into the existing store.
        if !target.is_null() {
            hx_news_node_set_loaded(target.cast(), glib::ffi::GTRUE);
        }
        hx_news15_fldr_list(gtkhx_active_htlc(), stub);
    }
}

/// Fire NEWSCATLIST for a category node whose children store receives the posts.
fn fetch_catlist(target: *mut c_void) {
    unsafe {
        if target.is_null() || hx_news_node_path(target.cast()).is_null() {
            return;
        }
        // Byte-oriented path — pass the pointer straight through (no lossy
        // cstr()/cs() round-trip).
        let stub = gnews_catalog_new(hx_news_node_path(target.cast()));
        let val: Option<glib::Object> =
            Some(from_glib_none(target as *mut glib::gobject_ffi::GObject));
        PENDING_CATLISTS.with(|t| t.borrow_mut().insert(stub as usize, val));
        hx_news_node_set_loaded(target.cast(), glib::ffi::GTRUE);
        hx_news15_cat_list(gtkhx_active_htlc(), stub);
    }
}

/// Issue GETTHREAD for `target`. A transfer-full ref rides the reply task
/// straight to `gnews_browser_handle_thread`.
fn fetch_thread(target: *mut c_void) {
    unsafe {
        if target.is_null()
            || hx_news_node_kind(target.cast()) != NB_KIND_POST
            || hx_news_node_path(target.cast()).is_null()
        {
            return;
        }
        if hx_news_node_body_fetching(target.cast()) != 0 {
            return; // already in flight
        }
        hx_news_node_set_body_fetching(target.cast(), glib::ffi::GTRUE);

        let mt_ptr = hx_news_node_mime_type(target.cast());
        let fallback = crate::cs("text/plain");
        let mt_arg = if mt_ptr.is_null() { fallback.as_ptr() } else { mt_ptr };

        glib::gobject_ffi::g_object_ref(target as *mut glib::gobject_ffi::GObject);
        hx_news15_get_post(
            gtkhx_active_htlc(),
            hx_news_node_path(target.cast()),
            hx_news_node_postid(target.cast()),
            mt_arg,
            target,
        );
    }
}

// ---------- Reply handlers (called from gtkhx.c signal adapters) ----------

/// NEWSDIRLIST reply. Builds the folder / category children from the parse
/// handle stashed on the carrier by the Rust receive path. Always frees the
/// parse handle + carrier and returns TRUE — the browser is the only producer,
/// so a missing pending entry is a should-never-happen we still clean up after
/// rather than leak (the gtkhx.c adapter ignores the return value anyway).
///
/// # Safety
/// C-ABI entry on the GTK main thread; `gfnews_p` is a `gnews_folder *` carrier.
#[no_mangle]
pub unsafe extern "C" fn gnews_browser_handle_dirlist(gfnews_p: *mut c_void) -> glib::ffi::gboolean {
    let entry = PENDING_DIRLISTS.with(|t| t.borrow_mut().remove(&(gfnews_p as usize)));
    let parsed = gnews_folder_parsed(gfnews_p);

    // Build only when the reply matches a pending fetch and the browser is
    // still alive; otherwise fall through to the free below so nothing leaks.
    if let Some(target) = entry {
        NEWS_BROWSER.with(|b| {
            if let Some(br) = b.borrow().as_ref() {
                match &target {
                    None => {
                        hx_news_build_dirlist_from_dirlist(
                            br.root_store.as_ptr(),
                            c"/".as_ptr(),
                            parsed.cast(),
                        );
                    }
                    Some(node) => {
                        let np = node.as_ptr() as *mut c_void;
                        let dest = hx_news_node_children(np.cast());
                        if !dest.is_null() {
                            // Raw byte-oriented path pointer (NULL is fine — the
                            // builder falls back to the root "/").
                            hx_news_build_dirlist_from_dirlist(
                                dest,
                                hx_news_node_path(np.cast()),
                                parsed.cast(),
                            );
                        }
                    }
                }
            }
        });
    }

    gtkhx_proto_dirlist_free(parsed.cast());
    gnews_folder_free(gfnews_p);
    glib::ffi::GTRUE
}

/// NEWSCATLIST reply. Threads the posts under the target category. Always frees
/// the parse handle + carrier and returns TRUE (see the dirlist handler on the
/// missing-entry cleanup).
///
/// # Safety
/// C-ABI entry on the GTK main thread; `gcnews_p` is a `gnews_catalog *`.
#[no_mangle]
pub unsafe extern "C" fn gnews_browser_handle_catlist(gcnews_p: *mut c_void) -> glib::ffi::gboolean {
    let entry = PENDING_CATLISTS.with(|t| t.borrow_mut().remove(&(gcnews_p as usize)));
    let parsed = gnews_catalog_parsed(gcnews_p);

    // Build only when found (with a target node) and the browser is alive; free
    // below regardless.
    if let Some(Some(node)) = entry {
        let np = node.as_ptr() as *mut c_void;
        let ch = hx_news_node_children(np.cast());
        NEWS_BROWSER.with(|b| {
            if b.borrow().as_ref().is_some() && !ch.is_null() {
                // Raw byte-oriented path pointer (no lossy cstr()/cs() round-trip).
                hx_news_build_category_tree_from_catlist(ch, hx_news_node_path(np.cast()), parsed.cast());
            }
        });
    }

    gtkhx_proto_catlist_free(parsed.cast());
    gnews_catalog_free(gcnews_p);
    glib::ffi::GTRUE
}

/// GETTHREAD reply. Sets the fetched body on the target node and, if it's still
/// the selected post, pushes it into the view.
///
/// # Safety
/// C-ABI entry on the GTK main thread; `post_p` is a `news_post *` carrier.
#[no_mangle]
pub unsafe extern "C" fn gnews_browser_handle_thread(post_p: *mut c_void) -> glib::ffi::gboolean {
    if post_p.is_null() {
        return glib::ffi::GFALSE;
    }

    // post->target carries the transfer-full ref fetch_thread took; released here.
    let target = news_post_target(post_p);
    if !target.is_null() {
        hx_news_node_set_body_fetching(target.cast(), glib::ffi::GFALSE);
        // news_post_body() is already a valid NUL-terminated C string that
        // hx_news_node_set_body copies — pass it straight through (empty for
        // NULL) to preserve the original bytes, no lossy cstr()/cs() round-trip.
        let body_ptr = news_post_body(post_p);
        let body_ptr = if body_ptr.is_null() { c"".as_ptr() } else { body_ptr };
        hx_news_node_set_body(target.cast(), body_ptr);

        with_browser(|br| {
            if br.selected_post.get() == target {
                render_selected_post(br);
            }
        });
        glib::gobject_ffi::g_object_unref(target as *mut glib::gobject_ffi::GObject);
    }

    news_post_free(post_p);
    glib::ffi::GTRUE
}

// ---------- Refresh ----------

/// Clear `node`'s children + reset loaded + refire the matching fetch. NULL =
/// root refresh.
fn refresh_node(br: &NewsBrowser, node: *mut c_void) {
    unsafe {
        if node.is_null() {
            br.root_store.remove_all();
            fetch_dirlist(std::ptr::null_mut());
            return;
        }
        let ch = hx_news_node_children(node.cast());
        if !ch.is_null() {
            let store: gio::ListStore = from_glib_none(ch);
            store.remove_all();
        }
        hx_news_node_set_loaded(node.cast(), glib::ffi::GFALSE);
        match hx_news_node_kind(node.cast()) {
            NB_KIND_FOLDER => fetch_dirlist(node),
            NB_KIND_CATEGORY => fetch_catlist(node),
            _ => {}
        }
    }
}

/// Find the category node owning `path`, walking the loaded tree only (no fetch
/// on miss). Returns a reffed node or `None`.
fn find_category_node(store: &gio::ListStore, path: &str) -> Option<glib::Object> {
    let n = store.n_items();
    for i in 0..n {
        let node = store.item(i)?;
        let np = node.as_ptr() as *mut c_void;
        unsafe {
            if hx_news_node_kind(np.cast()) == NB_KIND_CATEGORY
                && crate::cstr(hx_news_node_path(np.cast())) == path
            {
                return Some(node);
            }
            if hx_news_node_kind(np.cast()) == NB_KIND_FOLDER {
                let ch = hx_news_node_children(np.cast());
                if !ch.is_null() {
                    let ch_store: gio::ListStore = from_glib_none(ch);
                    if let Some(hit) = find_category_node(&ch_store, path) {
                        return Some(hit);
                    }
                }
            }
        }
    }
    None
}

// ---------- Connection state / panel present ----------

fn reset_browser_state(br: &NewsBrowser) {
    br.root_store.remove_all();
    br.selected_post.set(std::ptr::null_mut());
    br.post_view
        .buffer()
        .set_text(&tr("Select a post in the tree to view it here."));
    unsafe { gtkurl_textview_apply_tags(br.post_view.as_ptr()) };
    br.header_strip.set_visible(false);
    br.breadcrumb.set_text("/");
    sync_action_buttons(br);
}

fn on_connection_state(state: u32) {
    with_browser(|br| match state {
        GTKHX_CONNECTION_DISCONNECTED => {
            reset_browser_state(br);
            br.disconnected_banner.set_revealed(true);
        }
        GTKHX_CONNECTION_LOGIN_READY => {
            br.disconnected_banner.set_revealed(false);
            if br.root_store.n_items() == 0 {
                fetch_dirlist(std::ptr::null_mut());
            }
        }
        _ => {}
    });
}

fn on_panel_presented() {
    if unsafe { gtkhx_active_connected() } == 0 {
        return;
    }
    with_browser(|br| {
        if br.root_store.n_items() == 0 {
            fetch_dirlist(std::ptr::null_mut());
        }
    });
}

// ---------- Toolbar callbacks (C-ABI: connected via gtkhx_pixmap_button) ----------

unsafe extern "C" fn on_refresh_clicked(_btn: *mut gtk::ffi::GtkButton, _u: *mut c_void) {
    with_browser(|br| {
        let node = selected_node(br);
        // Post selected → refetch its body. Otherwise refresh the folder /
        // category, or the root when nothing suitable is selected.
        if !node.is_null() && hx_news_node_kind(node.cast()) == NB_KIND_POST {
            hx_news_node_set_body(node.cast(), std::ptr::null());
            hx_news_node_set_body_fetching(node.cast(), glib::ffi::GFALSE);
            render_selected_post(br);
            return;
        }
        if !node.is_null()
            && (hx_news_node_kind(node.cast()) == NB_KIND_FOLDER
                || hx_news_node_kind(node.cast()) == NB_KIND_CATEGORY)
        {
            refresh_node(br, node);
        } else {
            refresh_node(br, std::ptr::null_mut());
        }
    });
}

unsafe extern "C" fn on_new_folder_clicked(_btn: *mut gtk::ffi::GtkButton, _u: *mut c_void) {
    with_browser(|br| {
        let sel = selected_node(br);
        let parent = if !sel.is_null() && hx_news_node_kind(sel.cast()) == NB_KIND_FOLDER {
            sel
        } else {
            std::ptr::null_mut()
        };
        crate::news_dialogs::gtkhx_news_create_dialog_open(
            br.window.as_ptr() as *mut gtk::ffi::GtkWidget,
            parent,
            NB_KIND_FOLDER as c_int,
        );
    });
}

unsafe extern "C" fn on_new_category_clicked(_btn: *mut gtk::ffi::GtkButton, _u: *mut c_void) {
    with_browser(|br| {
        let sel = selected_node(br);
        let parent = if !sel.is_null() && hx_news_node_kind(sel.cast()) == NB_KIND_FOLDER {
            sel
        } else {
            std::ptr::null_mut()
        };
        crate::news_dialogs::gtkhx_news_create_dialog_open(
            br.window.as_ptr() as *mut gtk::ffi::GtkWidget,
            parent,
            NB_KIND_CATEGORY as c_int,
        );
    });
}

unsafe extern "C" fn on_new_post_clicked(_btn: *mut gtk::ffi::GtkButton, _u: *mut c_void) {
    with_browser(|br| {
        let sel = selected_node(br);
        if sel.is_null() {
            return;
        }
        let kind = hx_news_node_kind(sel.cast());
        if kind != NB_KIND_CATEGORY && kind != NB_KIND_POST {
            return;
        }
        let cat_path = hx_news_node_path(sel.cast());
        if cat_path.is_null() {
            return;
        }
        let empty = crate::cs("");
        crate::news_compose::gtkhx_news_compose_open(cat_path, std::ptr::null_mut(), empty.as_ptr());
    });
}

unsafe extern "C" fn on_reply_clicked(_btn: *mut gtk::ffi::GtkButton, _u: *mut c_void) {
    with_browser(|br| {
        let sel = selected_node(br);
        if sel.is_null()
            || hx_news_node_kind(sel.cast()) != NB_KIND_POST
            || hx_news_node_path(sel.cast()).is_null()
        {
            return;
        }

        // Prefill "Re: <original>" without stacking Re: Re: chains.
        let sel_name = crate::cstr(hx_news_node_name(sel.cast()));
        let subj = if sel_name.to_ascii_lowercase().starts_with("re:") {
            sel_name.clone()
        } else {
            format!("Re: {sel_name}")
        };

        // Make sure the original body is loading before the context card renders.
        if hx_news_node_body(sel.cast()).is_null() && hx_news_node_body_fetching(sel.cast()) == 0 {
            fetch_thread(sel);
        }

        let subj_c = crate::cs(&subj);
        crate::news_compose::gtkhx_news_compose_open(hx_news_node_path(sel.cast()), sel, subj_c.as_ptr());
    });
}

unsafe extern "C" fn on_delete_clicked(_btn: *mut gtk::ffi::GtkButton, _u: *mut c_void) {
    with_browser(|br| {
        let sel = selected_node(br);
        if sel.is_null() || hx_news_node_path(sel.cast()).is_null() {
            return;
        }
        crate::news_dialogs::gtkhx_news_delete_dialog_open(
            br.window.as_ptr() as *mut gtk::ffi::GtkWidget,
            hx_news_node_kind(sel.cast()) as c_int,
            hx_news_node_name(sel.cast()),
            hx_news_node_path(sel.cast()),
            hx_news_node_postid(sel.cast()),
        );
    });
}

// ---------- Bridges the sibling Rust modules call back into ----------

/// Per-kind row icon (borrowed) for the `news_tree` factory. `_browser` is the
/// opaque pointer the factory carries; we use the process singleton.
///
/// # Safety
/// C-ABI entry on the GTK main thread.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_news_icon_for_kind(
    _browser: *mut c_void,
    kind: i32,
) -> *mut gdk::ffi::GdkPaintable {
    with_browser(|br| {
        let icon = match kind {
            NB_KIND_FOLDER => br.icon_folder.as_ref(),
            NB_KIND_CATEGORY => br.icon_category.as_ref(),
            NB_KIND_POST => br.icon_post.as_ref(),
            _ => None,
        };
        icon.map(|p| p.as_ptr())
            .unwrap_or(std::ptr::null_mut())
    })
    .unwrap_or(std::ptr::null_mut())
}

/// Fire the DIRLIST / CATLIST fetch the first time a folder / category expands.
///
/// # Safety
/// C-ABI entry on the GTK main thread; `node` is an `HxNewsNode *`.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_news_fetch_for_expanded(_browser: *mut c_void, node: *mut c_void) {
    if node.is_null() || hx_news_node_loaded(node.cast()) != 0 {
        return;
    }
    match hx_news_node_kind(node.cast()) {
        NB_KIND_FOLDER => fetch_dirlist(node),
        NB_KIND_CATEGORY => fetch_catlist(node),
        _ => {}
    }
}

/// Cache-miss body fetch the `news_render` post renderer calls back into.
///
/// # Safety
/// C-ABI entry on the GTK main thread; `node` is an `HxNewsNode *`.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_news_fetch_thread(_browser: *mut c_void, node: *mut c_void) {
    fetch_thread(node);
}

/// Re-fetch a listing after a create / delete (no server push). NULL = root.
///
/// # Safety
/// C-ABI entry on the GTK main thread; `node` is NULL or an `HxNewsNode *`.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_news_browser_refresh(node: *mut c_void) {
    with_browser(|br| refresh_node(br, node));
}

/// Settle after a post: refetch just the affected category if still in the tree,
/// else the whole root.
///
/// # Safety
/// C-ABI entry on the GTK main thread; `path` is a valid C string or NULL.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_news_refresh_category(path: *const c_char) {
    if path.is_null() {
        return;
    }
    let path = crate::cstr(path);
    with_browser(|br| match find_category_node(&br.root_store, &path) {
        Some(cat) => refresh_node(br, cat.as_ptr() as *mut c_void),
        None => refresh_node(br, std::ptr::null_mut()),
    });
}

// ---------- Construction ----------

/// A `gtkhx_pixmap_button` wrapping the C leaf, returning an owned `gtk::Widget`.
unsafe fn pixmap_button(
    resource: &str,
    tooltip: &str,
    cb: unsafe extern "C" fn(*mut gtk::ffi::GtkButton, *mut c_void),
) -> gtk::Widget {
    let res = crate::cs(resource);
    let tip = crate::cs(tooltip);
    let w = crate::ffi::gtkhx_pixmap_button(
        res.as_ptr(),
        tip.as_ptr(),
        GTKHX_SCALE_WINDOW_BUTTONS,
        cb as *const c_void,
        std::ptr::null_mut(),
    );
    from_glib_none(w)
}

/// Build the whole browser + content tree, register it as the process
/// singleton, and return the content box (transfer to the dock).
fn build_content() -> gtk::Widget {
    crate::ensure_gtk_init();

    // ---- Cached row icons ----
    let icon_folder = load_icon("/com/nasledov/gtkhx/pixmaps/news_folder.png");
    let icon_category = load_icon("/com/nasledov/gtkhx/pixmaps/news_category.png");
    let icon_post = load_icon("/com/nasledov/gtkhx/pixmaps/news_post.png");

    // ---- Breadcrumb (its own row; the panel header holds the tab title) ----
    let breadcrumb = gtk::Label::new(Some("/"));
    breadcrumb.add_css_class("heading");
    breadcrumb.set_xalign(0.0);
    breadcrumb.set_margin_start(12);
    breadcrumb.set_margin_end(12);
    breadcrumb.set_margin_top(4);
    breadcrumb.set_margin_bottom(4);

    // ---- Action buttons ----
    let (btn_refresh, btn_new_folder, btn_new_category, btn_new_post, btn_reply, btn_delete) = unsafe {
        (
            pixmap_button("/com/nasledov/gtkhx/pixmaps/refresh.png", &tr("Refresh"), on_refresh_clicked),
            pixmap_button("/com/nasledov/gtkhx/pixmaps/news_folder.png", &tr("New Folder"), on_new_folder_clicked),
            pixmap_button("/com/nasledov/gtkhx/pixmaps/news_category.png", &tr("New Category"), on_new_category_clicked),
            pixmap_button("/com/nasledov/gtkhx/pixmaps/pencil.png", &tr("New Post"), on_new_post_clicked),
            pixmap_button("/com/nasledov/gtkhx/pixmaps/post_news.png", &tr("Reply"), on_reply_clicked),
            pixmap_button("/com/nasledov/gtkhx/pixmaps/trash.png", &tr("Delete"), on_delete_clicked),
        )
    };

    let button_bar = gtk::Box::new(gtk::Orientation::Horizontal, 4);
    button_bar.set_margin_start(6);
    button_bar.set_margin_end(6);
    button_bar.set_margin_top(6);
    button_bar.set_margin_bottom(4);
    button_bar.append(&btn_refresh);
    button_bar.append(&btn_new_folder);
    button_bar.append(&btn_new_category);
    button_bar.append(&btn_new_post);
    let spacer = gtk::Label::new(None);
    spacer.set_hexpand(true);
    button_bar.append(&spacer);
    button_bar.append(&btn_reply);
    button_bar.append(&btn_delete);

    // ---- Two-pane body ----
    let paned = gtk::Paned::new(gtk::Orientation::Horizontal);
    paned.set_position(280);
    paned.set_resize_start_child(false);
    paned.set_hexpand(true);
    paned.set_vexpand(true);

    // Left: GtkListView over a GtkTreeListModel (factory + child model in Rust).
    let node_type: glib::Type = unsafe { from_glib(hx_news_node_get_type()) };
    let root_store = gio::ListStore::with_type(node_type);
    // Consumes one root_store ref (transfer full), like gtk_tree_list_model_new;
    // our own root_store ref stays as the borrowed alias.
    let root_model: gio::ListModel = root_store.clone().upcast();
    let tree_model: gtk::TreeListModel = unsafe {
        from_glib_full(crate::news_tree::gtkhx_news_build_tree_model(
            root_model.into_glib_ptr(),
        ))
    };
    let selection = gtk::SingleSelection::new(Some(tree_model.clone()));
    selection.set_autoselect(false);
    selection.set_can_unselect(true);
    let factory: gtk::ListItemFactory = unsafe {
        from_glib_full(crate::news_tree::gtkhx_news_build_factory(
            std::ptr::null_mut(),
        ))
    };
    let list_view = gtk::ListView::new(Some(selection.clone()), Some(factory));
    list_view.set_show_separators(false);
    unsafe {
        crate::ffi::gtkhx_apply_listview_style(list_view.as_ptr() as *mut gtk::ffi::GtkWidget)
    };
    selection.connect_selection_changed(|_sel, _pos, _n| on_selection_changed());

    let left_scroll = gtk::ScrolledWindow::new();
    left_scroll.set_policy(gtk::PolicyType::Automatic, gtk::PolicyType::Automatic);
    left_scroll.set_child(Some(&list_view));
    paned.set_start_child(Some(&left_scroll));

    // Right: post header strip + body.
    let right_box = gtk::Box::new(gtk::Orientation::Vertical, 0);
    let header_strip = gtk::Box::new(gtk::Orientation::Vertical, 2);
    header_strip.set_margin_start(12);
    header_strip.set_margin_end(12);
    header_strip.set_margin_top(10);
    header_strip.set_margin_bottom(8);

    let subject_label = gtk::Label::new(None);
    subject_label.set_xalign(0.0);
    subject_label.set_wrap(true);
    subject_label.set_wrap_mode(pango::WrapMode::WordChar);
    subject_label.add_css_class("heading");
    let meta_label = gtk::Label::new(None);
    meta_label.set_xalign(0.0);
    meta_label.set_ellipsize(pango::EllipsizeMode::End);
    meta_label.add_css_class("dim-label");
    header_strip.append(&subject_label);
    header_strip.append(&meta_label);
    header_strip.set_visible(false);
    right_box.append(&header_strip);
    right_box.append(&gtk::Separator::new(gtk::Orientation::Horizontal));

    let right_scroll = gtk::ScrolledWindow::new();
    right_scroll.set_policy(gtk::PolicyType::Automatic, gtk::PolicyType::Automatic);
    let post_view = gtk::TextView::new();
    post_view.set_editable(false);
    post_view.set_cursor_visible(false);
    post_view.set_wrap_mode(gtk::WrapMode::WordChar);
    post_view.set_margin_start(12);
    post_view.set_margin_end(12);
    post_view.set_margin_top(10);
    post_view.set_margin_bottom(10);
    unsafe { crate::ffi::gtkhx_apply_text_style(post_view.as_ptr() as *mut gtk::ffi::GtkWidget) };
    post_view
        .buffer()
        .set_text(&tr("Select a post in the tree to view it here."));
    unsafe { gtkurl_textview_install(post_view.as_ptr()) };
    right_scroll.set_child(Some(&post_view));
    right_scroll.set_vexpand(true);
    right_box.append(&right_scroll);
    paned.set_end_child(Some(&right_box));

    // ---- Disconnected banner + content assembly ----
    let connected = unsafe { gtkhx_active_connected() } != 0;
    let disconnected_banner = adw::Banner::new(&tr("Not connected to a server."));
    disconnected_banner.set_revealed(!connected);

    let content_vbox = gtk::Box::new(gtk::Orientation::Vertical, 0);
    content_vbox.append(&disconnected_banner);
    content_vbox.append(&button_bar);
    content_vbox.append(&breadcrumb);
    content_vbox.append(&gtk::Separator::new(gtk::Orientation::Horizontal));
    content_vbox.append(&paned);

    let window: gtk::Widget = content_vbox.clone().upcast();
    let browser = NewsBrowser {
        window: window.clone(),
        root_store,
        tree_model,
        selection,
        icon_folder,
        icon_category,
        icon_post,
        post_view,
        subject_label,
        meta_label,
        header_strip: header_strip.upcast(),
        breadcrumb,
        btn_new_folder,
        btn_new_category,
        btn_new_post,
        btn_reply,
        btn_delete,
        disconnected_banner,
        selected_post: Cell::new(std::ptr::null_mut()),
        conn_handler: RefCell::new(None),
    };
    NEWS_BROWSER.with(|b| *b.borrow_mut() = Some(browser));

    // Initial button state (no selection → New Folder + New Category visible).
    with_browser(sync_action_buttons);

    // Connection-state changes drive the banner + LOGIN_READY auto-fetch.
    let session_obj: glib::Object =
        unsafe { from_glib_none(gtkhx_session_get_default() as *mut glib::gobject_ffi::GObject) };
    let id = session_obj.connect_local("connection-state-changed", false, |vals| {
        let state = vals.get(1).and_then(|v| v.get::<u32>().ok()).unwrap_or(0);
        on_connection_state(state);
        None
    });
    with_browser(|br| *br.conn_handler.borrow_mut() = Some(id));

    // Teardown on content destroy (embed failure / app exit): drop the session
    // handler + the singleton so no stale callback fires into a dead browser.
    content_vbox.connect_destroy(|_w| {
        with_browser(|br| {
            if let Some(id) = br.conn_handler.borrow_mut().take() {
                let s: glib::Object = unsafe {
                    from_glib_none(gtkhx_session_get_default() as *mut glib::gobject_ffi::GObject)
                };
                s.disconnect(id);
            }
        });
        NEWS_BROWSER.with(|b| *b.borrow_mut() = None);
    });

    window
}

/// Wire the one panel-level hook (`PanelWidget::presented`) once the dock has
/// embedded us — a tab switch onto News while connected + empty auto-fetches.
fn after_embed() {
    let id = crate::cs(HX_ID_NEWS15);
    let panel = unsafe { hx_panel_registry_lookup(id.as_ptr()) };
    if panel.is_null() || with_browser(|_| ()).is_none() {
        return;
    }
    let panel_obj: glib::Object =
        unsafe { from_glib_none(panel as *mut glib::gobject_ffi::GObject) };
    panel_obj.connect_local("presented", false, |_vals| {
        on_panel_presented();
        None
    });
}

/// Open (or raise) the News-browser panel: build + dock the content, or
/// re-attach if already open.
///
/// # Safety
/// C-ABI entry on the GTK main thread. `_widget` / `_sess` are vestigial (the
/// browser is a process singleton).
#[no_mangle]
pub unsafe extern "C" fn create_news_browser_window(_widget: *mut c_void, _sess: *mut c_void) {
    crate::ensure_gtk_init();

    if dock::raise_if_open(HX_ID_NEWS15) {
        return;
    }

    let content = build_content();
    if dock::embed(
        HX_ID_NEWS15,
        dock::KIND_CENTER,
        dock::AREA_CENTER,
        "News (1.5+)",
        "text-x-generic-symbolic",
        content.into_glib_ptr(),
    ) {
        after_embed();
    }
}

/// Toolbar entry point for the News (1.5+) button: build/raise the panel, wire
/// the keyaccels on first open, and pull a fresh tree while connected.
///
/// # Safety
/// C-ABI entry on the GTK main thread.
#[no_mangle]
pub unsafe extern "C" fn open_news_browser(widget: *mut c_void, sess: *mut c_void) {
    let id = crate::cs(HX_ID_NEWS15);
    let was_open = !hx_panel_registry_lookup(id.as_ptr()).is_null();

    create_news_browser_window(widget, sess);

    if !was_open {
        // Freshly built: Ctrl+Q / Ctrl+K / Ctrl+T on the content box.
        with_browser(|br| {
            crate::ffi::init_keyaccel(br.window.as_ptr() as *mut gtk::ffi::GtkWidget)
        });
    }

    if gtkhx_active_connected() != 0 {
        with_browser(|br| {
            if was_open {
                br.root_store.remove_all();
            }
            fetch_dirlist(std::ptr::null_mut());
        });
    }
}
