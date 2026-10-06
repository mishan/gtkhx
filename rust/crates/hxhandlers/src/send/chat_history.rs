//! The chat-history extension's request (GET_CHAT_HISTORY, 700): the fetch
//! that follows the login, and a page older than what a chat shows. Sent
//! only where the server agreed to chat history — anywhere else it earns a
//! refusal every time — with its reply expected by the session, which hands
//! it on as an event (`recv::chat::history`).

use std::os::raw::{c_int, c_void};

use glib::ffi::{gboolean, GFALSE, GTRUE};
use hxproto::build::HxChunk;

use crate::recv::chat::{history_forget, history_requested};

/// `HTLC_CAP_CHAT_HISTORY` (bit 4, hotline.h).
const HTLC_CAP_CHAT_HISTORY: u64 = 0x0010;
/// A page of history for someone who asked for one, when the setting for
/// the fetch at login says none.
const ASKED_PAGE: u16 = 50;

#[cfg(not(test))]
use gtkhx_core::conn::{hx_conn_chat_history_last_msgid, hx_conn_has_cap};

#[cfg(not(test))]
extern "C" {
    /// hxtask: pack + queue a client transaction, on the trans reserved for it.
    fn hlwrite_chunks(htlc: *mut c_void, ty: u32, flag: u32, chunks: *const HxChunk, hc: c_int);
    /// debug.c — a pre-formatted line under a debug category.
    fn debug_log_str(cat: *const std::os::raw::c_char, msg: *const std::os::raw::c_char);
}

#[cfg(test)]
use tests::{debug_log_str, hlwrite_chunks, hx_conn_chat_history_last_msgid, hx_conn_has_cap};

/// The lines the fetch at login asks for (`chat.history_initial`).
fn initial_lines() -> u16 {
    let n = hxconfig::ffi::with_settings(|s| s.chat.history_initial).unwrap_or(50);
    u16::try_from(n).unwrap_or(u16::MAX)
}

/// `gboolean hx_chat_history_fetch_initial (htlc)` — the public chat's
/// history, once the login has settled. After a reconnect to the same
/// server, everything since the newest line this connection saw, as many
/// as the server keeps; otherwise the last `chat.history_initial` lines,
/// and nothing when that is 0. Whether it was sent.
///
/// # Safety
/// `htlc` is NULL or a live connection; main thread.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_history_fetch_initial(htlc: *mut c_void) -> gboolean {
    if htlc.is_null() {
        return GFALSE;
    }
    history_forget(htlc);
    match hx_conn_chat_history_last_msgid(htlc.cast()) {
        0 => match initial_lines() {
            0 => GFALSE,
            limit => fetch(htlc, 0, 0, 0, limit),
        },
        newest => fetch(htlc, 0, 0, newest, 0),
    }
}

/// `gboolean hx_chat_history_fetch_older (htlc, cid, before)` — the page of
/// chat `cid` before line `before`, of `chat.history_initial` lines, or
/// [`ASKED_PAGE`] when that is 0: the user asked for it. Whether it was
/// sent.
///
/// # Safety
/// `htlc` is NULL or a live connection; main thread.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_history_fetch_older(
    htlc: *mut c_void,
    cid: u32,
    before: u64,
) -> gboolean {
    if htlc.is_null() {
        return GFALSE;
    }
    let limit = match initial_lines() {
        0 => ASKED_PAGE,
        n => n,
    };
    fetch(htlc, cid, before, 0, limit)
}

unsafe fn fetch(htlc: *mut c_void, cid: u32, before: u64, after: u64, limit: u16) -> gboolean {
    if hx_conn_has_cap(htlc.cast(), HTLC_CAP_CHAT_HISTORY) == 0 {
        return GFALSE;
    }
    let req = hxrequest::chat::history(cid, before, after, limit);
    let line = format!("request: cid={cid} before={before} after={after} limit={limit}");
    if let Ok(line) = std::ffi::CString::new(line) {
        debug_log_str(c"chat-history".as_ptr(), line.as_ptr());
    }
    let trans = super::expect_next(htlc, hxsession::Expect::ChatHistory { cid });
    history_requested(htlc, trans, cid, before != 0);
    req.with_hx_chunks(|chunks| {
        hlwrite_chunks(htlc, req.opcode, 0, chunks.as_ptr(), chunks.len() as c_int)
    });
    GTRUE
}

#[cfg(test)]
mod tests;
