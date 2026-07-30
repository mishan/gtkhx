//! `hxchat-send` — chat wire-out senders (the Rust port of `src/chat.c`'s send
//! path, Phase R5).
//!
//! Thin wrappers that emit the client-initiated chat transactions — public /
//! private CHAT, and the CHAT_CREATE / _INVITE / _JOIN / _PART / _SUBJECT
//! room-management opcodes. Each one: encodes any text body for the wire
//! (via `gtkhx_text_for_wire`, the hxtext crate), builds the chunks with the
//! **native** `hotline_proto::build` builders (not the C-ABI
//! `gtkhx_proto_build_*` shims — the whole build flow is Rust), registers a
//! reply task where the C original did, and hands the chunks to
//! `hlwrite_chunks`. Exports the exact `hx_send_chat` / `hx_chat_*` /
//! `hx_invite_user` / `hx_part_chat` / `hx_reject_chat` / `hx_change_subject`
//! C ABI so every caller (toolbar.c, users.c, the chat input handler, the
//! Rust invite dialog) links unchanged.
//!
//! A lean dedicated crate (only `glib` + the pure `hotline-proto`, no GTK) so
//! it's `cargo test`-able: the builders run natively and the C send-path
//! primitives are stubbed in the test module.
//!
//! What stays C behind the FFI seam is the send-path *infrastructure*, not the
//! protocol: the text encoder (`gtkhx_text_for_wire`, hxtext), the per-htlc
//! cap + chat-model lookups (`chat_send_bridge.c`), the task table (`task_new`
//! and the `hx_rcv_user_change` / `rcv_task_user_list_switch` reply handlers,
//! rcv.c), and the write primitive (`hlwrite_chunks`, network.c).

use std::ffi::{c_char, c_void};
use std::os::raw::c_int;

use hotline_proto::build::{
    self, ChatRequest, ChatSubjectRequest, HxChunk,
};
use hotline_proto::messages::ClientHdr;

// Wire opcodes — single source of truth is hotline_proto::messages::ClientHdr
// (the repr(u32) HTLC_HDR_* enum), not re-spelled magic numbers.
const HTLC_HDR_CHAT: u32 = ClientHdr::Chat as u32;
const HTLC_HDR_CHAT_CREATE: u32 = ClientHdr::ChatCreate as u32;
const HTLC_HDR_CHAT_INVITE: u32 = ClientHdr::ChatInvite as u32;
const HTLC_HDR_CHAT_JOIN: u32 = ClientHdr::ChatJoin as u32;
const HTLC_HDR_CHAT_PART: u32 = ClientHdr::ChatPart as u32;
const HTLC_HDR_CHAT_SUBJECT: u32 = ClientHdr::ChatSubject as u32;
const HTLC_HDR_CHAT_DECLINE: u32 = ClientHdr::ChatDecline as u32;

/// `rcv_task_fn` (protocol.h): the reply-handler shape `task_new` stores —
/// `(htlc, frame, frame_len, ptr, data)`. `hx_rcv_task` hands the registered
/// callback the received frame as a `(frame, frame_len)` slice ahead of the
/// task `ptr` / `data`. `hx_rcv_user_change` is a primary handler
/// `(htlc, frame, frame_len)` reused here as a reply handler — it reads the
/// first three args in both calling conventions and ignores `ptr` / `data`.
type RcvTaskFn = unsafe extern "C" fn(*mut c_void, *const c_void, usize, *mut c_void, *mut c_void);

// Real build: these resolve at the final C link. Test build: the `use
// tests::{…}` below shadows them with recording stubs, so the extern
// declarations are gated off to avoid a name clash.
#[cfg(not(test))]
use hxtext::gtkhx_text_for_wire;

#[cfg(not(test))]
extern "C" {
    // chat_send_bridge.c — per-htlc cap + chat-model lookups.
    fn hx_htlc_text_encoding_cap(htlc: *mut c_void) -> glib::ffi::gboolean;
    fn hx_chat_lookup(htlc: *mut c_void, cid: u32) -> *mut c_void;
    fn hx_chat_lookup_or_create(htlc: *mut c_void, cid: u32) -> *mut c_void;

    // tasks.c / network.c — the send-path primitives. hlwrite_chunks takes the
    // native HxChunk (repr(C), layout-pinned identical to C's struct hx_chunk).
    fn task_new(
        htlc: *mut c_void,
        // Nullable: the CHAT_INVITE ack registers a task with no reply handler
        // (`task_new(htlc, 0, …)` in the C original). Option<fn> is the
        // null-optimized FFI-safe way to pass that 0.
        rcv: Option<RcvTaskFn>,
        ptr: *mut c_void,
        data: *mut c_void,
        str_: *const c_char,
    ) -> *mut c_void;
    fn hlwrite_chunks(
        htlc: *mut c_void,
        ty: u32,
        flag: u32,
        chunks: *const HxChunk,
        hc: c_int,
    );

    // rcv.c — reply-task handlers. Declared with the 3-arg RcvTaskFn shape (see
    // the typedef note); the linker resolves the real symbols.
    fn hx_rcv_user_change(htlc: *mut c_void, frame: *const c_void, frame_len: usize, ptr: *mut c_void, data: *mut c_void);
    fn rcv_task_user_list_switch(htlc: *mut c_void, frame: *const c_void, frame_len: usize, ptr: *mut c_void, data: *mut c_void);
}

