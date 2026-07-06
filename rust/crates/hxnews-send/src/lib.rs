//! `hxnews-send` — the news RPC senders (the Rust port of `src/news15.c`'s 1.5
//! threaded-news senders + `src/news.c`'s flat 1.0/1.2 `hx_get_news` /
//! `hx_post_news`, Phase R5 N2c).
//!
//! Thin wrappers that emit the client-initiated news transactions — flat
//! NEWS_GETFILE / NEWS_POST, and the 1.5 DIRLIST / CATLIST folder+category
//! enumeration, GETTHREAD post-body fetch, POSTTHREAD, DELETETHREAD,
//! DELNEWSDIRCAT, MAKECATEGORY, MAKENEWSDIR. Each
//! one: encodes the request's path to the wire `NEWSPATH` bytes (via C's
//! `path_to_hldir`), encodes any text for the wire (`gtkhx_text_for_wire`,
//! hxtext), builds the chunks with the **native** `hotline_proto::build`
//! builders (the same ones the R2 `gtkhx_proto_build_news_*` C-ABI shims wrap),
//! registers a reply task where the C original did, and hands the chunks to
//! `hlwrite_chunks`. Exports the exact `hx_news15_*` C ABI so `news_browser.c`
//! links unchanged.
//!
//! Three senders (`get_post` / `cat_list` / `fldr_list`) take C structs
//! (`news_item` / `gnews_catalog` / `gnews_folder`); their fields are read (and
//! the `listing` flag set) through `news_send_bridge.c` rather than mirroring
//! the GTK-laden structs here.
//!
//! A lean crate (`glib` + the pure `hotline-proto`, no GTK) so it's
//! `cargo test`-able: the builders run natively and the C send-path primitives
//! are stubbed in the test module. Mirrors `hxchat-send`.

use std::ffi::{c_char, c_void, CStr};
use std::os::raw::c_int;

use hotline_proto::build::{
    self, HxChunk, NewsDeleteThreadRequest, NewsGetThreadRequest, NewsMakeCategoryRequest,
    NewsPostThreadRequest,
};

// Wire opcodes (hotline.h). Only GETTHREAD / POSTTHREAD have ClientHdr enum
// entries; the 0x65/0x67 flat-news + 0x17x threaded blocks are spelled here
// against the hotline.h reference.
const HTLC_HDR_NEWS_GETFILE: u32 = 0x0000_0065;
const HTLC_HDR_NEWS_POST: u32 = 0x0000_0067;
const HTLC_HDR_NEWSDIRLIST: u32 = 0x0000_0172;
const HTLC_HDR_NEWSCATLIST: u32 = 0x0000_0173;
const HTLC_HDR_DELNEWSDIRCAT: u32 = 0x0000_017c;
const HTLC_HDR_MAKENEWSDIR: u32 = 0x0000_017d;
const HTLC_HDR_MAKECATEGORY: u32 = 0x0000_017e;
const HTLC_HDR_GETTHREAD: u32 = 0x0000_0190;
const HTLC_HDR_POSTTHREAD: u32 = 0x0000_019a;
const HTLC_HDR_DELETETHREAD: u32 = 0x0000_019b;

/// `rcv_task_fn` (protocol.h): the reply-handler shape `task_new` stores. The
/// real `rcv_task_news_post` (2-arg) / `rcv_task_newscat_list` /
/// `rcv_task_newsfolder_list` symbols are invoked through this 3-arg type —
/// exactly what the C `RCV_TASK_FN` cast does.
type RcvTaskFn = unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void);

