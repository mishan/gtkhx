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

extern "C" {
    // news_recv_bridge.c — the received frame body + its length (htlc->in).
    fn hx_htlc_in_buf(htlc: *mut c_void) -> *const u8;
    fn hx_htlc_in_pos(htlc: *mut c_void) -> usize;
    // news_recv_bridge.c — stash the parse handle on the catalog carrier.
    fn gnews_catalog_set_parsed(g: *mut c_void, parsed: *mut c_void);
    // hotline-proto — parse the first CATLIST chunk to an owned handle, or NULL
    // when absent / malformed. The view handler frees it (gtkhx_proto_catlist_free).
    fn gtkhx_proto_parse_catlist(msg: *const u8, msglen: usize) -> *mut c_void;
    // gtkhx-session — the singleton + the news-catalog signal emit.
    fn gtkhx_session_get_default() -> *mut c_void;
    fn gtkhx_session_emit_news_catalog(self_: *mut c_void, gcnews: *mut c_void);
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