// The C send-path primitives are stubbed under cfg(test) (see tests.rs), so the
// cargo-test build resolves without linking hxtext / chat_send_bridge / tasks /
// network / rcv.
#[cfg(test)]
use tests::{
    gtkhx_text_for_wire, hlwrite_chunks, hx_chat_lookup, hx_chat_lookup_or_create,
    hx_htlc_text_encoding_cap, hx_rcv_user_change, rcv_task_user_list_switch, task_new,
};

/// A NUL-terminated C string's bytes (without the NUL), or empty for NULL.
unsafe fn cstr_bytes<'a>(s: *const c_char) -> &'a [u8] {
    if s.is_null() {
        &[]
    } else {
        std::ffi::CStr::from_ptr(s).to_bytes()
    }
}

/// Encode `text` (a C string) for the wire on this connection: UTF-8 verbatim
/// when CAP_TEXT_ENCODING was negotiated, else Mac Roman (`?` fallback), LF→CR
/// when `is_body`. Runs the encoded bytes through `f`, then g_free's the
/// buffer. `f` must not retain the slice past its own return.
unsafe fn with_wire<R>(
    htlc: *mut c_void,
    text: *const c_char,
    is_body: glib::ffi::gboolean,
    f: impl FnOnce(&[u8]) -> R,
) -> R {
    let bytes = cstr_bytes(text);
    let utf8_mode = hx_htlc_text_encoding_cap(htlc);
    let mut wire_len: usize = 0;
    let wire = gtkhx_text_for_wire(
        text,
        bytes.len(),
        utf8_mode,
        is_body,
        &mut wire_len,
    );
    // gtkhx_text_for_wire never returns NULL (empty buffer on any guard), but
    // treat NULL / 0 / oversized as empty defensively — from_raw_parts requires
    // a non-NULL base and len <= isize::MAX.
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

/// `void hx_send_chat(struct htlc_conn *htlc, char *str, guint32 cid,
/// guint16 style)` — public (cid 0) or private chat line. No reply task.
///
/// # Safety
/// `htlc` is NULL or a valid `htlc_conn *`; `str` is a NUL-terminated C string
/// or NULL; main thread only.
#[no_mangle]
pub unsafe extern "C" fn hx_send_chat(
    htlc: *mut c_void,
    str_: *const c_char,
    cid: u32,
    style: u16,
) {
    // task_new / hlwrite_chunks dereference htlc on the C side (htlc->trans,
    // htlc->fd); a NULL would crash there. Guard here — every sender does.
    if htlc.is_null() {
        return;
    }
    with_wire(htlc, str_, glib::ffi::GTRUE, |wire| {
        let mut chunks = [HxChunk::EMPTY; 3];
        let mut scratch = [0u8; 8];
        let req = ChatRequest {
            cid,
            style,
            body: wire,
        };
        let hc = build::build_chat_chunks(&req, &mut chunks, &mut scratch);
        if hc > 0 {
            hlwrite_chunks(htlc, HTLC_HDR_CHAT, 0, chunks.as_ptr(), hc as c_int);
        }
    });
}

/// `void hx_chat_user(struct htlc_conn *htlc, guint16 uid)` — open a private
/// chat with `uid` (CHAT_CREATE; reply drives `hx_rcv_user_change`).
///
/// # Safety
/// `htlc` is NULL or a valid `htlc_conn *`; main thread only.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_user(htlc: *mut c_void, uid: u16) {
    if htlc.is_null() {
        return;
    }
    let mut chunks = [HxChunk::EMPTY; 1];
    let mut scratch = [0u8; 2];
    let hc = build::build_chat_create_chunks(uid, &mut chunks, &mut scratch);
    if hc > 0 {
        // Build BEFORE task_new: a builder reject must not leave a phantom
        // task with no on-wire request (see the C original's comment).
        task_new(
            htlc,
            Some(hx_rcv_user_change),
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            c"chat".as_ptr(),
        );
        hlwrite_chunks(htlc, HTLC_HDR_CHAT_CREATE, 0, chunks.as_ptr(), hc as c_int);
    }
}

