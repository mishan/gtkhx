//! `hxhandlers::send::news` — the news senders: flat NEWS_GETFILE /
//! NEWS_POST, and threaded news's DIRLIST / CATLIST listings, GETTHREAD,
//! POSTTHREAD, DELETETHREAD, DELNEWSDIRCAT, MAKECATEGORY and MAKENEWSDIR.
//!
//! Each request is built by `hxrequest::news`, which the end-to-end suite
//! drives against real servers; what is left here is the C ABI the news
//! views call (`hx_news15_*`, `hx_get_news`, `hx_post_news`), the session
//! expecting each reply, and the send. A fetch's reply is matched to what
//! asked for it (`recv::news`) by the trans it went out on; a change's reply
//! says nothing unless the server refused, which `request-failed` reports.
//!
//! The `cat_list` / `fldr_list` senders take an opaque reply carrier (the
//! Rust-owned `gnews_catalog` / `gnews_folder` in hxhandlers::recv::news); they
//! read its request path through the `gnews_*_path` accessor. Paths go as
//! the bytes they hold: a listing's, which name a thing back exactly.

use std::ffi::{c_char, c_void, CStr};
use std::os::raw::c_int;

use hxproto::build::HxChunk;
use hxrequest::{news, Request};
use hxsession::Expect;

use super::expect_next;
use crate::recv::news::{asked, Asked};

// Real build: these resolve at the final C link. Test build: `use tests::{…}`
// below shadows them with recording stubs, so the extern block is gated off.
#[cfg(not(test))]
use crate::recv::news::carrier::{gnews_catalog_path, gnews_folder_path};

#[cfg(not(test))]
extern "C" {
    // chat_send_bridge.c — per-htlc CAP_TEXT_ENCODING probe (shared with the
    // chat senders).
    fn hx_htlc_text_encoding_cap(htlc: *mut c_void) -> glib::ffi::gboolean;

    // hxtask — the send-path primitive.
    fn hlwrite_chunks(htlc: *mut c_void, ty: u32, flag: u32, chunks: *const HxChunk, hc: c_int);
}

#[cfg(test)]
use tests::{gnews_catalog_path, gnews_folder_path, hlwrite_chunks, hx_htlc_text_encoding_cap};

/// A NUL-terminated C string's bytes (without the NUL), or empty for NULL.
unsafe fn cstr_bytes<'a>(s: *const c_char) -> &'a [u8] {
    if s.is_null() {
        &[]
    } else {
        CStr::from_ptr(s).to_bytes()
    }
}

unsafe fn utf8(htlc: *mut c_void) -> bool {
    hx_htlc_text_encoding_cap(htlc) != glib::ffi::GFALSE
}

/// Send `req` with its reply expected as `what`; the trans it went out on.
unsafe fn send(htlc: *mut c_void, req: &Request, what: Expect) -> u32 {
    let trans = expect_next(htlc, what);
    req.with_hx_chunks(|chunks| {
        hlwrite_chunks(htlc, req.opcode, 0, chunks.as_ptr(), chunks.len() as c_int)
    });
    trans
}

/// Send a change to news: a post, a deletion, a new bundle or category.
unsafe fn change(htlc: *mut c_void, req: Option<Request>) {
    if let Some(req) = req {
        send(htlc, &req, Expect::NewsChange);
    }
}

/// `void hx_news15_get_post(struct htlc_conn *htlc, const char *path,
/// guint32 postid, const char *mime_type, void *target)` — GETTHREAD: fetch a
/// post's body. The reply carries `target` (the `HxNewsNode *` whose body is
/// being fetched) to `gnews_browser_handle_thread`.
///
/// `target` is **transfer-full**: this takes ownership of one GObject ref. On
/// success it waits with the request for its reply, whose handler unrefs it;
/// on any early-out (bad args, nothing to send) it's released here, so the
/// caller (`fetch_thread`) can hand over a ref and forget.
///
/// # Safety
/// `htlc` is NULL or valid; `path` / `mime_type` are NULL or NUL-terminated;
/// `target` is NULL or a valid `HxNewsNode *` whose ref is transferred here;
/// main thread only.
#[no_mangle]
pub unsafe extern "C" fn hx_news15_get_post(
    htlc: *mut c_void,
    path: *const c_char,
    postid: u32,
    mime_type: *const c_char,
    target: *mut c_void,
) {
    let req = (!htlc.is_null() && !path.is_null())
        .then(|| news::article(cstr_bytes(path), postid, cstr_bytes(mime_type)))
        .flatten();
    match req {
        Some(req) => {
            let trans = send(htlc, &req, Expect::NewsArticle);
            asked(htlc, trans, Asked::Article(target));
        }
        None => release(target),
    }
}

/// Drop a transfer-full GObject ref (no-op on NULL).
unsafe fn release(obj: *mut c_void) {
    if !obj.is_null() {
        glib::gobject_ffi::g_object_unref(obj as *mut glib::gobject_ffi::GObject);
    }
}

/// `void hx_news15_cat_list(struct htlc_conn *htlc, struct gnews_catalog *g)` —
/// NEWSCATLIST: enumerate a category's posts, into `g`.
///
/// # Safety
/// `htlc` / `g` are NULL or valid; main thread only.
#[no_mangle]
pub unsafe extern "C" fn hx_news15_cat_list(htlc: *mut c_void, g: *mut c_void) {
    if htlc.is_null() || g.is_null() {
        return;
    }
    // A node cleared mid-refresh yields a NULL path: nothing to ask for.
    let path = gnews_catalog_path(g);
    if path.is_null() {
        return;
    }
    if let Some(req) = news::category(cstr_bytes(path)) {
        let trans = send(htlc, &req, Expect::NewsCategory);
        asked(htlc, trans, Asked::Catalog(g));
    }
}