// Real build: these resolve at the final C link. Test build: `use tests::{…}`
// below shadows them with recording stubs, so the extern block is gated off.
#[cfg(not(test))]
extern "C" {
    // path_hldir.c — encode a "/a/b" path to the wire NEWSPATH bytes. Returns a
    // g_malloc'd buffer + out length; caller g_free's. is_file = 0 for news.
    fn path_to_hldir(path: *const c_char, hldirlen: *mut u16, is_file: c_int) -> *mut u8;

    // hxtext — encode UTF-8 → wire (verbatim or Mac Roman + LF→CR).
    fn gtkhx_text_for_wire(
        utf8: *const c_char,
        utf8_len: usize,
        utf8_mode: glib::ffi::gboolean,
        is_body: glib::ffi::gboolean,
        out_len: *mut usize,
    ) -> *mut c_char;

    // chat_send_bridge.c — per-htlc CAP_TEXT_ENCODING probe (shared with the
    // chat senders).
    fn hx_htlc_text_encoding_cap(htlc: *mut c_void) -> glib::ffi::gboolean;

    // tasks.c / network.c — the send-path primitives.
    fn task_new(
        htlc: *mut c_void,
        rcv: Option<RcvTaskFn>,
        ptr: *mut c_void,
        data: *mut c_void,
        str_: *const c_char,
    ) -> *mut c_void;
    fn hlwrite_chunks(htlc: *mut c_void, ty: u32, flag: u32, chunks: *const HxChunk, hc: c_int);

    // rcv.c — reply-task handlers (declared 3-arg per the RcvTaskFn note).
    fn rcv_task_news_file(htlc: *mut c_void, ptr: *mut c_void, data: *mut c_void);
    fn rcv_task_news_post(htlc: *mut c_void, ptr: *mut c_void, data: *mut c_void);
    fn rcv_task_newscat_list(htlc: *mut c_void, ptr: *mut c_void, data: *mut c_void);
    fn rcv_task_newsfolder_list(htlc: *mut c_void, ptr: *mut c_void, data: *mut c_void);

    // news_send_bridge.c — struct field accessors.
    fn news_item_group_path(item: *mut c_void) -> *const c_char;
    fn news_item_postid(item: *mut c_void) -> u32;
    fn news_item_mime0(item: *mut c_void) -> *const c_char;
    fn gnews_catalog_path(g: *mut c_void) -> *const c_char;
    fn gnews_catalog_mark_listing(g: *mut c_void);
    fn gnews_folder_path(g: *mut c_void) -> *const c_char;
    fn gnews_folder_mark_listing(g: *mut c_void);
}

#[cfg(test)]
use tests::{
    gnews_catalog_mark_listing, gnews_catalog_path, gnews_folder_mark_listing, gnews_folder_path,
    gtkhx_text_for_wire, hlwrite_chunks, hx_htlc_text_encoding_cap, news_item_group_path,
    news_item_mime0, news_item_postid, path_to_hldir, rcv_task_news_file, rcv_task_news_post,
    rcv_task_newscat_list, rcv_task_newsfolder_list, task_new,
};

/// A NUL-terminated C string's bytes (without the NUL), or empty for NULL.
unsafe fn cstr_bytes<'a>(s: *const c_char) -> &'a [u8] {
    if s.is_null() {
        &[]
    } else {
        CStr::from_ptr(s).to_bytes()
    }
}

/// View a `path_to_hldir` result (`*mut u8` + `u16` length) as a byte slice.
unsafe fn hldir_slice<'a>(ptr: *mut u8, len: u16) -> &'a [u8] {
    if ptr.is_null() || len == 0 {
        &[]
    } else {
        std::slice::from_raw_parts(ptr, len as usize)
    }
}

/// Encode `text` for the wire on this connection (`utf8_mode` from the htlc's
/// CAP_TEXT_ENCODING; `is_body` toggles LF→CR), run the bytes through `f`, then
/// g_free the buffer. `f` must not retain the slice past its own return.
unsafe fn with_wire<R>(
    text: *const c_char,
    utf8_mode: glib::ffi::gboolean,
    is_body: glib::ffi::gboolean,
    f: impl FnOnce(&[u8]) -> R,
) -> R {
    let bytes = cstr_bytes(text);
    let mut wire_len: usize = 0;
    let wire = gtkhx_text_for_wire(text, bytes.len(), utf8_mode, is_body, &mut wire_len);
    let slice: &[u8] = if wire.is_null() || wire_len == 0 || wire_len > isize::MAX as usize {
        &[]
    } else {
        std::slice::from_raw_parts(wire as *const u8, wire_len)
    };
    let r = f(slice);
    if !wire.is_null() {
        glib::ffi::g_free(wire as *mut c_void);
    }
    r
}

