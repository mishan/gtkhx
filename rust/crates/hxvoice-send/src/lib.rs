//! `hxvoice-send` — voice-chat wire-out senders (ported from `src/voice.c`,
//! Phase 8.A).
//!
//! Thin wrappers that emit the client-initiated voice transactions
//! (600 JOIN / 601 LEAVE / 603 SDP_ANSWER / 604 ICE / 606 MUTE): cap-gate,
//! build the wire chunks with the **native** `hotline_proto::voice` builders
//! (not the C-ABI `gtkhx_proto_build_*` shims — the whole build flow is Rust),
//! register the reply task (except 604, a bidirectional notification with no
//! reply), and hand the chunks to `hlwrite_chunks`. Exports the exact
//! `hx_send_voice_*` C ABI so `rcv.c` links unchanged, and the sibling Rust
//! voice modules (`voice_panel`, `voice_ptt` in gtkhx-ui) keep reaching it
//! through their existing externs.
//!
//! A lean dedicated crate (only `glib` + the pure `hotline-proto`, no GTK) so
//! it's `cargo test`-able — the builders run natively and the C send-path
//! primitives are stubbed in the test module — and so the C send-path unit
//! test can link just this staticlib. Built + linked only when voice is
//! enabled (excluded from the voice-off cargo build); every `hx_send_voice_*`
//! caller is `#ifdef HAVE_VOICE`.
//!
//! What stays C behind the FFI seam is the send-path *infrastructure*, not the
//! protocol: the htlc cap accessor (`hx_htlc_voice_cap`, voice_bridge.c), the
//! task table (`task_new` + the `rcv_task_voice_*` reply handlers, rcv.c), and
//! the write primitive (`hlwrite_chunks`, network.c). The wire path is
//! exercised end-to-end by the Tier 3 `test_integration_voice_rejoin_media`
//! against Janus.

use std::ffi::{c_char, c_void};
use std::os::raw::c_int;

use hotline_proto::build::HxChunk;
use hotline_proto::messages::ClientHdr;
use hotline_proto::voice;

// Wire opcodes — the single source of truth is hotline_proto::messages::
// ClientHdr (the repr(u32) HTLC_HDR_* enum), not re-spelled magic numbers.
const HTLC_HDR_VOICE_JOIN: u32 = ClientHdr::VoiceJoin as u32;
const HTLC_HDR_VOICE_LEAVE: u32 = ClientHdr::VoiceLeave as u32;
const HTLC_HDR_VOICE_SDP_ANSWER: u32 = ClientHdr::VoiceSdpAnswer as u32;
const HTLC_HDR_VOICE_ICE: u32 = ClientHdr::VoiceIce as u32;
const HTLC_HDR_VOICE_MUTE: u32 = ClientHdr::VoiceMute as u32;

/// `rcv_task_fn` (protocol.h): the reply-handler shape `task_new` stores —
/// `(htlc, frame, frame_len, ptr, data)`. `hx_rcv_task` hands the registered
/// callback the received frame as a `(frame, frame_len)` slice ahead of the
/// task `ptr` / `data`.
type RcvTaskFn = unsafe extern "C" fn(*mut c_void, *const c_void, usize, *mut c_void, *mut c_void);

// Real build: these resolve at the final C link. Test build: the `use
// tests::{…}` below shadows them with recording stubs (see the cfg(test)
// block), so the extern declarations are gated off to avoid a name clash.
#[cfg(not(test))]
extern "C" {
    // voice_bridge.c — htlc->caps & HTLC_CAP_VOICE.
    fn hx_htlc_voice_cap(htlc: *mut c_void) -> glib::ffi::gboolean;

    // tasks.c / network.c — the send-path primitives. hlwrite_chunks takes the
    // native HxChunk (repr(C), layout-pinned identical to C's struct hx_chunk).
    fn task_new(
        htlc: *mut c_void,
        rcv: RcvTaskFn,
        ptr: *mut c_void,
        data: *mut c_void,
        str_: *const c_char,
    ) -> *mut c_void;
    fn hlwrite_chunks(htlc: *mut c_void, ty: u32, flag: u32, chunks: *const HxChunk, hc: c_int);

    // rcv.c — reply-task handlers. Declared with the 3-arg RcvTaskFn shape
    // (see the typedef note); the linker resolves the real symbols.
    fn rcv_task_voice_join(
        htlc: *mut c_void,
        frame: *const c_void,
        frame_len: usize,
        ptr: *mut c_void,
        data: *mut c_void,
    );
    fn rcv_task_voice_simple_ack(
        htlc: *mut c_void,
        frame: *const c_void,
        frame_len: usize,
        ptr: *mut c_void,
        data: *mut c_void,
    );
}

