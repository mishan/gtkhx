//! The 1.5 threaded-news reply carriers, owned in Rust.
//!
//! These are the small structs that thread a news fetch from the browser
//! through the send + receive paths and back:
//!
//! - `Carrier` (folder / catalog) — created by the browser with the request
//!   `path`, waiting with the request for its reply; the sender reads `path`,
//!   the receive side stashes what the session read of the reply, and the
//!   browser's reply handler reads that and frees the carrier.
//! - `NewsPost` — built per GETTHREAD reply: the post body + the target
//!   `HxNewsNode` (carrying a transfer-full ref); consumed + freed by the
//!   browser's thread handler.
//!
//! They used to be C structs in `session.h` reached through `news_send_bridge.c`
//! and `news_recv_bridge.c` shims. They're pure Rust-owned boxes now; the only
//! thing that crosses the FFI is the opaque `*mut c_void` handle (C never
//! dereferences it — `gtkhx.c` passes it straight through). The
//! `#[no_mangle]` accessors keep the exact symbol names the browser (gtkhx-ui)
//! and the senders (hxhandlers::send::news) already link against.

use std::ffi::{c_char, c_void, CStr, CString};

use hxmodel::news::node::hx_news_node_set_body_fetching;
use hxsession::{Article, NewsItem};

extern "C" {
    // glib — release the transfer-full target ref a refused fetch won't carry
    // onward.
    fn g_object_unref(obj: *mut c_void);
}

/// A NEWSDIRLIST / NEWSCATLIST request carrier: the path we asked for plus
/// what the reply listed, a bundle's items or a category's articles.
struct Carrier {
    path: CString,
    items: Vec<NewsItem>,
    articles: Vec<Article>,
}

unsafe fn carrier_new(path: *const c_char) -> *mut c_void {
    let path = if path.is_null() {
        CString::default()
    } else {
        CStr::from_ptr(path).to_owned()
    };
    Box::into_raw(Box::new(Carrier {
        path,
        items: Vec::new(),
        articles: Vec::new(),
    })) as *mut c_void
}

unsafe fn carrier_path(g: *mut c_void) -> *const c_char {
    if g.is_null() {
        std::ptr::null()
    } else {
        (*(g as *mut Carrier)).path.as_ptr()
    }
}

unsafe fn carrier_free(g: *mut c_void) {
    if !g.is_null() {
        drop(Box::from_raw(g as *mut Carrier));
    }
}

// Folder + catalog carriers share the identical shape; two symbol sets keep the
// call sites self-documenting (and match the two distinct reply signals).

/// # Safety
/// `path` is NULL or a valid C string; the returned handle is freed by
/// `gnews_folder_free`.
#[no_mangle]
pub unsafe extern "C" fn gnews_folder_new(path: *const c_char) -> *mut c_void {
    carrier_new(path)
}
/// # Safety
/// `g` is NULL or a `gnews_folder_new` handle.
#[no_mangle]
pub unsafe extern "C" fn gnews_folder_path(g: *mut c_void) -> *const c_char {
    carrier_path(g)
}
/// What the bundle holds, once its reply has come; empty until then.
///
/// # Safety
/// `g` is a live `gnews_folder_new` handle, which outlives the slice.
pub unsafe fn gnews_folder_items<'a>(g: *mut c_void) -> &'a [NewsItem] {
    &(*(g as *mut Carrier)).items
}
/// # Safety
/// `g` is a live `gnews_folder_new` handle.
pub(crate) unsafe fn gnews_folder_set_items(g: *mut c_void, items: Vec<NewsItem>) {
    (*(g as *mut Carrier)).items = items;
}
/// # Safety
/// `g` is NULL or a `gnews_folder_new` handle; not used afterward.
#[no_mangle]
pub unsafe extern "C" fn gnews_folder_free(g: *mut c_void) {
    carrier_free(g);
}

/// # Safety
/// `path` is NULL or a valid C string; the returned handle is freed by
/// `gnews_catalog_free`.
#[no_mangle]
pub unsafe extern "C" fn gnews_catalog_new(path: *const c_char) -> *mut c_void {
    carrier_new(path)
}
/// # Safety
/// `g` is NULL or a `gnews_catalog_new` handle.
#[no_mangle]
pub unsafe extern "C" fn gnews_catalog_path(g: *mut c_void) -> *const c_char {
    carrier_path(g)
}
/// The category's articles, once its reply has come; empty until then.
///
/// # Safety
/// `g` is a live `gnews_catalog_new` handle, which outlives the slice.
pub unsafe fn gnews_catalog_articles<'a>(g: *mut c_void) -> &'a [Article] {
    &(*(g as *mut Carrier)).articles
}
/// # Safety
/// `g` is a live `gnews_catalog_new` handle.
pub(crate) unsafe fn gnews_catalog_set_articles(g: *mut c_void, articles: Vec<Article>) {
    (*(g as *mut Carrier)).articles = articles;
}
/// # Safety
/// `g` is NULL or a `gnews_catalog_new` handle; not used afterward.
#[no_mangle]
pub unsafe extern "C" fn gnews_catalog_free(g: *mut c_void) {
    carrier_free(g);
}

/// The GETTHREAD reply carrier: the post body + the target node ref.
struct NewsPost {
    buf: CString,
    target: *mut c_void,
}

/// Build a `news_post` from a parsed body. Mirrors the old `g_strndup`: copies
/// up to `body_len` bytes, truncating at the first NUL.
///
/// # Safety
/// `body` is NULL or valid for `body_len` bytes; `target` is the transfer-full
/// `HxNewsNode *`. The handle is freed by `news_post_free`.
#[no_mangle]
pub unsafe extern "C" fn news_post_new(
    target: *mut c_void,
    body: *const u8,
    body_len: usize,
) -> *mut c_void {
    let bytes = if body.is_null() {
        &[][..]
    } else {
        std::slice::from_raw_parts(body, body_len)
    };
    let end = bytes.iter().position(|&b| b == 0).unwrap_or(bytes.len());
    let buf = CString::new(&bytes[..end]).unwrap_or_default();
    Box::into_raw(Box::new(NewsPost { buf, target })) as *mut c_void
}

/// # Safety
/// `post` is NULL or a `news_post_new` handle.
#[no_mangle]
pub unsafe extern "C" fn news_post_target(post: *mut c_void) -> *mut c_void {
    if post.is_null() {
        std::ptr::null_mut()
    } else {
        (*(post as *mut NewsPost)).target
    }
}

/// # Safety
/// `post` is NULL or a `news_post_new` handle. The returned string is valid
/// until `news_post_free`.
#[no_mangle]
pub unsafe extern "C" fn news_post_body(post: *mut c_void) -> *const c_char {
    if post.is_null() {
        std::ptr::null()
    } else {
        (*(post as *mut NewsPost)).buf.as_ptr()
    }
}

/// # Safety
/// `post` is NULL or a `news_post_new` handle; not used afterward.
#[no_mangle]
pub unsafe extern "C" fn news_post_free(post: *mut c_void) {
    if !post.is_null() {
        drop(Box::from_raw(post as *mut NewsPost));
    }
}

/// A GETTHREAD that was refused, or never answered: release the
/// transfer-full target ref + clear body_fetching so the user can retry. No
/// `news_post` is built in this case.
///
/// # Safety
/// `target` is NULL or the transfer-full `HxNewsNode *`.
#[no_mangle]
pub unsafe extern "C" fn news_post_fetch_failed(target: *mut c_void) {
    if !target.is_null() {
        hx_news_node_set_body_fetching(target.cast(), 0);
        g_object_unref(target);
    }
}