/// `void hx_news15_get_post(struct htlc_conn *htlc, struct news_item *item)` —
/// GETTHREAD: fetch a post's body. Reply drives `rcv_task_news_post` with the
/// `item` pointer.
///
/// # Safety
/// `htlc` / `item` are NULL or valid; main thread only.
#[no_mangle]
pub unsafe extern "C" fn hx_news15_get_post(htlc: *mut c_void, item: *mut c_void) {
    if htlc.is_null() || item.is_null() {
        return;
    }
    let mut hldirlen: u16 = 0;
    let hldir = path_to_hldir(news_item_group_path(item), &mut hldirlen, 0);

    let mut chunks = [HxChunk::EMPTY; 3];
    let mut scratch = [0u8; 4];
    let req = NewsGetThreadRequest {
        path: hldir_slice(hldir, hldirlen),
        threadid: news_item_postid(item),
        mime_type: cstr_bytes(news_item_mime0(item)),
    };
    let hc = build::build_news_getthread_chunks(&req, &mut chunks, &mut scratch);
    if hc > 0 {
        task_new(
            htlc,
            Some(rcv_task_news_post),
            item,
            std::ptr::null_mut(),
            c"news_post".as_ptr(),
        );
        hlwrite_chunks(htlc, HTLC_HDR_GETTHREAD, 0, chunks.as_ptr(), hc as c_int);
    }
    glib::ffi::g_free(hldir as *mut c_void);
}

/// `void hx_news15_cat_list(struct htlc_conn *htlc, struct gnews_catalog *g)` —
/// NEWSCATLIST: enumerate a category's posts. Reply drives
/// `rcv_task_newscat_list` with `g`.
///
/// # Safety
/// `htlc` / `g` are NULL or valid; main thread only.
#[no_mangle]
pub unsafe extern "C" fn hx_news15_cat_list(htlc: *mut c_void, g: *mut c_void) {
    if htlc.is_null() || g.is_null() {
        return;
    }
    gnews_catalog_mark_listing(g);
    let mut hldirlen: u16 = 0;
    let hldir = path_to_hldir(gnews_catalog_path(g), &mut hldirlen, 0);

    let mut chunks = [HxChunk::EMPTY; 1];
    let hc = build::build_news_catlist_chunks(hldir_slice(hldir, hldirlen), &mut chunks);
    if hc > 0 {
        task_new(
            htlc,
            Some(rcv_task_newscat_list),
            g,
            std::ptr::null_mut(),
            c"news_category".as_ptr(),
        );
        hlwrite_chunks(htlc, HTLC_HDR_NEWSCATLIST, 0, chunks.as_ptr(), hc as c_int);
    }
    glib::ffi::g_free(hldir as *mut c_void);
}

/// `void hx_news15_fldr_list(struct htlc_conn *htlc, struct gnews_folder *g)` —
/// NEWSDIRLIST: enumerate a folder's folders+categories. Reply drives
/// `rcv_task_newsfolder_list` with `g`.
///
/// # Safety
/// `htlc` / `g` are NULL or valid; main thread only.
#[no_mangle]
pub unsafe extern "C" fn hx_news15_fldr_list(htlc: *mut c_void, g: *mut c_void) {
    if htlc.is_null() || g.is_null() {
        return;
    }
    gnews_folder_mark_listing(g);
    let mut hldirlen: u16 = 0;
    let hldir = path_to_hldir(gnews_folder_path(g), &mut hldirlen, 0);

    let mut chunks = [HxChunk::EMPTY; 1];
    let hc = build::build_news_dirlist_chunks(hldir_slice(hldir, hldirlen), &mut chunks);
    if hc > 0 {
        task_new(
            htlc,
            Some(rcv_task_newsfolder_list),
            g,
            std::ptr::null_mut(),
            c"news_folder".as_ptr(),
        );
        hlwrite_chunks(htlc, HTLC_HDR_NEWSDIRLIST, 0, chunks.as_ptr(), hc as c_int);
    }
    glib::ffi::g_free(hldir as *mut c_void);
}

