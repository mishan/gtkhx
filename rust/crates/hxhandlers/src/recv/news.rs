//! `hxnews-recv` — the 1.5 threaded-news receive handlers.
//!
//! These are the `rcv_task_*` reply callbacks the `hxnews-send` senders already
//! register via `task_new` (they were externed out of `rcv.c` and passed as
//! function pointers; only their bodies were still C). Moving the bodies here
//! leaves **no news code in `rcv.c`** — the generic trans-ID dispatcher
//! (`hx_rcv_task`, shared by every reply type) just calls these Rust callbacks.
//!
//! Each handler composes pieces that are already Rust: the `hotline-proto`
//! parser (owned handle) fed the received frame slice, the carrier stash
//! (`news_recv_bridge.c`), and the `gtkhx-session` signal emit. The main-thread
//! `gnews_browser_handle_*` view handler then feeds the handle to the
//! `hxnews-model` builder and frees it. See `docs/rust/news-receive-plan.md`.

use std::ffi::c_void;

pub mod carrier;
use carrier::{
    gnews_catalog_set_parsed, gnews_folder_set_parsed, news_post_fetch_failed, news_post_new,
};

use gtkhx_core::session::{gtkhx_session_emit_news_catalog, gtkhx_session_emit_news_file, gtkhx_session_emit_news_folder, gtkhx_session_emit_news_post, gtkhx_session_emit_news_thread, gtkhx_session_get_default};

/// `void hx_news_post_recv (htlc, bytes, len)` — emit the flat-news `news-post`
/// signal for one appended NEWSDATA chunk. [`hx_rcv_news_post`] calls this once
/// per sanitised NEWS chunk.
///
/// # Safety
/// `bytes` valid for `len` bytes (the sanitised chunk body); `htlc` is only
/// forwarded to the signal.
#[no_mangle]
pub unsafe extern "C" fn hx_news_post_recv(htlc: *mut c_void, bytes: *const u8, len: usize) {
    gtkhx_session_emit_news_post(gtkhx_session_get_default(), htlc, bytes.cast(), len as u16);
}

/// `void hx_rcv_news_post (htlc, frame, frame_len)` — the HTLS_HDR_NEWS_POST
/// primary handler (the flat 1.0/1.2 news push; was `rcv.c`).
///
/// Walks the message's `HTLS_DATA_NEWS` chunks natively
/// (`hotline_proto::parse::news_post_chunks`, the same per-chunk CR2LF +
/// strip_ansi contract as the old C `hx_news_post_walk`) and emits one
/// `news-post` line per chunk via [`hx_news_post_recv`]. Non-NEWS chunks are
/// skipped; an empty / chunk-less frame emits nothing.
///
/// # Safety
/// C-ABI primary handler invoked from the receive dispatch on the main thread.
/// `frame` is valid for `frame_len` bytes; `htlc` is only forwarded to the emit.
#[no_mangle]
pub unsafe extern "C" fn hx_rcv_news_post(htlc: *mut c_void, frame: *const u8, frame_len: usize) {
    if frame.is_null() {
        return;
    }
    let buf = std::slice::from_raw_parts(frame, frame_len);
    for body in hotline_proto::parse::news_post_chunks(buf, frame_len, u16::MAX as usize) {
        hx_news_post_recv(htlc, body.as_ptr(), body.len());
    }
}

/// `void hx_news_file_recv (htlc, bytes, len)` — emit the `news-file` signal
/// carrying the whole flat-news document (a `NEWS_FILE` task reply).
/// [`rcv_task_news_file`] calls this with the parsed document (or empty).
///
/// # Safety
/// `bytes` valid for `len` bytes (the sanitised document); `htlc` is only
/// forwarded to the signal.
#[no_mangle]
pub unsafe extern "C" fn hx_news_file_recv(htlc: *mut c_void, bytes: *const u8, len: usize) {
    gtkhx_session_emit_news_file(gtkhx_session_get_default(), htlc, bytes.cast(), len as u16);
}

/// `void rcv_task_news_file (htlc, frame, frame_len, ptr, data)` — the flat
/// `NEWS_FILE` task reply (the whole 1.0/1.2 news document; was `rcv.c`).
///
/// Parses the first `HTLS_DATA_NEWS` chunk natively
/// (`hotline_proto::parse::parse_news_file`, CR2LF + strip_ansi, capped at the
/// old 64 KiB scratch size less the NUL) and publishes it via
/// [`hx_news_file_recv`]. A chunk-less / short reply publishes an empty document,
/// exactly as the old C path did after its extractor returned FALSE. The `rcv.c`
/// `news_buf` / `news_len` scratch globals are gone with it.
///
/// # Safety
/// C-ABI reply callback invoked by `hx_rcv_task` on the main thread. `frame` is
/// valid for `frame_len` bytes; `htlc` is only forwarded to the emit.
#[no_mangle]
pub unsafe extern "C" fn rcv_task_news_file(
    htlc: *mut c_void,
    frame: *const c_void,
    frame_len: usize,
    _ptr: *mut c_void,
    _data: *mut c_void,
) {
    let buf = frame as *const u8;
    let body = if buf.is_null() {
        None
    } else {
        let s = std::slice::from_raw_parts(buf, frame_len);
        // 65535 = the old 64 KiB C scratch buffer minus the NUL the extractor
        // reserved (gtkhx_proto_parse_news_file used cap - 1).
        hotline_proto::parse::parse_news_file(s, frame_len, 65535)
    };
    match body {
        Some(b) => hx_news_file_recv(htlc, b.as_ptr(), b.len()),
        None => hx_news_file_recv(htlc, b"".as_ptr(), 0),
    }
}

