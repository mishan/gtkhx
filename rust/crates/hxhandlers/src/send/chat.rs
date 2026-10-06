//! `hxhandlers::send::chat` — chat wire-out senders (the Rust port of `src/chat.c`'s send
//! path, Phase R5).
//!
//! Thin wrappers that emit the client-initiated chat transactions — public /
//! private CHAT, and the CHAT_CREATE / _INVITE / _JOIN / _PART / _SUBJECT
//! room-management opcodes. Each one: encodes any text body for the wire
//! (via `gtkhx_text_for_wire`, the hxtext crate), builds the chunks with the
//! **native** `hxproto::build` builders (not the C-ABI
//! `gtkhx_proto_build_*` shims — the whole build flow is Rust), registers a
//! reply task where the C original did, and hands the chunks to
//! `hlwrite_chunks`. Exports the exact `hx_send_chat` / `hx_chat_*` /
//! `hx_invite_user` / `hx_part_chat` / `hx_reject_chat` / `hx_change_subject`
//! C ABI so every caller (toolbar.c, users.c, the chat input handler, the
//! Rust invite dialog) links unchanged.
//!
//! A lean dedicated crate (only `glib` + the pure `hxproto`, no GTK) so
//! it's `cargo test`-able: the builders run natively and the C send-path
//! primitives are stubbed in the test module.
//!
//! What stays C behind the FFI seam is the send-path *infrastructure*, not the
//! protocol: the text encoder (`gtkhx_text_for_wire`, hxtext), the per-htlc
//! cap + chat-model lookups (`chat_send_bridge.c`), and the write primitive
//! (`hlwrite_chunks`, network.c).

use std::ffi::{c_char, c_void};
use std::os::raw::c_int;

use hxproto::build::{self, ChatRequest, ChatSubjectRequest, HxChunk};
use hxproto::messages::{tag, ClientHdr};

// Wire opcodes — single source of truth is hxproto::messages::ClientHdr
// (the repr(u32) HTLC_HDR_* enum), not re-spelled magic numbers.
const HTLC_HDR_CHAT: u32 = ClientHdr::Chat as u32;
const HTLC_HDR_CHAT_CREATE: u32 = ClientHdr::ChatCreate as u32;
const HTLC_HDR_CHAT_INVITE: u32 = ClientHdr::ChatInvite as u32;
const HTLC_HDR_CHAT_JOIN: u32 = ClientHdr::ChatJoin as u32;
const HTLC_HDR_CHAT_PART: u32 = ClientHdr::ChatPart as u32;
const HTLC_HDR_CHAT_SUBJECT: u32 = ClientHdr::ChatSubject as u32;
const HTLC_HDR_CHAT_DECLINE: u32 = ClientHdr::ChatDecline as u32;
const HTLC_CAP_INLINE_MEDIA: u64 = hxsession::cap::INLINE_MEDIA as u64;

// Real build: these resolve at the final C link. Test build: the `use
// tests::{…}` below shadows them with recording stubs, so the extern
// declarations are gated off to avoid a name clash.
#[cfg(not(test))]
use gtkhx_core::conn::hx_conn_has_cap;
#[cfg(not(test))]
use hxtext::gtkhx_text_for_wire;

#[cfg(not(test))]
extern "C" {
    // chat_send_bridge.c — per-htlc cap + chat-model lookups.
    fn hx_htlc_text_encoding_cap(htlc: *mut c_void) -> glib::ffi::gboolean;
    fn hx_chat_lookup(htlc: *mut c_void, cid: u32) -> *mut c_void;

    // network.c — the send primitive. hlwrite_chunks takes the native HxChunk
    // (repr(C), layout-pinned identical to C's struct hx_chunk).
    fn hlwrite_chunks(htlc: *mut c_void, ty: u32, flag: u32, chunks: *const HxChunk, hc: c_int);
}