// The C send-path primitives are stubbed under `cfg(test)` (see tests.rs), so
// the cargo-test build resolves without linking network.c / tasks.c / rcv.c.
#[cfg(test)]
use tests::{
    hlwrite_chunks, hx_htlc_voice_cap, rcv_task_voice_join, rcv_task_voice_simple_ack, task_new,
};

/// `GUINT_TO_POINTER` for a u32.
fn to_ptr(v: u32) -> *mut c_void {
    v as usize as *mut c_void
}

/// Borrow `(ptr, len)` as a slice, treating NULL / 0 / oversized as empty
/// (from_raw_parts' precondition: no NULL base, len <= isize::MAX).
unsafe fn slice_or_empty<'a>(ptr: *const u8, len: usize) -> &'a [u8] {
    if ptr.is_null() || len == 0 || len > isize::MAX as usize {
        &[]
    } else {
        std::slice::from_raw_parts(ptr, len)
    }
}

/// Cap gate — every send is a no-op unless the server echoed HTLC_CAP_VOICE.
unsafe fn cap_ok(htlc: *mut c_void) -> bool {
    if htlc.is_null() {
        return false;
    }
    if hx_htlc_voice_cap(htlc) == glib::ffi::GFALSE {
        glib::g_debug!("gtkhx", "skip voice send: server didn't echo CAP_VOICE");
        return false;
    }
    true
}

/// `gboolean hx_send_voice_join(struct htlc_conn *htlc, guint32 cid)`.
///
/// # Safety
/// `htlc` is NULL or a valid `htlc_conn *`; main thread only.
#[no_mangle]
pub unsafe extern "C" fn hx_send_voice_join(htlc: *mut c_void, cid: u32) -> glib::ffi::gboolean {
    if !cap_ok(htlc) {
        return glib::ffi::GFALSE;
    }
    let mut chunks = [HxChunk::EMPTY; 1];
    let mut scratch = [0u8; 4];
    let hc = voice::build_voice_join_chunks(cid, &mut chunks, &mut scratch);
    if hc == 0 {
        glib::g_debug!("gtkhx", "VOICE_JOIN builder failed");
        return glib::ffi::GFALSE;
    }
    // Register the JOIN reply task BEFORE the wire send so hx_rcv_task finds
    // it when the reply (server SDP offer + codec + participants) lands.
    task_new(
        htlc,
        rcv_task_voice_join,
        to_ptr(cid),
        std::ptr::null_mut(),
        c"voice-join".as_ptr(),
    );
    hlwrite_chunks(htlc, HTLC_HDR_VOICE_JOIN, 0, chunks.as_ptr(), hc as c_int);
    glib::ffi::GTRUE
}

/// `gboolean hx_send_voice_leave(struct htlc_conn *htlc, guint32 cid)`.
///
/// # Safety
/// See `hx_send_voice_join`.
#[no_mangle]
pub unsafe extern "C" fn hx_send_voice_leave(htlc: *mut c_void, cid: u32) -> glib::ffi::gboolean {
    if !cap_ok(htlc) {
        return glib::ffi::GFALSE;
    }
    let mut chunks = [HxChunk::EMPTY; 1];
    let mut scratch = [0u8; 4];
    let hc = voice::build_voice_leave_chunks(cid, &mut chunks, &mut scratch);
    if hc == 0 {
        glib::g_debug!("gtkhx", "VOICE_LEAVE builder failed");
        return glib::ffi::GFALSE;
    }
    // 601 reply is an empty-body success; simple_ack logs completion.
    task_new(
        htlc,
        rcv_task_voice_simple_ack,
        to_ptr(HTLC_HDR_VOICE_LEAVE),
        to_ptr(cid),
        c"voice-leave".as_ptr(),
    );
    hlwrite_chunks(htlc, HTLC_HDR_VOICE_LEAVE, 0, chunks.as_ptr(), hc as c_int);
    glib::ffi::GTRUE
}