/// `void hx_news15_fldr_list(struct htlc_conn *htlc, struct gnews_folder *g)` —
/// NEWSDIRLIST: enumerate a folder's folders+categories, into `g`.
///
/// # Safety
/// `htlc` / `g` are NULL or valid; main thread only.
#[no_mangle]
pub unsafe extern "C" fn hx_news15_fldr_list(htlc: *mut c_void, g: *mut c_void) {
    if htlc.is_null() || g.is_null() {
        return;
    }
    // A node cleared mid-refresh yields a NULL path: nothing to ask for.
    let path = gnews_folder_path(g);
    if path.is_null() {
        return;
    }
    if let Some(req) = news::listing(cstr_bytes(path)) {
        let trans = send(htlc, &req, Expect::NewsListing);
        asked(htlc, trans, Asked::Folder(g));
    }
}

/// `void hx_news15_post_thread(struct htlc_conn *htlc, char *path,
/// const char *subject, guint32 threadid, char *text)` — POSTTHREAD. `threadid`
/// is the post being replied to (0 for a new top-level post).
///
/// # Safety
/// `htlc` is NULL or valid; `path` / `subject` / `text` are NUL-terminated C
/// strings or NULL; main thread only.
#[no_mangle]
pub unsafe extern "C" fn hx_news15_post_thread(
    htlc: *mut c_void,
    path: *const c_char,
    subject: *const c_char,
    threadid: u32,
    text: *const c_char,
) {
    // A node cleared during a refresh yields a NULL path: nothing to send.
    if htlc.is_null() || path.is_null() {
        return;
    }
    let req = news::post_article(
        cstr_bytes(path),
        threadid,
        cstr_bytes(subject),
        cstr_bytes(text),
        utf8(htlc),
    );
    change(htlc, req);
}

/// `void hx_news15_delete_thread(struct htlc_conn *htlc, char *path,
/// guint32 threadid)` — DELETETHREAD.
///
/// # Safety
/// `htlc` is NULL or valid; `path` is a NUL-terminated C string or NULL.
#[no_mangle]
pub unsafe extern "C" fn hx_news15_delete_thread(
    htlc: *mut c_void,
    path: *const c_char,
    threadid: u32,
) {
    if htlc.is_null() || path.is_null() {
        return;
    }
    change(htlc, news::delete_article(cstr_bytes(path), threadid));
}

/// `void hx_news15_delete(struct htlc_conn *htlc, char *path)` — DELNEWSDIRCAT
/// (deletes a folder or category; mhxd inspects the path).
///
/// # Safety
/// `htlc` is NULL or valid; `path` is a NUL-terminated C string or NULL.
#[no_mangle]
pub unsafe extern "C" fn hx_news15_delete(htlc: *mut c_void, path: *const c_char) {
    if htlc.is_null() || path.is_null() {
        return;
    }
    change(htlc, news::delete(cstr_bytes(path)));
}

/// `void hx_news15_mkcat(struct htlc_conn *htlc, char *path, const char *name)`
/// — MAKECATEGORY.
///
/// # Safety
/// `htlc` is NULL or valid; `path` / `name` are NUL-terminated C strings or NULL.
#[no_mangle]
pub unsafe extern "C" fn hx_news15_mkcat(
    htlc: *mut c_void,
    path: *const c_char,
    name: *const c_char,
) {
    if htlc.is_null() || path.is_null() {
        return;
    }
    let req = news::create_category(cstr_bytes(path), cstr_bytes(name), utf8(htlc));
    change(htlc, req);
}

/// `void hx_news15_mkdir(struct htlc_conn *htlc, char *path, const char *name)`
/// — MAKENEWSDIR. `path` is the *parent* folder, `name` the new folder.
///
/// # Safety
/// `htlc` is NULL or valid; `path` / `name` are NUL-terminated C strings or
/// NULL.
#[no_mangle]
pub unsafe extern "C" fn hx_news15_mkdir(
    htlc: *mut c_void,
    path: *const c_char,
    name: *const c_char,
) {
    if htlc.is_null() || path.is_null() {
        return;
    }
    let req = news::create_bundle(cstr_bytes(path), cstr_bytes(name), utf8(htlc));
    change(htlc, req);
}

// ---- flat 1.0/1.2 news ------------------------------------------------

/// `void hx_get_news(struct htlc_conn *htlc)` — NEWS_GETFILE: fetch the flat
/// 1.0/1.2 news file; its reply is the `news-file` signal.
///
/// # Safety
/// `htlc` is NULL or valid; main thread only.
#[no_mangle]
pub unsafe extern "C" fn hx_get_news(htlc: *mut c_void) {
    if htlc.is_null() {
        return;
    }
    send(htlc, &news::file(), Expect::NewsFile);
}

/// `void hx_post_news(struct htlc_conn *htlc, const char *news, guint16 len)` —
/// NEWS_POST: append `len` bytes to the flat news file; `len` is the explicit
/// byte count (the news body isn't necessarily NUL-terminated at `len`).
///
/// # Safety
/// `htlc` is NULL or valid; `text` is valid for `len` bytes or NULL.
#[no_mangle]
pub unsafe extern "C" fn hx_post_news(htlc: *mut c_void, text: *const c_char, len: u16) {
    if htlc.is_null() {
        return;
    }
    let bytes = if text.is_null() {
        &[][..]
    } else {
        std::slice::from_raw_parts(text.cast::<u8>(), len as usize)
    };
    change(htlc, news::post(bytes, utf8(htlc)));
}

#[cfg(test)]
mod tests;
