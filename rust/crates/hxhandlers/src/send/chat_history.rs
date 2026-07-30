//! `hx_get_chat_history` — the chat-history extension request sender (the Rust
//! port of `src/chat_history.c`'s send path).
//!
//! Cap-gates on `CAP_CHAT_HISTORY` (sending TRAN 700 to a server that didn't echo
//! the bit earns a task error every time), builds the request chunks with the
//! **native** `hotline_proto::build::build_get_chat_history_chunks`, and hands
//! them to `hlwrite_chunks`. The caller registers the `rcv_task_chat_history`
//! reply task *before* calling — the task is keyed on `htlc->trans`, which
//! `hlwrite_chunks` consumes — so this crate stays free of the task table, same
//! contract as the C original.
//!
//! The pure chunk-builder stays reachable to the integration harness via the
//! `hx_get_chat_history_build_chunks` C-ABI shim in hotline-proto; only the
//! cap-gate + write wrapper lives here. Exports the exact `hx_get_chat_history`
//! C ABI so its callers (chat.c's Load-older flow, `hx_post_login_fetches`) link
//! unchanged.

use std::os::raw::{c_int, c_void};

use glib::ffi::{gboolean, GFALSE, GTRUE};
use hotline_proto::build::{self, GetChatHistoryRequest, HxChunk};
use hotline_proto::messages::ClientHdr;

/// `HTLC_CAP_CHAT_HISTORY` (bit 4, hotline.h) — the negotiated-cap bit that must
/// be set before a TRAN 700 request is legal.
const HTLC_CAP_CHAT_HISTORY: u64 = 0x0010;
const HTLC_HDR_GET_CHAT_HISTORY: u32 = ClientHdr::GetChatHistory as u32;

#[cfg(not(test))]
use gtkhx_core::conn::hx_conn_has_cap;

#[cfg(not(test))]
extern "C" {
    /// network.c → hxtask: pack + queue a client transaction. Consumes
    /// `htlc->trans`.
    fn hlwrite_chunks(htlc: *mut c_void, ty: u32, flag: u32, chunks: *const HxChunk, hc: c_int);
}

// The write primitive + cap check are stubbed under cfg(test) (see tests.rs), so
// the cargo-test build resolves without linking network.c / gtkhx-core's real
// (htlc-dereferencing) accessor.
#[cfg(test)]
use tests::{hlwrite_chunks, hx_conn_has_cap};

/// `gboolean hx_get_chat_history (htlc, channel_id, before, after, limit)` — send
/// a GET_CHAT_HISTORY (700) request for `channel_id`. `before` / `after` / `limit`
/// are optional (0 omits the chunk): `before > 0` = older-than, `after > 0` =
/// newer-than (reconnect catch-up), `limit > 0` = max results. Returns FALSE
/// without sending when `htlc` is NULL or the session didn't negotiate
/// `CAP_CHAT_HISTORY`; TRUE once the request is on the wire.
///
/// # Safety
/// `htlc` is a valid `struct htlc_conn *` (NULL is tolerated). The caller must
/// have registered the reply task first (see the module note).
#[no_mangle]
pub unsafe extern "C" fn hx_get_chat_history(
    htlc: *mut c_void,
    channel_id: u32,
    before: u64,
    after: u64,
    limit: u16,
) -> gboolean {
    if htlc.is_null() {
        return GFALSE;
    }
    if hx_conn_has_cap(htlc.cast(), HTLC_CAP_CHAT_HISTORY) == 0 {
        return GFALSE;
    }
    let mut chunks = [HxChunk::EMPTY; 4];
    let mut scratch = [0u8; 22];
    let req = GetChatHistoryRequest {
        channel_id,
        before,
        after,
        limit,
    };
    let hc = build::build_get_chat_history_chunks(&req, &mut chunks, &mut scratch);
    hlwrite_chunks(
        htlc,
        HTLC_HDR_GET_CHAT_HISTORY,
        0,
        chunks.as_ptr(),
        hc as c_int,
    );
    GTRUE
}

#[cfg(test)]
mod tests;