/// `gboolean hx_send_voice_sdp_answer(struct htlc_conn *htlc, guint32 cid,
/// const guint8 *sdp, gsize sdp_len)`.
///
/// # Safety
/// `sdp` is NULL or valid for `sdp_len`; `htlc` as above.
#[no_mangle]
pub unsafe extern "C" fn hx_send_voice_sdp_answer(
    htlc: *mut c_void,
    cid: u32,
    sdp: *const u8,
    sdp_len: usize,
) -> glib::ffi::gboolean {
    if !cap_ok(htlc) {
        return glib::ffi::GFALSE;
    }
    if sdp.is_null() || sdp_len == 0 {
        glib::g_debug!("gtkhx", "VOICE_SDP_ANSWER: empty SDP rejected");
        return glib::ffi::GFALSE;
    }
    let sdp_slice = slice_or_empty(sdp, sdp_len);
    let mut chunks = [HxChunk::EMPTY; 2];
    let mut scratch = [0u8; 4];
    let hc = voice::build_voice_answer_chunks(cid, sdp_slice, &mut chunks, &mut scratch);
    if hc == 0 {
        glib::g_debug!(
            "gtkhx",
            "VOICE_SDP_ANSWER builder failed (sdp_len={sdp_len})"
        );
        return glib::ffi::GFALSE;
    }
    // 603 reply: empty-body success; an error here is fatal to the session.
    task_new(
        htlc,
        rcv_task_voice_simple_ack,
        to_ptr(HTLC_HDR_VOICE_SDP_ANSWER),
        to_ptr(cid),
        c"voice-sdp-answer".as_ptr(),
    );
    hlwrite_chunks(
        htlc,
        HTLC_HDR_VOICE_SDP_ANSWER,
        0,
        chunks.as_ptr(),
        hc as c_int,
    );
    glib::ffi::GTRUE
}

/// `gboolean hx_send_voice_ice(struct htlc_conn *htlc, guint32 cid,
/// const guint8 *ice, gsize ice_len)`. Empty ice = end-of-candidates marker.
///
/// # Safety
/// `ice` is NULL or valid for `ice_len`; `htlc` as above.
#[no_mangle]
pub unsafe extern "C" fn hx_send_voice_ice(
    htlc: *mut c_void,
    cid: u32,
    ice: *const u8,
    ice_len: usize,
) -> glib::ffi::gboolean {
    if !cap_ok(htlc) {
        return glib::ffi::GFALSE;
    }
    // Empty ICE is the legitimate end-of-candidates marker, so a NULL pointer
    // is ONLY valid with ice_len == 0. Don't coerce a caller bug — (NULL,
    // non-zero) or an oversized length — into that marker: it would silently
    // emit an EOC frame and corrupt negotiation. Fail those explicitly.
    if (ice.is_null() && ice_len != 0) || ice_len > isize::MAX as usize {
        glib::g_debug!("gtkhx", "VOICE_ICE: invalid ice ptr/len (len={ice_len})");
        return glib::ffi::GFALSE;
    }
    let ice_slice: &[u8] = if ice.is_null() {
        &[] // (NULL, 0) — end-of-candidates marker
    } else {
        std::slice::from_raw_parts(ice, ice_len)
    };
    let mut chunks = [HxChunk::EMPTY; 2];
    let mut scratch = [0u8; 4];
    let hc = voice::build_voice_ice_chunks(cid, ice_slice, &mut chunks, &mut scratch);
    if hc == 0 {
        glib::g_debug!("gtkhx", "VOICE_ICE builder failed (ice_len={ice_len})");
        return glib::ffi::GFALSE;
    }
    // 604 VOICE_ICE is a bidirectional notification — no reply, no task.
    hlwrite_chunks(htlc, HTLC_HDR_VOICE_ICE, 0, chunks.as_ptr(), hc as c_int);
    glib::ffi::GTRUE
}

/// `gboolean hx_send_voice_mute(struct htlc_conn *htlc, guint32 cid,
/// gboolean muted)`.
///
/// # Safety
/// `htlc` is NULL or a valid `htlc_conn *`; main thread only.
#[no_mangle]
pub unsafe extern "C" fn hx_send_voice_mute(
    htlc: *mut c_void,
    cid: u32,
    muted: glib::ffi::gboolean,
) -> glib::ffi::gboolean {
    if !cap_ok(htlc) {
        return glib::ffi::GFALSE;
    }
    // Normalise to exactly 0/1 for the strict u16-in-{0,1} wire field.
    let wire_muted: u16 = if muted != glib::ffi::GFALSE { 1 } else { 0 };
    let mut chunks = [HxChunk::EMPTY; 2];
    let mut scratch = [0u8; 6];
    let hc = voice::build_voice_mute_chunks(cid, wire_muted, &mut chunks, &mut scratch);
    if hc == 0 {
        glib::g_debug!("gtkhx", "VOICE_MUTE builder failed");
        return glib::ffi::GFALSE;
    }
    // 606 reply: empty-body success; simple_ack / task_error toast on failure.
    task_new(
        htlc,
        rcv_task_voice_simple_ack,
        to_ptr(HTLC_HDR_VOICE_MUTE),
        to_ptr(cid),
        c"voice-mute".as_ptr(),
    );
    hlwrite_chunks(htlc, HTLC_HDR_VOICE_MUTE, 0, chunks.as_ptr(), hc as c_int);
    glib::ffi::GTRUE
}

#[cfg(test)]
mod tests;