/// `void rcv_task_newscat_list(struct htlc_conn *htlc, void *gcnews, void *data)`
/// — the HTLC_HDR_NEWSCATLIST reply handler (was `rcv.c`).
///
/// Parses the CATLIST chunk out of the received `frame` to an owned `CatList`
/// handle, stashes it on the `gnews_catalog` carrier, and emits `news-catalog`.
/// A NULL handle (absent / malformed chunk) is stashed as-is and treated as an
/// empty listing downstream — matching the old C, which emitted an empty
/// `news_group`.
///
/// # Safety
/// C-ABI reply callback invoked by `hx_rcv_task` on the main thread. `htlc` is a
/// valid `struct htlc_conn *` (unused here); `frame` is valid for `frame_len`
/// bytes; `gcnews` is the `struct gnews_catalog *` task pointer.
#[no_mangle]
pub unsafe extern "C" fn rcv_task_newscat_list(
    _htlc: *mut c_void,
    frame: *const c_void,
    frame_len: usize,
    gcnews: *mut c_void,
    _data: *mut c_void,
) {
    // Parse natively to an owned handle the C view side consumes + frees
    // (gtkhx_proto_catlist_free reclaims the same Box<CatList>). NULL when the
    // catalog chunk is absent / malformed, matching the old FFI.
    let parsed = if frame.is_null() {
        std::ptr::null_mut()
    } else {
        let s = std::slice::from_raw_parts(frame as *const u8, frame_len);
        match hotline_proto::parse::parse_catlist(s, frame_len) {
            Some(cl) => Box::into_raw(Box::new(cl)) as *mut c_void,
            None => std::ptr::null_mut(),
        }
    };
    gnews_catalog_set_parsed(gcnews, parsed);
    gtkhx_session_emit_news_catalog(gtkhx_session_get_default(), gcnews);
}

/// `void rcv_task_newsfolder_list(struct htlc_conn *htlc, void *gfnews, void *data)`
/// — the HTLC_HDR_NEWSDIRLIST reply handler (was `rcv.c`).
///
/// Parses every NEWSFOLDERITEM / CATEGORYITEM chunk out of the received `frame`
/// into an owned `DirList` handle, stashes it on the `gnews_folder` carrier, and
/// emits `news-folder`. The C `dh_start` chunk-walk + `folder_item[]`
/// accumulation are gone — native `hotline_proto::parse::parse_dirlist` does the
/// walk and always returns a (possibly empty) list.
///
/// # Safety
/// C-ABI reply callback invoked by `hx_rcv_task` on the main thread. `htlc` is a
/// valid `struct htlc_conn *` (unused here); `frame` is valid for `frame_len`
/// bytes; `gfnews` is the `struct gnews_folder *` task pointer.
#[no_mangle]
pub unsafe extern "C" fn rcv_task_newsfolder_list(
    _htlc: *mut c_void,
    frame: *const c_void,
    frame_len: usize,
    gfnews: *mut c_void,
    _data: *mut c_void,
) {
    // Parse natively to an owned handle the C view side consumes + frees
    // (gtkhx_proto_dirlist_free reclaims the same Box<DirList>). parse_dirlist
    // always yields a (possibly empty) list; NULL only on a NULL frame.
    let parsed = if frame.is_null() {
        std::ptr::null_mut()
    } else {
        let s = std::slice::from_raw_parts(frame as *const u8, frame_len);
        Box::into_raw(Box::new(hotline_proto::parse::parse_dirlist(s, frame_len))) as *mut c_void
    };
    gnews_folder_set_parsed(gfnews, parsed);
    gtkhx_session_emit_news_folder(gtkhx_session_get_default(), gfnews);
}

/// `void rcv_task_news_post(struct htlc_conn *htlc, void *target, void *data)` —
/// the HTLC_HDR_GETTHREAD reply handler (a post's body; was `rcv.c`).
///
/// `target` is the `HxNewsNode *` being fetched, carrying a transfer-full ref
/// (set up by `hx_news15_get_post` → `fetch_thread`). Parses the NEWSDATA body
/// out of the received `frame`; on a TASK_ERROR / body-less reply it releases
/// the ref via `news_post_fetch_failed` (no signal, so nothing else would).
/// Otherwise it hands the body + `target` to `news_post_new` and emits
/// `news-thread`, and the ref rides on to `gnews_browser_handle_thread`, which
/// unrefs it.
///
/// Unlike the catalog / folder carriers this one is created per-reply rather
/// than pre-allocated, so the body rides as a plain `g_strndup`'d string on
/// `news_post` (the existing shape) rather than an owned parse handle.
///
/// # Safety
/// C-ABI reply callback invoked by `hx_rcv_task` on the main thread. `htlc` is a
/// valid `struct htlc_conn *` (unused here); `frame` is valid for `frame_len`
/// bytes; `target` is the `HxNewsNode *` task pointer.
#[no_mangle]
pub unsafe extern "C" fn rcv_task_news_post(
    _htlc: *mut c_void,
    frame: *const c_void,
    frame_len: usize,
    target: *mut c_void,
    _data: *mut c_void,
) {
    if frame.is_null() {
        news_post_fetch_failed(target);
        return;
    }
    let s = std::slice::from_raw_parts(frame as *const u8, frame_len);
    // 65535 = the wire ceiling (chunk lens are u16; the old FFI capped at
    // text_cap-1). A TASK_ERROR or body-less reply releases the fetch ref.
    let reply = hotline_proto::parse::parse_news_thread_reply(s, frame_len, 65535);
    let body = match reply.text {
        Some(ref b) if !reply.has_task_error => b,
        _ => {
            news_post_fetch_failed(target);
            return;
        }
    };
    let post = news_post_new(target, body.as_ptr(), body.len());
    gtkhx_session_emit_news_thread(gtkhx_session_get_default(), post);
}