// The C send-path primitives are stubbed under cfg(test) (see tests.rs), so the
// cargo-test build resolves without linking hxtext / chat_send_bridge /
// network.
#[cfg(test)]
use tests::{
    gtkhx_text_for_wire, hlwrite_chunks, hx_chat_lookup, hx_conn_has_cap, hx_htlc_text_encoding_cap,
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
pub(super) unsafe fn with_wire<R>(
    htlc: *mut c_void,
    text: *const c_char,
    is_body: glib::ffi::gboolean,
    f: impl FnOnce(&[u8]) -> R,
) -> R {
    let bytes = cstr_bytes(text);
    let utf8_mode = hx_htlc_text_encoding_cap(htlc);
    let mut wire_len: usize = 0;
    let wire = gtkhx_text_for_wire(text, bytes.len(), utf8_mode, is_body, &mut wire_len);
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
    send_chat(htlc, str_, cid, style, None);
}

/// `void hx_send_chat_with_media (htlc, str, cid, style, media_id,
/// media_id_len, mime, mime_len)` — a chat line carrying the picture an
/// upload gave the handle and type of. Without both, or where the server
/// did not agree to inline media, it goes as a plain line: a server drops
/// the picture of a sender that did not agree to it anyway.
///
/// # Safety
/// As [`hx_send_chat`]; `media_id` points at `media_id_len` bytes and `mime`
/// at `mime_len`, when not NULL.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn hx_send_chat_with_media(
    htlc: *mut c_void,
    str_: *const c_char,
    cid: u32,
    style: u16,
    media_id: *const u8,
    media_id_len: usize,
    mime: *const c_char,
    mime_len: usize,
) {
    if htlc.is_null() || str_.is_null() {
        return;
    }
    let field = |p: *const u8, len: usize| {
        (!p.is_null() && (1..=u16::MAX as usize).contains(&len))
            .then(|| std::slice::from_raw_parts(p, len))
    };
    let media = field(media_id, media_id_len)
        .zip(field(mime.cast(), mime_len))
        .filter(|_| hx_conn_has_cap(htlc.cast(), HTLC_CAP_INLINE_MEDIA) != glib::ffi::GFALSE);
    send_chat(htlc, str_, cid, style, media);
}

/// A chat line, with the picture `media` names (its handle and type).
unsafe fn send_chat(
    htlc: *mut c_void,
    str_: *const c_char,
    cid: u32,
    style: u16,
    media: Option<(&[u8], &[u8])>,
) {
    // hlwrite_chunks dereferences htlc (htlc->trans, htlc->fd); a NULL would
    // crash there. Guard here — every sender does.
    if htlc.is_null() {
        return;
    }
    with_wire(htlc, str_, glib::ffi::GTRUE, |wire| {
        let mut chunks = [HxChunk::EMPTY; 5];
        let mut scratch = [0u8; 8];
        let req = ChatRequest {
            cid,
            style,
            body: wire,
        };
        let mut hc = build::build_chat_chunks(&req, &mut chunks, &mut scratch);
        if hc == 0 {
            return;
        }
        if let Some((id, mime)) = media {
            for (tag, data) in [(tag::CHAT_MEDIA_ID, id), (tag::CHAT_MEDIA_TYPE, mime)] {
                chunks[hc] = HxChunk {
                    tag,
                    len: data.len() as u16,
                    data: data.as_ptr(),
                };
                hc += 1;
            }
        }
        hlwrite_chunks(htlc, HTLC_HDR_CHAT, 0, chunks.as_ptr(), hc as c_int);
    });
}

/// `void hx_chat_user(struct htlc_conn *htlc, guint16 uid)` — open a private
/// chat with `uid` (CHAT_CREATE). Its reply comes back as the session's
/// `ChatCreated`.
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
        super::expect_next(htlc, hxsession::Expect::ChatCreate);
        hlwrite_chunks(htlc, HTLC_HDR_CHAT_CREATE, 0, chunks.as_ptr(), hc as c_int);
    }
}

/// `void hx_invite_user(struct htlc_conn *htlc, guint16 uid, guint32 cid)` —
/// invite `uid` into chat `cid` (CHAT_INVITE). Its reply says nothing unless
/// the server refused, which the session reports.
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
        super::expect_next(htlc, hxsession::Expect::ChatInvite);
        hlwrite_chunks(htlc, HTLC_HDR_CHAT_INVITE, 0, chunks.as_ptr(), hc as c_int);
    }
}

/// `void hx_chat_join(struct htlc_conn *htlc, guint32 cid)` — join chat `cid`
/// (CHAT_JOIN). Its reply comes back as the session's `ChatJoined`, which
/// makes the chat; a refused join leaves none behind. Sent even for a chat
/// we already have: creating one with ourselves invites us to it.
///
/// # Safety
/// See `hx_chat_user`.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_join(htlc: *mut c_void, cid: u32) {
    if htlc.is_null() {
        return;
    }
    let mut chunks = [HxChunk::EMPTY; 1];
    let mut scratch = [0u8; 4];
    let hc = build::build_chat_join_chunks(cid, &mut chunks, &mut scratch);
    if hc > 0 {
        let trans = super::expect_next(htlc, hxsession::Expect::ChatJoin { cid });
        crate::recv::user::join_requested(htlc, trans, cid);
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
    crate::recv::user::join_parted(htlc, cid);
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
pub unsafe extern "C" fn hx_change_subject(htlc: *mut c_void, cid: u32, subject: *const c_char) {
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