/// `void hx_news15_post_thread(struct htlc_conn *htlc, char *path,
/// const char *subject, guint32 threadid, char *text)` — POSTTHREAD. `threadid`
/// is the post being replied to (0 for a new top-level post). No reply handler.
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
    if htlc.is_null() {
        return;
    }
    let mut hldirlen: u16 = 0;
    let hldir = path_to_hldir(path, &mut hldirlen, 0);
    let utf8 = hx_htlc_text_encoding_cap(htlc);

    // Subject is a single-line field (is_body = FALSE); the body is a body
    // field (is_body = TRUE → LF→CR on legacy servers). NEWSTYPE is hard-coded
    // "text/plain" — gtkhx only sends plain-text articles.
    with_wire(subject, utf8, glib::ffi::GFALSE, |subj_wire| {
        with_wire(text, utf8, glib::ffi::GTRUE, |text_wire| {
            let mut chunks = [HxChunk::EMPTY; 6];
            let mut scratch = [0u8; 8];
            let req = NewsPostThreadRequest {
                path: hldir_slice(hldir, hldirlen),
                parent_thread: 0,
                mime_type: b"text/plain",
                subject: subj_wire,
                text: text_wire,
                thread_id: threadid,
            };
            let hc = build::build_news_post_thread_chunks(&req, &mut chunks, &mut scratch);
            if hc > 0 {
                task_new(
                    htlc,
                    None,
                    std::ptr::null_mut(),
                    std::ptr::null_mut(),
                    c"news15_post".as_ptr(),
                );
                hlwrite_chunks(htlc, HTLC_HDR_POSTTHREAD, 0, chunks.as_ptr(), hc as c_int);
            }
        });
    });
    glib::ffi::g_free(hldir as *mut c_void);
}

/// `void hx_news15_delete_thread(struct htlc_conn *htlc, char *path,
/// guint32 threadid)` — DELETETHREAD. No reply handler.
///
/// # Safety
/// `htlc` is NULL or valid; `path` is a NUL-terminated C string or NULL.
#[no_mangle]
pub unsafe extern "C" fn hx_news15_delete_thread(
    htlc: *mut c_void,
    path: *const c_char,
    threadid: u32,
) {
    if htlc.is_null() {
        return;
    }
    let mut hldirlen: u16 = 0;
    let hldir = path_to_hldir(path, &mut hldirlen, 0);

    let mut chunks = [HxChunk::EMPTY; 2];
    let mut scratch = [0u8; 4];
    let req = NewsDeleteThreadRequest {
        path: hldir_slice(hldir, hldirlen),
        threadid,
    };
    let hc = build::build_news_delete_thread_chunks(&req, &mut chunks, &mut scratch);
    if hc > 0 {
        task_new(
            htlc,
            None,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            c"news15_rm_thread".as_ptr(),
        );
        hlwrite_chunks(htlc, HTLC_HDR_DELETETHREAD, 0, chunks.as_ptr(), hc as c_int);
    }
    glib::ffi::g_free(hldir as *mut c_void);
}

/// `void hx_news15_delete(struct htlc_conn *htlc, char *path)` — DELNEWSDIRCAT
/// (deletes a folder or category; mhxd inspects the path). No reply handler.
///
/// # Safety
/// `htlc` is NULL or valid; `path` is a NUL-terminated C string or NULL.
#[no_mangle]
pub unsafe extern "C" fn hx_news15_delete(htlc: *mut c_void, path: *const c_char) {
    if htlc.is_null() {
        return;
    }
    let mut hldirlen: u16 = 0;
    let hldir = path_to_hldir(path, &mut hldirlen, 0);

    let mut chunks = [HxChunk::EMPTY; 1];
    let hc = build::build_news_delete_chunks(hldir_slice(hldir, hldirlen), &mut chunks);
    if hc > 0 {
        task_new(
            htlc,
            None,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            c"news15_rm".as_ptr(),
        );
        hlwrite_chunks(htlc, HTLC_HDR_DELNEWSDIRCAT, 0, chunks.as_ptr(), hc as c_int);
    }
    glib::ffi::g_free(hldir as *mut c_void);
}