/// `void hx_invite_user(struct htlc_conn *htlc, guint16 uid, guint32 cid)` —
/// invite `uid` into chat `cid` (CHAT_INVITE; ack task, no handler).
///
/// # Safety
/// See `hx_chat_user`.
#[no_mangle]
pub unsafe extern "C" fn hx_invite_user(htlc: *mut c_void, uid: u16, cid: u32) {
    if htlc.is_null() {
        return;
    }
    let mut chunks = [HxChunk::EMPTY; 2];
    let mut scratch = [0u8; 6];
    let hc = build::build_chat_invite_chunks(cid, uid, &mut chunks, &mut scratch);
    if hc > 0 {
        task_new(
            htlc,
            None,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            c"invite".as_ptr(),
        );
        hlwrite_chunks(htlc, HTLC_HDR_CHAT_INVITE, 0, chunks.as_ptr(), hc as c_int);
    }
}

/// `void hx_chat_join(struct htlc_conn *htlc, guint32 cid)` — join chat `cid`
/// (CHAT_JOIN; reply drives `rcv_task_user_list_switch` with the chat ptr).
///
/// # Safety
/// See `hx_chat_user`.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_join(htlc: *mut c_void, cid: u32) {
    if htlc.is_null() {
        return;
    }
    // Look up (or seed) the chat before sending — the JOIN reply's
    // user-list-switch task carries the chat pointer. Always send the JOIN,
    // even when the chat was pre-registered by a self-invite CHAT_CREATE
    // reply (see the C original's self-invite comment).
    let chat = hx_chat_lookup_or_create(htlc, cid);

    let mut chunks = [HxChunk::EMPTY; 1];
    let mut scratch = [0u8; 4];
    let hc = build::build_chat_join_chunks(cid, &mut chunks, &mut scratch);
    if hc > 0 {
        task_new(
            htlc,
            Some(rcv_task_user_list_switch),
            chat,
            std::ptr::null_mut(),
            c"join".as_ptr(),
        );
        hlwrite_chunks(htlc, HTLC_HDR_CHAT_JOIN, 0, chunks.as_ptr(), hc as c_int);
    }
}

/// `void hx_part_chat(struct htlc_conn *htlc, guint32 cid)` — leave chat `cid`
/// (CHAT_PART; no task). Bails if the cid isn't known (UI-close / server
/// chat-delete race).
///
/// # Safety
/// See `hx_chat_user`.
#[no_mangle]
pub unsafe extern "C" fn hx_part_chat(htlc: *mut c_void, cid: u32) {
    if htlc.is_null() {
        return;
    }
    if hx_chat_lookup(htlc, cid).is_null() {
        return;
    }
    let mut chunks = [HxChunk::EMPTY; 1];
    let mut scratch = [0u8; 4];
    let hc = build::build_chat_part_chunks(cid, &mut chunks, &mut scratch);
    if hc > 0 {
        hlwrite_chunks(htlc, HTLC_HDR_CHAT_PART, 0, chunks.as_ptr(), hc as c_int);
    }
}

/// `void hx_reject_chat(struct htlc_conn *htlc, guint32 cid)` — decline a
/// pending chat invitation for `cid` (CHAT_DECLINE; no task). No membership
/// lookup: declining an invite is valid for a cid we never joined, so unlike
/// `hx_part_chat` there's nothing to find in the chat registry.
///
/// # Safety
/// See `hx_chat_user`.
#[no_mangle]
pub unsafe extern "C" fn hx_reject_chat(htlc: *mut c_void, cid: u32) {
    if htlc.is_null() {
        return;
    }
    let mut chunks = [HxChunk::EMPTY; 1];
    let mut scratch = [0u8; 4];
    let hc = build::build_chat_decline_chunks(cid, &mut chunks, &mut scratch);
    if hc > 0 {
        hlwrite_chunks(htlc, HTLC_HDR_CHAT_DECLINE, 0, chunks.as_ptr(), hc as c_int);
    }
}

/// `void hx_change_subject(struct htlc_conn *htlc, guint32 cid, char *subject)`
/// — set chat `cid`'s subject (CHAT_SUBJECT; no task). Single-line field, so
/// `is_body = FALSE` (no LF→CR).
///
/// # Safety
/// `htlc` is NULL or a valid `htlc_conn *`; `subject` is a NUL-terminated C
/// string or NULL; main thread only.
#[no_mangle]
pub unsafe extern "C" fn hx_change_subject(
    htlc: *mut c_void,
    cid: u32,
    subject: *const c_char,
) {
    if htlc.is_null() {
        return;
    }
    with_wire(htlc, subject, glib::ffi::GFALSE, |wire| {
        let mut chunks = [HxChunk::EMPTY; 2];
        let mut scratch = [0u8; 4];
        let req = ChatSubjectRequest { cid, subject: wire };
        let hc = build::build_chat_subject_chunks(&req, &mut chunks, &mut scratch);
        if hc > 0 {
            hlwrite_chunks(htlc, HTLC_HDR_CHAT_SUBJECT, 0, chunks.as_ptr(), hc as c_int);
        }
    });
}

#[cfg(test)]
mod tests;
