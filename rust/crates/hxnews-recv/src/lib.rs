//! `hxnews-recv` — the 1.5 threaded-news receive handlers.
//!
//! These are the `rcv_task_*` reply callbacks the `hxnews-send` senders already
//! register via `task_new` (they were externed out of `rcv.c` and passed as
//! function pointers; only their bodies were still C). Moving the bodies here
//! leaves **no news code in `rcv.c`** — the generic trans-ID dispatcher
//! (`hx_rcv_task`, shared by every reply type) just calls these Rust callbacks.
//!
//! Each handler composes pieces that are already Rust: the `hotline-proto`
//! parser (owned handle), the carrier stash + `htlc->in` accessor
//! (`news_recv_bridge.c`), and the `gtkhx-session` signal emit. The main-thread
//! `gnews_browser_handle_*` view handler then feeds the handle to the
//! `hxnews-model` builder and frees it. See `docs/rust/news-receive-plan.md`.

use std::ffi::c_void;

mod carrier;
use carrier::{
    gnews_catalog_set_parsed, gnews_folder_set_parsed, news_post_fetch_failed, news_post_new,
};

extern "C" {
    // news_recv_bridge.c — the received frame body + its length (htlc->in).
    fn hx_htlc_in_buf(htlc: *mut c_void) -> *const u8;
    fn hx_htlc_in_pos(htlc: *mut c_void) -> usize;
    // hotline-proto — parse a reply to an owned handle. catlist: NULL when the
    // chunk is absent / malformed. dirlist: always a (possibly empty) handle.
    // The view handler frees them (gtkhx_proto_catlist_free / _dirlist_free).
    fn gtkhx_proto_parse_catlist(msg: *const u8, msglen: usize) -> *mut c_void;
    fn gtkhx_proto_parse_dirlist(msg: *const u8, msglen: usize) -> *mut c_void;
    // hotline-proto — parse the post-GETTHREAD TASK reply: writes the CR2LF +
    // strip_ansi'd NEWSDATA body into `text_buf` (NUL-terminated) and fills the
    // flags. Body-less / TASK_ERROR replies are filtered via the flags.
    fn gtkhx_proto_parse_news_thread_reply(
        msg: *const u8,
        msglen: usize,
        text_buf: *mut u8,
        text_cap: usize,
        out: *mut NewsThreadReply,
    ) -> bool;
    // gtkhx-session — the singleton + the news signal emits.
    fn gtkhx_session_get_default() -> *mut c_void;
    fn gtkhx_session_emit_news_catalog(self_: *mut c_void, gcnews: *mut c_void);
    fn gtkhx_session_emit_news_folder(self_: *mut c_void, gfnews: *mut c_void);
    fn gtkhx_session_emit_news_thread(self_: *mut c_void, post: *mut c_void);
    fn gtkhx_session_emit_news_post(self_: *mut c_void, htlc: *mut c_void, news: *const u8, len: u16);
    fn gtkhx_session_emit_news_file(self_: *mut c_void, htlc: *mut c_void, news: *const u8, len: u16);
}

/// `void hx_news_post_recv (htlc, bytes, len)` — emit the flat-news `news-post`
/// signal for one appended NEWSDATA chunk. The C handler (`hx_rcv_news_post`)
/// keeps the `gtkhx_proto_walk_news_post` chunk walk and calls this once per
/// sanitised chunk; the signal emit is the only thing that moved here, so
/// `rcv.c` no longer touches `GtkhxSession` for flat news.
///
/// # Safety
/// `bytes` valid for `len` bytes (the sanitised chunk body); `htlc` is only
/// forwarded to the signal.
#[no_mangle]
pub unsafe extern "C" fn hx_news_post_recv(htlc: *mut c_void, bytes: *const u8, len: usize) {
    gtkhx_session_emit_news_post(gtkhx_session_get_default(), htlc, bytes, len as u16);
}

/// `void hx_news_file_recv (htlc, bytes, len)` — emit the `news-file` signal
/// carrying the whole flat-news document (a `NEWS_FILE` task reply). The C
/// handler (`rcv_task_news_file`) keeps the `hx_news_file_extract` parse into
/// its scratch buffer and calls this to publish it.
///
/// # Safety
/// `bytes` valid for `len` bytes (the sanitised document); `htlc` is only
/// forwarded to the signal.
#[no_mangle]
pub unsafe extern "C" fn hx_news_file_recv(htlc: *mut c_void, bytes: *const u8, len: usize) {
    gtkhx_session_emit_news_file(gtkhx_session_get_default(), htlc, bytes, len as u16);
}

/// `#[repr(C)]` mirror of C's `struct gtkhx_proto_news_thread_reply`
/// (`hotline_proto.h`): the flags the thread-reply parser fills.
#[repr(C)]
#[derive(Default)]
struct NewsThreadReply {
    thread_id: u32,
    /// Body bytes written to the buffer, excluding the trailing NUL.
    text_len: u16,
    /// 1 iff a NEWSDATA chunk was present and no TASK_ERROR short-circuited.
    has_text: u8,
    /// 1 iff a TASK_ERROR chunk was seen mid-walk.
    has_task_error: u8,
}