/// `void hx_news15_mkcat(struct htlc_conn *htlc, char *path, const char *name)`
/// — MAKECATEGORY. No reply handler.
///
/// # Safety
/// `htlc` is NULL or valid; `path` / `name` are NUL-terminated C strings or NULL.
#[no_mangle]
pub unsafe extern "C" fn hx_news15_mkcat(
    htlc: *mut c_void,
    path: *const c_char,
    name: *const c_char,
) {
    if htlc.is_null() {
        return;
    }
    let mut hldirlen: u16 = 0;
    let hldir = path_to_hldir(path, &mut hldirlen, 0);
    let utf8 = hx_htlc_text_encoding_cap(htlc);

    with_wire(name, utf8, glib::ffi::GFALSE, |name_wire| {
        let mut chunks = [HxChunk::EMPTY; 2];
        let req = NewsMakeCategoryRequest {
            path: hldir_slice(hldir, hldirlen),
            name: name_wire,
        };
        let hc = build::build_news_mkcat_chunks(&req, &mut chunks);
        if hc > 0 {
            task_new(
                htlc,
                None,
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                c"news15_mkcat".as_ptr(),
            );
            hlwrite_chunks(htlc, HTLC_HDR_MAKECATEGORY, 0, chunks.as_ptr(), hc as c_int);
        }
    });
    glib::ffi::g_free(hldir as *mut c_void);
}

/// `void hx_news15_mkdir(struct htlc_conn *htlc, char *path)` — MAKENEWSDIR
/// (the path's last component is the new folder name). No reply handler.
///
/// # Safety
/// `htlc` is NULL or valid; `path` is a NUL-terminated C string or NULL.
#[no_mangle]
pub unsafe extern "C" fn hx_news15_mkdir(htlc: *mut c_void, path: *const c_char) {
    if htlc.is_null() {
        return;
    }
    let mut hldirlen: u16 = 0;
    let hldir = path_to_hldir(path, &mut hldirlen, 0);

    let mut chunks = [HxChunk::EMPTY; 1];
    let hc = build::build_news_mkdir_chunks(hldir_slice(hldir, hldirlen), &mut chunks);
    if hc > 0 {
        task_new(
            htlc,
            None,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            c"news15_mkdir".as_ptr(),
        );
        hlwrite_chunks(htlc, HTLC_HDR_MAKENEWSDIR, 0, chunks.as_ptr(), hc as c_int);
    }
    glib::ffi::g_free(hldir as *mut c_void);
}

// ---- flat 1.0/1.2 news (was news.c) ---------------------------------

/// `void hx_get_news(struct htlc_conn *htlc)` — NEWS_GETFILE: fetch the flat
/// 1.0/1.2 news file. Zero-chunk opcode; reply drives `rcv_task_news_file`.
///
/// # Safety
/// `htlc` is NULL or valid; main thread only.
#[no_mangle]
pub unsafe extern "C" fn hx_get_news(htlc: *mut c_void) {
    if htlc.is_null() {
        return;
    }
    task_new(
        htlc,
        Some(rcv_task_news_file),
        std::ptr::null_mut(),
        std::ptr::null_mut(),
        c"news".as_ptr(),
    );
    hlwrite_chunks(htlc, HTLC_HDR_NEWS_GETFILE, 0, std::ptr::null(), 0);
}

/// `void hx_post_news(struct htlc_conn *htlc, const char *news, guint16 len)` —
/// NEWS_POST: append `len` bytes to the flat news file. No reply handler. The
/// body is a body field (LF→CR on legacy servers); `len` is the explicit byte
/// count (the news body isn't necessarily NUL-terminated at `len`).
///
/// # Safety
/// `htlc` is NULL or valid; `news` is valid for `len` bytes or NULL.
#[no_mangle]
pub unsafe extern "C" fn hx_post_news(htlc: *mut c_void, news: *const c_char, len: u16) {
    if htlc.is_null() {
        return;
    }
    let utf8 = hx_htlc_text_encoding_cap(htlc);
    let mut wire_len: usize = 0;
    let wire = gtkhx_text_for_wire(news, len as usize, utf8, glib::ffi::GTRUE, &mut wire_len);
    let body: &[u8] = if wire.is_null() || wire_len == 0 || wire_len > isize::MAX as usize {
        &[]
    } else {
        std::slice::from_raw_parts(wire as *const u8, wire_len)
    };
    let mut chunks = [HxChunk::EMPTY; 1];
    let hc = build::build_news_post_chunks(body, &mut chunks);
    if hc > 0 {
        task_new(
            htlc,
            None,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            c"post".as_ptr(),
        );
        hlwrite_chunks(htlc, HTLC_HDR_NEWS_POST, 0, chunks.as_ptr(), hc as c_int);
    }
    if !wire.is_null() {
        glib::ffi::g_free(wire as *mut c_void);
    }
}

#[cfg(test)]
mod tests;