/// `void rcv_task_newscat_list(struct htlc_conn *htlc, void *gcnews, void *data)`
/// — the HTLC_HDR_NEWSCATLIST reply handler (was `rcv.c`).
///
/// Parses the CATLIST chunk out of `htlc->in` to an owned `CatList` handle,
/// stashes it on the `gnews_catalog` carrier, and emits `news-catalog`. A NULL
/// handle (absent / malformed chunk) is stashed as-is and treated as an empty
/// listing downstream — matching the old C, which emitted an empty `news_group`.
///
/// # Safety
/// C-ABI reply callback invoked by `hx_rcv_task` on the main thread. `htlc` is a
/// valid `struct htlc_conn *`; `gcnews` is the `struct gnews_catalog *` task
/// pointer.
#[no_mangle]
pub unsafe extern "C" fn rcv_task_newscat_list(
    htlc: *mut c_void,
    _frame: *const c_void,
    _frame_len: usize,
    gcnews: *mut c_void,
    _data: *mut c_void,
) {
    let buf = hx_htlc_in_buf(htlc);
    let len = hx_htlc_in_pos(htlc);
    let parsed = if buf.is_null() {
        std::ptr::null_mut()
    } else {
        gtkhx_proto_parse_catlist(buf, len)
    };
    gnews_catalog_set_parsed(gcnews, parsed);
    gtkhx_session_emit_news_catalog(gtkhx_session_get_default(), gcnews);
}

/// `void rcv_task_newsfolder_list(struct htlc_conn *htlc, void *gfnews, void *data)`
/// — the HTLC_HDR_NEWSDIRLIST reply handler (was `rcv.c`).
///
/// Parses every NEWSFOLDERITEM / CATEGORYITEM chunk out of `htlc->in` into an
/// owned `DirList` handle, stashes it on the `gnews_folder` carrier, and emits
/// `news-folder`. The C `dh_start` chunk-walk + `folder_item[]` accumulation are
/// gone — `gtkhx_proto_parse_dirlist` does the walk and always returns a
/// (possibly empty) handle.
///
/// # Safety
/// C-ABI reply callback invoked by `hx_rcv_task` on the main thread. `htlc` is a
/// valid `struct htlc_conn *`; `gfnews` is the `struct gnews_folder *` task
/// pointer.
#[no_mangle]
pub unsafe extern "C" fn rcv_task_newsfolder_list(
    htlc: *mut c_void,
    _frame: *const c_void,
    _frame_len: usize,
    gfnews: *mut c_void,
    _data: *mut c_void,
) {
    let buf = hx_htlc_in_buf(htlc);
    let len = hx_htlc_in_pos(htlc);
    let parsed = if buf.is_null() {
        std::ptr::null_mut()
    } else {
        gtkhx_proto_parse_dirlist(buf, len)
    };
    gnews_folder_set_parsed(gfnews, parsed);
    gtkhx_session_emit_news_folder(gtkhx_session_get_default(), gfnews);
}

/// `void rcv_task_news_post(struct htlc_conn *htlc, void *target, void *data)` —
/// the HTLC_HDR_GETTHREAD reply handler (a post's body; was `rcv.c`).
///
/// `target` is the `HxNewsNode *` being fetched, carrying a transfer-full ref
/// (set up by `hx_news15_get_post` → `fetch_thread`). Parses the NEWSDATA body
/// out of `htlc->in`; on a TASK_ERROR / body-less reply it releases the ref via
/// `news_post_fetch_failed` (no signal, so nothing else would). Otherwise it
/// hands the body + `target` to `news_post_new` and emits `news-thread`, and the
/// ref rides on to `gnews_browser_handle_thread`, which unrefs it.
///
/// Unlike the catalog / folder carriers this one is created per-reply rather
/// than pre-allocated, so the body rides as a plain `g_strndup`'d string on
/// `news_post` (the existing shape) rather than an owned parse handle.
///
/// # Safety
/// C-ABI reply callback invoked by `hx_rcv_task` on the main thread. `htlc` is a
/// valid `struct htlc_conn *`; `target` is the `HxNewsNode *` task pointer.
#[no_mangle]
pub unsafe extern "C" fn rcv_task_news_post(
    htlc: *mut c_void,
    _frame: *const c_void,
    _frame_len: usize,
    target: *mut c_void,
    _data: *mut c_void,
) {
    let buf = hx_htlc_in_buf(htlc);
    if buf.is_null() {
        news_post_fetch_failed(target);
        return;
    }
    let len = hx_htlc_in_pos(htlc);
    // 65536: chunk lens are u16, so this comfortably holds any NEWSDATA body
    // (the parser caps at text_cap-1 = 65535, the wire ceiling).
    let mut text = vec![0u8; 65536];
    let mut reply = NewsThreadReply::default();
    gtkhx_proto_parse_news_thread_reply(buf, len, text.as_mut_ptr(), text.len(), &mut reply);
    if reply.has_task_error != 0 || reply.has_text == 0 {
        news_post_fetch_failed(target);
        return;
    }
    let post = news_post_new(target, text.as_ptr(), reply.text_len as usize);
    gtkhx_session_emit_news_thread(gtkhx_session_get_default(), post);
}
