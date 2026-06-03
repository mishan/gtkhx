//! Outgoing-message builders for the SEND path.
//!
//! Phase R2 ports each `hx_send_*` function in C (chat.c, msg.c,
//! network.c) into a `build_*_chunks` helper here. The C wrapper still
//! calls `hlwrite_chunks()` (network.c) to push bytes onto the socket —
//! this module is purely about the "build the message" step, mirroring
//! the existing `agreement_packet.c::hx_agreement_agree_build_chunks`
//! pattern.
//!
//! ## API shape (matches `agreement_packet.c`)
//!
//! Each builder takes a typed request struct plus two caller-owned
//! buffers:
//!
//! - `chunks: &mut [HxChunk]` — populated with (tag, len, data-ptr)
//!   triples on success. The caller hands the slice to `hlwrite_chunks`
//!   which copies bytes out before the buffers go out of scope.
//! - `scratch: &mut [u8]` — backing storage for the integer chunks
//!   (style/uid/cid, big-endian). Variable-length payloads (the chat
//!   body, the message body) live in the request struct's `body`
//!   pointer; the FFI documents the lifetime constraint.
//!
//! Returns the number of chunks populated, or `0` on validation
//! failure (buffer too small / NULL pointer). Matches the C builder
//! return convention so the dispatch site can `if (hc > 0) hlwrite_chunks(...)`.
//!
//! ## What stays in C
//!
//! Text conversion (`gtkhx_text_for_wire`, the iconv-backed UTF-8 ↔
//! Mac Roman path in `src/text_util.c`) is the caller's responsibility —
//! the builder receives already-encoded body bytes. This keeps the
//! Rust crate free of GLib iconv dependencies, same discipline as the
//! existing C `agreement_packet.c`.

use crate::messages::tag;

/// C-ABI mirror of `struct hx_chunk` in `src/proto_helpers.h`. Layout
/// must match exactly:
///
/// ```c
/// struct hx_chunk {
///     guint16 type;
///     guint16 len;
///     const void *data;
/// };
/// ```
///
/// On 64-bit hosts that's `u16 + u16 + 4 pad + 8-byte ptr` = 16 bytes.
/// `#[repr(C)]` reproduces the C compiler's natural alignment of the
/// pointer, so we don't spell the padding out.
#[repr(C)]
pub struct HxChunk {
    pub tag: u16,
    pub len: u16,
    pub data: *const u8,
}

// Compile-time pin on the HxChunk layout. Same discipline as the
// Phase R1 AEAD state's size_of assert (cipher.h has a paired
// `_Static_assert(sizeof(chacha_aead_state) == 48, ...)`). If a future
// edit reorders the fields or accidentally changes the ABI, this fails
// the build before any C caller can read garbage at runtime.
//
// The numbers below are derived from pointer-width so they hold on
// both 64-bit (size 16, data offset 8) and 32-bit (size 8, data
// offset 4) targets — both layouts a `#[repr(C)]` mirror of
// `{ u16, u16, const void * }` produces under natural alignment.
const _: () = {
    assert!(std::mem::offset_of!(HxChunk, tag) == 0);
    assert!(std::mem::offset_of!(HxChunk, len) == 2);
    assert!(std::mem::offset_of!(HxChunk, data) == std::mem::size_of::<*const u8>());
    assert!(std::mem::size_of::<HxChunk>() == 2 * std::mem::size_of::<*const u8>());
    assert!(std::mem::align_of::<HxChunk>() == std::mem::align_of::<*const u8>());
};

impl HxChunk {
    /// Fresh empty chunk (tag 0, len 0, NULL data). Used to pre-fill
    /// the caller's chunks buffer in tests; production callers
    /// overwrite every slot the builder fills.
    pub const EMPTY: HxChunk = HxChunk {
        tag: 0,
        len: 0,
        data: std::ptr::null(),
    };
}

// Builders here borrow `body` (and similar pointer fields) — the
// returned chunks reference the original buffer via raw pointers. The
// builders themselves never read those pointers; they only stash them
// into the chunk array for the C side to copy out via hlwrite_chunks.

/// Request data for [`build_chat_chunks`].
pub struct ChatRequest<'a> {
    /// Private-chat channel id, or 0 for the public/lobby chat. The
    /// C handler treats 0 as "no CHAT_ID chunk" — historical Hotline
    /// servers default to lobby for that case.
    pub cid: u32,
    /// Style bitmap (mIRC-ish flags). Always emitted as a u16 chunk.
    pub style: u16,
    /// Already-encoded body bytes (UTF-8 if CAP_TEXT_ENCODING was
    /// negotiated, Mac Roman otherwise — see `gtkhx_text_for_wire`).
    /// Empty bodies are legal and emit a zero-length CHAT chunk.
    pub body: &'a [u8],
}

/// Build the chunk array for `HTLC_HDR_CHAT`. Wire shape:
///
/// - `HTLC_DATA_STYLE` (`tag::STYLE`, 0x006d) — u16 BE, 2 bytes.
/// - `HTLC_DATA_CHAT` (`tag::BODY`, 0x0065) — body bytes (already encoded).
/// - `HTLC_DATA_CHAT_ID` (`tag::CHAT_ID`, 0x0072) — u32 BE, 4 bytes,
///   **only when `cid != 0`**. Public chat (cid == 0) omits this chunk.
///
/// Scratch usage: 6 bytes (style at +0, cid at +2). The chunks
/// `data` pointers reference into `scratch` and into `req.body`, so
/// both buffers must outlive the eventual `hlwrite_chunks` call.
///
/// Returns 2 (no cid) or 3 (with cid) on success, or 0 on validation
/// failure: `chunks` has fewer than 3 slots, `scratch` is shorter than
/// 6 bytes, or `body.len() > u16::MAX` (Hotline chunk lengths are
/// 16-bit on the wire — without this check, a Rust caller passing a
/// 65 KiB body would silently wrap and emit an invalid chunk).
pub fn build_chat_chunks(
    req: &ChatRequest<'_>,
    chunks: &mut [HxChunk],
    scratch: &mut [u8],
) -> usize {
    if chunks.len() < 3 || scratch.len() < 6 || req.body.len() > u16::MAX as usize {
        return 0;
    }
    let with_cid = req.cid != 0;

    let style_be = req.style.to_be_bytes();
    let cid_be = req.cid.to_be_bytes();
    scratch[0..2].copy_from_slice(&style_be);
    scratch[2..6].copy_from_slice(&cid_be);

    chunks[0] = HxChunk {
        tag: tag::STYLE,
        len: 2,
        data: scratch.as_ptr(),
    };
    chunks[1] = HxChunk {
        tag: tag::BODY,
        len: req.body.len() as u16,
        // Empty body is legal: hand the C side a non-NULL pointer so
        // it doesn't trip a defensive null-check; len 0 means no bytes
        // are read anyway.
        data: if req.body.is_empty() {
            b"".as_ptr()
        } else {
            req.body.as_ptr()
        },
    };
    if with_cid {
        chunks[2] = HxChunk {
            tag: tag::CHAT_ID,
            len: 4,
            // Cid lives at scratch[2..6]; take a subslice and read its
            // pointer — safe and bounds-checked, vs. the manual
            // pointer-add this used to do.
            data: scratch[2..6].as_ptr(),
        };
        3
    } else {
        2
    }
}

/// Request data for [`build_msg_chunks`].
pub struct MsgRequest<'a> {
    /// Recipient user id. Always emitted as a u16 chunk.
    pub uid: u16,
    /// Already-encoded body bytes.
    pub body: &'a [u8],
}

/// Build the chunk array for `HTLC_HDR_MSG`. Wire shape:
///
/// - `HTLC_DATA_UID` (`tag::UID`, 0x0067) — u16 BE, 2 bytes.
/// - `HTLC_DATA_MSG` (`tag::BODY`, 0x0065) — body bytes.
///
/// Scratch usage: 2 bytes (the uid). Returns 2 on success, or 0 on
/// validation failure: too-small chunks / scratch buffers, or
/// `body.len() > u16::MAX` (chunk lengths are 16-bit on the wire).
pub fn build_msg_chunks(
    req: &MsgRequest<'_>,
    chunks: &mut [HxChunk],
    scratch: &mut [u8],
) -> usize {
    if chunks.len() < 2 || scratch.len() < 2 || req.body.len() > u16::MAX as usize {
        return 0;
    }

    let uid_be = req.uid.to_be_bytes();
    scratch[0..2].copy_from_slice(&uid_be);

    chunks[0] = HxChunk {
        tag: tag::UID,
        len: 2,
        data: scratch.as_ptr(),
    };
    chunks[1] = HxChunk {
        tag: tag::BODY,
        len: req.body.len() as u16,
        data: if req.body.is_empty() {
            b"".as_ptr()
        } else {
            req.body.as_ptr()
        },
    };
    2
}

/// Request data for [`build_broadcast_chunks`].
pub struct BroadcastRequest<'a> {
    /// Already-encoded body bytes.
    pub body: &'a [u8],
}

/// Build the chunk array for `HTLC_HDR_MSG_BROADCAST`. Wire shape:
///
/// - `HTLC_DATA_MSG` (`tag::BODY`, 0x0065) — body bytes.
///
/// No scratch needed (no integer chunks). Returns 1 on success, or 0
/// on validation failure: empty `chunks` slice, or `body.len() >
/// u16::MAX` (chunk lengths are 16-bit on the wire).
pub fn build_broadcast_chunks(
    req: &BroadcastRequest<'_>,
    chunks: &mut [HxChunk],
) -> usize {
    if chunks.is_empty() || req.body.len() > u16::MAX as usize {
        return 0;
    }
    chunks[0] = HxChunk {
        tag: tag::BODY,
        len: req.body.len() as u16,
        data: if req.body.is_empty() {
            b"".as_ptr()
        } else {
            req.body.as_ptr()
        },
    };
    1
}

// ---- Chat-admin send opcodes ------------------------------------------
//
// HTLC_HDR_CHAT_CREATE / _INVITE / _JOIN / _PART / _DECLINE / _SUBJECT
// are the chat-room management opcodes. Most are pure integer-chunk
// requests; the only one with a variable-length payload is
// CHAT_SUBJECT (the subject body).

/// Build chunks for `HTLC_HDR_CHAT_CREATE` — `HTLC_DATA_UID` (u16 BE)
/// only, 1 chunk. Requires `chunks_cap >= 1`, `scratch_cap >= 2`.
pub fn build_chat_create_chunks(
    uid: u16,
    chunks: &mut [HxChunk],
    scratch: &mut [u8],
) -> usize {
    if chunks.is_empty() || scratch.len() < 2 {
        return 0;
    }
    scratch[0..2].copy_from_slice(&uid.to_be_bytes());
    chunks[0] = HxChunk {
        tag: tag::UID,
        len: 2,
        data: scratch.as_ptr(),
    };
    1
}

/// Build chunks for `HTLC_HDR_CHAT_INVITE` — `HTLC_DATA_CHAT_ID` (u32)
/// then `HTLC_DATA_UID` (u16). 2 chunks; `chunks_cap >= 2`,
/// `scratch_cap >= 6` (cid at offset 0, uid at offset 4).
pub fn build_chat_invite_chunks(
    cid: u32,
    uid: u16,
    chunks: &mut [HxChunk],
    scratch: &mut [u8],
) -> usize {
    if chunks.len() < 2 || scratch.len() < 6 {
        return 0;
    }
    scratch[0..4].copy_from_slice(&cid.to_be_bytes());
    scratch[4..6].copy_from_slice(&uid.to_be_bytes());
    chunks[0] = HxChunk {
        tag: tag::CHAT_ID,
        len: 4,
        data: scratch.as_ptr(),
    };
    chunks[1] = HxChunk {
        tag: tag::UID,
        len: 2,
        // Uid lives at scratch[4..6]; subslice + as_ptr is safe and
        // bounds-checked, scratch.len() >= 6 already verified above.
        data: scratch[4..6].as_ptr(),
    };
    2
}

/// Internal helper: single-`CHAT_ID` chunk for the JOIN / PART / DECLINE
/// opcodes (all three share the wire shape). The caller picks the
/// header type when handing the chunks to `hlwrite_chunks`.
fn build_chat_id_only_chunks(
    cid: u32,
    chunks: &mut [HxChunk],
    scratch: &mut [u8],
) -> usize {
    if chunks.is_empty() || scratch.len() < 4 {
        return 0;
    }
    scratch[0..4].copy_from_slice(&cid.to_be_bytes());
    chunks[0] = HxChunk {
        tag: tag::CHAT_ID,
        len: 4,
        data: scratch.as_ptr(),
    };
    1
}

/// Build chunks for `HTLC_HDR_CHAT_JOIN` — `HTLC_DATA_CHAT_ID` only.
pub fn build_chat_join_chunks(
    cid: u32,
    chunks: &mut [HxChunk],
    scratch: &mut [u8],
) -> usize {
    build_chat_id_only_chunks(cid, chunks, scratch)
}

/// Build chunks for `HTLC_HDR_CHAT_PART` — `HTLC_DATA_CHAT_ID` only.
pub fn build_chat_part_chunks(
    cid: u32,
    chunks: &mut [HxChunk],
    scratch: &mut [u8],
) -> usize {
    build_chat_id_only_chunks(cid, chunks, scratch)
}

/// Build chunks for `HTLC_HDR_CHAT_DECLINE` — `HTLC_DATA_CHAT_ID` only.
pub fn build_chat_decline_chunks(
    cid: u32,
    chunks: &mut [HxChunk],
    scratch: &mut [u8],
) -> usize {
    build_chat_id_only_chunks(cid, chunks, scratch)
}

/// Request data for [`build_chat_subject_chunks`].
pub struct ChatSubjectRequest<'a> {
    pub cid: u32,
    /// Already-encoded subject bytes (UTF-8 or Mac Roman per
    /// CAP_TEXT_ENCODING). Empty subject is legal.
    pub subject: &'a [u8],
}

/// Build chunks for `HTLC_HDR_CHAT_SUBJECT` — `HTLC_DATA_CHAT_ID` (u32)
/// + `HTLC_DATA_CHAT_SUBJECT` (bytes). 2 chunks; `chunks_cap >= 2`,
/// `scratch_cap >= 4`. Also rejects `subject.len() > u16::MAX` (the
/// wire chunk length is 16-bit; without this check a 65 KiB subject
/// would silently wrap and emit an invalid chunk).
pub fn build_chat_subject_chunks(
    req: &ChatSubjectRequest<'_>,
    chunks: &mut [HxChunk],
    scratch: &mut [u8],
) -> usize {
    if chunks.len() < 2 || scratch.len() < 4 || req.subject.len() > u16::MAX as usize {
        return 0;
    }
    scratch[0..4].copy_from_slice(&req.cid.to_be_bytes());
    chunks[0] = HxChunk {
        tag: tag::CHAT_ID,
        len: 4,
        data: scratch.as_ptr(),
    };
    chunks[1] = HxChunk {
        tag: tag::CHAT_SUBJECT,
        len: req.subject.len() as u16,
        data: if req.subject.is_empty() {
            b"".as_ptr()
        } else {
            req.subject.as_ptr()
        },
    };
    2
}

// ---- HTLC_HDR_AGREEMENTAGREE ------------------------------------------
//
// Phase R2 port of agreement_packet.c::hx_agreement_agree_build_chunks.
// Same wire shape (ICON + NAME + OPTIONS, all three mandatory), same
// chunks-array + scratch contract. The C function stays as a thin
// shim so the existing call sites (network.c::hx_send_agreement_agree
// and the integration harness) keep working.

/// Request data for [`build_agreement_agree_chunks`]. Mirrors the
/// C `hx_agreement_agree_request` struct in `agreement_packet.h`.
pub struct AgreementAgreeRequest<'a> {
    /// HTLC_DATA_ICON value. Always emitted.
    pub icon: u16,
    /// HTLC_DATA_NAME body, already encoded to the negotiated wire
    /// encoding. Empty is legal (zero-length NAME chunk).
    pub display_name: &'a [u8],
    /// HTLC_DATA_OPTIONS bitmap. Mandatory — Mobius panics without it,
    /// see the long comment in `src/network.c::hx_send_agreement_agree`.
    pub options: u16,
}

/// Build the chunk array for `HTLC_HDR_AGREEMENTAGREE`. Always 3
/// chunks (ICON + NAME + OPTIONS). Requires `chunks_cap >= 3`,
/// `scratch_cap >= 4` (icon at offset 0, options at offset 2). Also
/// rejects `display_name.len() > u16::MAX` (chunk lengths are 16-bit
/// on the wire — without this check a 65 KiB nick would silently
/// wrap and emit an invalid chunk).
pub fn build_agreement_agree_chunks(
    req: &AgreementAgreeRequest<'_>,
    chunks: &mut [HxChunk],
    scratch: &mut [u8],
) -> usize {
    if chunks.len() < 3 || scratch.len() < 4 || req.display_name.len() > u16::MAX as usize {
        return 0;
    }
    scratch[0..2].copy_from_slice(&req.icon.to_be_bytes());
    scratch[2..4].copy_from_slice(&req.options.to_be_bytes());

    chunks[0] = HxChunk {
        tag: tag::ICON,
        len: 2,
        data: scratch.as_ptr(),
    };
    chunks[1] = HxChunk {
        tag: tag::NAME,
        len: req.display_name.len() as u16,
        data: if req.display_name.is_empty() {
            b"".as_ptr()
        } else {
            req.display_name.as_ptr()
        },
    };
    chunks[2] = HxChunk {
        tag: tag::OPTIONS,
        len: 2,
        // Options lives at scratch[2..4]; subslice + as_ptr is safe
        // and bounds-checked, scratch.len() >= 4 already verified.
        data: scratch[2..4].as_ptr(),
    };
    3
}

// ---- User-management send opcodes ------------------------------------
//
// HTLC_HDR_USER_CHANGE: client pushes its current ICON / NAME (and
// optionally the Colored-Nicknames COLOR) — the server broadcasts it
// back as HTLS_HDR_USER_CHANGE to everyone in the chat.
// HTLC_HDR_USER_KICK: kick-with-optional-ban (the BAN flag chunk shares
// 0x0071 with OPTIONS, see the `tag::BAN` doc).
// HTLC_HDR_USER_GETINFO: ask the server for an arbitrary user's info
// (the reply is a TASK whose body carries the user-info text).

/// Request data for [`build_user_change_chunks`]. Mirrors the C
/// `hx_change_name_icon` call site in `src/users.c`.
pub struct UserChangeRequest<'a> {
    /// HTLC_DATA_ICON value. Always emitted.
    pub icon: u16,
    /// HTLC_DATA_NAME body, already encoded to the negotiated wire
    /// encoding (UTF-8 or Mac Roman per CAP_TEXT_ENCODING). Empty is
    /// legal (zero-length NAME chunk).
    pub name: &'a [u8],
    /// Colored-Nicknames extension. When `Some(c)`, emit
    /// HTLC_DATA_COLOR (BE u32 0x00RRGGBB); when `None`, omit the
    /// chunk entirely. The C side passes `None` for
    /// `HX_NICK_COLOR_NONE` (the spec's auto-opt-in fires on first
    /// DATA_COLOR receipt regardless of value, and a "no color"
    /// client shouldn't opt in — see the long comment in
    /// hx_change_name_icon for the rationale).
    pub nick_color: Option<u32>,
}

/// Build the chunk array for `HTLC_HDR_USER_CHANGE`. Wire shape:
///
/// - `HTLC_DATA_ICON` (`tag::ICON`, 0x0068) — u16 BE.
/// - `HTLC_DATA_NAME` (`tag::NAME`, 0x0066) — body bytes.
/// - `HTLC_DATA_COLOR` (`tag::COLOR`, 0x0500) — u32 BE — **only when
///   `nick_color.is_some()`**. Servers that don't know the
///   Colored-Nicknames extension ignore this trailing chunk; supporting
///   servers mark the session color-aware on first DATA_COLOR receipt.
///
/// Returns 2 (no color) or 3 (with color) on success, or 0 on
/// validation failure: too-small chunks / scratch buffers, or
/// `name.len() > u16::MAX` (16-bit chunk-length boundary).
///
/// Scratch layout: icon at +0 (2 bytes), color at +2 (4 bytes). The
/// chunks' data pointers reference into `scratch` and into `req.name`,
/// so both must outlive the eventual `hlwrite_chunks` call.
pub fn build_user_change_chunks(
    req: &UserChangeRequest<'_>,
    chunks: &mut [HxChunk],
    scratch: &mut [u8],
) -> usize {
    if chunks.len() < 3 || scratch.len() < 6 || req.name.len() > u16::MAX as usize {
        return 0;
    }

    scratch[0..2].copy_from_slice(&req.icon.to_be_bytes());

    chunks[0] = HxChunk {
        tag: tag::ICON,
        len: 2,
        data: scratch.as_ptr(),
    };
    chunks[1] = HxChunk {
        tag: tag::NAME,
        len: req.name.len() as u16,
        data: if req.name.is_empty() {
            b"".as_ptr()
        } else {
            req.name.as_ptr()
        },
    };
    if let Some(c) = req.nick_color {
        scratch[2..6].copy_from_slice(&c.to_be_bytes());
        chunks[2] = HxChunk {
            tag: tag::COLOR,
            len: 4,
            data: scratch[2..6].as_ptr(),
        };
        3
    } else {
        2
    }
}

/// Request data for [`build_user_kick_chunks`]. Mirrors the C
/// `hx_kick_user` call site: the BAN flag is emitted only when
/// `ban != 0`, and (matching the wire ordering used in production)
/// BAN comes BEFORE UID.
pub struct UserKickRequest {
    pub uid: u16,
    /// Non-zero means "also ban" (the BAN chunk is emitted with this
    /// value); zero suppresses the BAN chunk entirely.
    pub ban: u16,
}

/// Build the chunk array for `HTLC_HDR_USER_KICK`. Wire shape:
///
/// - `HTLC_DATA_BAN` (`tag::BAN`, 0x0071) — u16 BE — only when
///   `ban != 0`. Emitted FIRST when present, matching the C call site.
/// - `HTLC_DATA_UID` (`tag::UID`, 0x0067) — u16 BE.
///
/// Returns 1 (no ban) or 2 (with ban) on success, or 0 on a too-small
/// chunks / scratch buffer. Scratch layout: ban at +0 (2 bytes), uid
/// at +2 (2 bytes).
pub fn build_user_kick_chunks(
    req: &UserKickRequest,
    chunks: &mut [HxChunk],
    scratch: &mut [u8],
) -> usize {
    if chunks.len() < 2 || scratch.len() < 4 {
        return 0;
    }

    let with_ban = req.ban != 0;
    let uid_be = req.uid.to_be_bytes();

    if with_ban {
        let ban_be = req.ban.to_be_bytes();
        scratch[0..2].copy_from_slice(&ban_be);
        scratch[2..4].copy_from_slice(&uid_be);
        chunks[0] = HxChunk {
            tag: tag::BAN,
            len: 2,
            data: scratch.as_ptr(),
        };
        chunks[1] = HxChunk {
            tag: tag::UID,
            len: 2,
            data: scratch[2..4].as_ptr(),
        };
        2
    } else {
        scratch[0..2].copy_from_slice(&uid_be);
        chunks[0] = HxChunk {
            tag: tag::UID,
            len: 2,
            data: scratch.as_ptr(),
        };
        1
    }
}

/// Build the chunk array for `HTLC_HDR_USER_GETINFO`. Single
/// `HTLC_DATA_UID` chunk (u16 BE). Returns 1 on success, 0 on a
/// too-small chunks / scratch buffer.
pub fn build_user_getinfo_chunks(
    uid: u16,
    chunks: &mut [HxChunk],
    scratch: &mut [u8],
) -> usize {
    if chunks.is_empty() || scratch.len() < 2 {
        return 0;
    }
    scratch[0..2].copy_from_slice(&uid.to_be_bytes());
    chunks[0] = HxChunk {
        tag: tag::UID,
        len: 2,
        data: scratch.as_ptr(),
    };
    1
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Read a chunk's payload bytes through its raw pointer (safe in
    /// tests because we know the buffers are still alive).
    unsafe fn chunk_bytes(c: &HxChunk) -> &[u8] {
        std::slice::from_raw_parts(c.data, c.len as usize)
    }

    #[test]
    fn chat_no_cid_emits_two_chunks() {
        let req = ChatRequest {
            cid: 0,
            style: 0x0102,
            body: b"hello",
        };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 8];
        let hc = build_chat_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(hc, 2);
        assert_eq!(chunks[0].tag, tag::STYLE);
        assert_eq!(chunks[0].len, 2);
        assert_eq!(unsafe { chunk_bytes(&chunks[0]) }, &[0x01, 0x02]);
        assert_eq!(chunks[1].tag, tag::BODY);
        assert_eq!(chunks[1].len, 5);
        assert_eq!(unsafe { chunk_bytes(&chunks[1]) }, b"hello");
    }

    #[test]
    fn chat_with_cid_emits_three_chunks_in_order() {
        let req = ChatRequest {
            cid: 0xdead_beef,
            style: 0,
            body: b"hi",
        };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 8];
        let hc = build_chat_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(hc, 3);
        assert_eq!(chunks[0].tag, tag::STYLE);
        assert_eq!(chunks[1].tag, tag::BODY);
        assert_eq!(chunks[2].tag, tag::CHAT_ID);
        assert_eq!(chunks[2].len, 4);
        assert_eq!(unsafe { chunk_bytes(&chunks[2]) }, &[0xde, 0xad, 0xbe, 0xef]);
    }

    #[test]
    fn chat_empty_body_is_legal() {
        let req = ChatRequest { cid: 0, style: 0, body: b"" };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 8];
        let hc = build_chat_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(hc, 2);
        assert_eq!(chunks[1].len, 0);
        // data pointer is non-NULL so the C side won't trip a guard.
        assert!(!chunks[1].data.is_null());
    }

    #[test]
    fn chat_rejects_short_chunks_buffer() {
        let req = ChatRequest { cid: 1, style: 0, body: b"" };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY]; // 2 < 3
        let mut scratch = [0u8; 8];
        assert_eq!(build_chat_chunks(&req, &mut chunks, &mut scratch), 0);
    }

    #[test]
    fn chat_rejects_short_scratch_buffer() {
        let req = ChatRequest { cid: 1, style: 0, body: b"" };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 4]; // 4 < 6
        assert_eq!(build_chat_chunks(&req, &mut chunks, &mut scratch), 0);
    }

    #[test]
    fn msg_emits_uid_then_body() {
        let req = MsgRequest { uid: 0x1234, body: b"hello" };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 4];
        let hc = build_msg_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(hc, 2);
        assert_eq!(chunks[0].tag, tag::UID);
        assert_eq!(chunks[0].len, 2);
        assert_eq!(unsafe { chunk_bytes(&chunks[0]) }, &[0x12, 0x34]);
        assert_eq!(chunks[1].tag, tag::BODY);
        assert_eq!(unsafe { chunk_bytes(&chunks[1]) }, b"hello");
    }

    #[test]
    fn msg_rejects_short_buffers() {
        let req = MsgRequest { uid: 1, body: b"" };
        let mut chunks_short = [HxChunk::EMPTY]; // 1 < 2
        let mut scratch = [0u8; 4];
        assert_eq!(build_msg_chunks(&req, &mut chunks_short, &mut scratch), 0);

        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch_short = [0u8; 1]; // 1 < 2
        assert_eq!(build_msg_chunks(&req, &mut chunks, &mut scratch_short), 0);
    }

    #[test]
    fn broadcast_emits_just_body() {
        let req = BroadcastRequest { body: b"server going down" };
        let mut chunks = [HxChunk::EMPTY];
        let hc = build_broadcast_chunks(&req, &mut chunks);
        assert_eq!(hc, 1);
        assert_eq!(chunks[0].tag, tag::BODY);
        assert_eq!(chunks[0].len, 17);
        assert_eq!(unsafe { chunk_bytes(&chunks[0]) }, b"server going down");
    }

    #[test]
    fn broadcast_rejects_empty_chunks_slice() {
        let req = BroadcastRequest { body: b"" };
        let mut chunks: [HxChunk; 0] = [];
        assert_eq!(build_broadcast_chunks(&req, &mut chunks), 0);
    }

    // ---- oversize-body rejects ----
    //
    // Chunk lengths on the Hotline wire are 16-bit. Each builder
    // truncates body.len() to u16 when populating the chunk; without
    // the explicit length check, a 65 KiB body would silently wrap to
    // a small chunk-length and put an invalid frame on the wire. Cover
    // the boundary (u16::MAX + 1) for each builder. Allocating a
    // 65537-byte vec is cheap in test.

    #[test]
    fn chat_rejects_body_larger_than_u16_max() {
        let big = vec![0u8; u16::MAX as usize + 1];
        let req = ChatRequest { cid: 0, style: 0, body: &big };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 8];
        assert_eq!(build_chat_chunks(&req, &mut chunks, &mut scratch), 0);
    }

    #[test]
    fn chat_accepts_body_exactly_u16_max() {
        let big = vec![0u8; u16::MAX as usize];
        let req = ChatRequest { cid: 0, style: 0, body: &big };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 8];
        let hc = build_chat_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(hc, 2);
        assert_eq!(chunks[1].len, u16::MAX);
    }

    #[test]
    fn msg_rejects_body_larger_than_u16_max() {
        let big = vec![0u8; u16::MAX as usize + 1];
        let req = MsgRequest { uid: 0, body: &big };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 2];
        assert_eq!(build_msg_chunks(&req, &mut chunks, &mut scratch), 0);
    }

    #[test]
    fn broadcast_rejects_body_larger_than_u16_max() {
        let big = vec![0u8; u16::MAX as usize + 1];
        let req = BroadcastRequest { body: &big };
        let mut chunks = [HxChunk::EMPTY];
        assert_eq!(build_broadcast_chunks(&req, &mut chunks), 0);
    }

    // ---- chat admin ----

    #[test]
    fn chat_create_emits_uid_chunk() {
        let mut chunks = [HxChunk::EMPTY];
        let mut scratch = [0u8; 4];
        let hc = build_chat_create_chunks(0x1234, &mut chunks, &mut scratch);
        assert_eq!(hc, 1);
        assert_eq!(chunks[0].tag, tag::UID);
        assert_eq!(chunks[0].len, 2);
        assert_eq!(unsafe { chunk_bytes(&chunks[0]) }, &[0x12, 0x34]);
    }

    #[test]
    fn chat_create_rejects_short_buffers() {
        let mut chunks_short: [HxChunk; 0] = [];
        let mut scratch = [0u8; 4];
        assert_eq!(
            build_chat_create_chunks(1, &mut chunks_short, &mut scratch),
            0
        );

        let mut chunks = [HxChunk::EMPTY];
        let mut scratch_short = [0u8; 1];
        assert_eq!(
            build_chat_create_chunks(1, &mut chunks, &mut scratch_short),
            0
        );
    }

    #[test]
    fn chat_invite_emits_cid_then_uid() {
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 8];
        let hc = build_chat_invite_chunks(0xdead_beef, 0x42, &mut chunks, &mut scratch);
        assert_eq!(hc, 2);
        assert_eq!(chunks[0].tag, tag::CHAT_ID);
        assert_eq!(chunks[0].len, 4);
        assert_eq!(unsafe { chunk_bytes(&chunks[0]) }, &[0xde, 0xad, 0xbe, 0xef]);
        assert_eq!(chunks[1].tag, tag::UID);
        assert_eq!(chunks[1].len, 2);
        assert_eq!(unsafe { chunk_bytes(&chunks[1]) }, &[0x00, 0x42]);
    }

    #[test]
    fn chat_invite_rejects_short_buffers() {
        let mut chunks_short = [HxChunk::EMPTY];
        let mut scratch = [0u8; 8];
        assert_eq!(
            build_chat_invite_chunks(1, 1, &mut chunks_short, &mut scratch),
            0
        );

        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch_short = [0u8; 5];
        assert_eq!(
            build_chat_invite_chunks(1, 1, &mut chunks, &mut scratch_short),
            0
        );
    }

    #[test]
    fn chat_id_only_builders_share_shape() {
        // join, part, decline all wrap the same helper — verify each
        // emits a single CHAT_ID chunk with the BE-encoded cid.
        for &builder in &[
            build_chat_join_chunks
                as fn(u32, &mut [HxChunk], &mut [u8]) -> usize,
            build_chat_part_chunks
                as fn(u32, &mut [HxChunk], &mut [u8]) -> usize,
            build_chat_decline_chunks
                as fn(u32, &mut [HxChunk], &mut [u8]) -> usize,
        ] {
            let mut chunks = [HxChunk::EMPTY];
            let mut scratch = [0u8; 4];
            let hc = builder(0xcafe_babe, &mut chunks, &mut scratch);
            assert_eq!(hc, 1);
            assert_eq!(chunks[0].tag, tag::CHAT_ID);
            assert_eq!(chunks[0].len, 4);
            assert_eq!(
                unsafe { chunk_bytes(&chunks[0]) },
                &[0xca, 0xfe, 0xba, 0xbe]
            );
        }
    }

    #[test]
    fn chat_id_only_rejects_short_buffers() {
        let mut chunks_short: [HxChunk; 0] = [];
        let mut scratch = [0u8; 4];
        assert_eq!(
            build_chat_join_chunks(1, &mut chunks_short, &mut scratch),
            0
        );

        let mut chunks = [HxChunk::EMPTY];
        let mut scratch_short = [0u8; 2];
        assert_eq!(
            build_chat_part_chunks(1, &mut chunks, &mut scratch_short),
            0
        );
    }

    #[test]
    fn chat_subject_emits_cid_then_subject() {
        let req = ChatSubjectRequest {
            cid: 0x0000_0007,
            subject: b"Welcome",
        };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 4];
        let hc = build_chat_subject_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(hc, 2);
        assert_eq!(chunks[0].tag, tag::CHAT_ID);
        assert_eq!(unsafe { chunk_bytes(&chunks[0]) }, &[0, 0, 0, 7]);
        assert_eq!(chunks[1].tag, tag::CHAT_SUBJECT);
        assert_eq!(chunks[1].len, 7);
        assert_eq!(unsafe { chunk_bytes(&chunks[1]) }, b"Welcome");
    }

    #[test]
    fn chat_subject_empty_subject_legal() {
        let req = ChatSubjectRequest { cid: 1, subject: b"" };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 4];
        let hc = build_chat_subject_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(hc, 2);
        assert_eq!(chunks[1].len, 0);
        assert!(!chunks[1].data.is_null());
    }

    #[test]
    fn chat_subject_rejects_short_buffers() {
        let req = ChatSubjectRequest { cid: 1, subject: b"" };
        let mut chunks_short = [HxChunk::EMPTY];
        let mut scratch = [0u8; 4];
        assert_eq!(
            build_chat_subject_chunks(&req, &mut chunks_short, &mut scratch),
            0
        );

        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch_short = [0u8; 3];
        assert_eq!(
            build_chat_subject_chunks(&req, &mut chunks, &mut scratch_short),
            0
        );
    }

    // ---- agreement agree ----

    #[test]
    fn agreement_agree_emits_three_chunks_in_order() {
        let req = AgreementAgreeRequest {
            icon: 0x01f4,
            display_name: b"misha",
            options: 0,
        };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 4];
        let hc = build_agreement_agree_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(hc, 3);
        assert_eq!(chunks[0].tag, tag::ICON);
        assert_eq!(chunks[0].len, 2);
        assert_eq!(unsafe { chunk_bytes(&chunks[0]) }, &[0x01, 0xf4]);
        assert_eq!(chunks[1].tag, tag::NAME);
        assert_eq!(chunks[1].len, 5);
        assert_eq!(unsafe { chunk_bytes(&chunks[1]) }, b"misha");
        assert_eq!(chunks[2].tag, tag::OPTIONS);
        assert_eq!(chunks[2].len, 2);
        assert_eq!(unsafe { chunk_bytes(&chunks[2]) }, &[0x00, 0x00]);
    }

    #[test]
    fn agreement_agree_empty_name_legal() {
        let req = AgreementAgreeRequest {
            icon: 0,
            display_name: b"",
            options: 0,
        };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 4];
        let hc = build_agreement_agree_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(hc, 3);
        assert_eq!(chunks[1].len, 0);
        // OPTIONS chunk is always emitted (Mobius-panic invariant).
        assert_eq!(chunks[2].tag, tag::OPTIONS);
    }

    #[test]
    fn agreement_agree_rejects_short_buffers() {
        let req = AgreementAgreeRequest {
            icon: 0,
            display_name: b"",
            options: 0,
        };
        let mut chunks_short = [HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 4];
        assert_eq!(
            build_agreement_agree_chunks(&req, &mut chunks_short, &mut scratch),
            0
        );

        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch_short = [0u8; 3];
        assert_eq!(
            build_agreement_agree_chunks(&req, &mut chunks, &mut scratch_short),
            0
        );
    }

    // ---- oversize-body rejects (chat_subject + agreement_agree) ----
    //
    // Same 16-bit-chunk-length boundary checked above for the
    // chat / msg / broadcast builders, applied to the
    // variable-length payloads on the chat-admin builders.

    #[test]
    fn chat_subject_rejects_subject_larger_than_u16_max() {
        let big = vec![0u8; u16::MAX as usize + 1];
        let req = ChatSubjectRequest { cid: 1, subject: &big };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 4];
        assert_eq!(
            build_chat_subject_chunks(&req, &mut chunks, &mut scratch),
            0
        );
    }

    #[test]
    fn chat_subject_accepts_subject_exactly_u16_max() {
        let big = vec![b's'; u16::MAX as usize];
        let req = ChatSubjectRequest { cid: 1, subject: &big };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 4];
        let hc = build_chat_subject_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(hc, 2);
        assert_eq!(chunks[1].len, u16::MAX);
    }

    #[test]
    fn agreement_agree_rejects_name_larger_than_u16_max() {
        let big = vec![b'q'; u16::MAX as usize + 1];
        let req = AgreementAgreeRequest {
            icon: 0,
            display_name: &big,
            options: 0,
        };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 4];
        assert_eq!(
            build_agreement_agree_chunks(&req, &mut chunks, &mut scratch),
            0
        );
    }

    // ---- user change ----

    #[test]
    fn user_change_no_color_emits_two_chunks() {
        let req = UserChangeRequest {
            icon: 0x01f4,
            name: b"misha",
            nick_color: None,
        };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 8];
        let hc = build_user_change_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(hc, 2);
        assert_eq!(chunks[0].tag, tag::ICON);
        assert_eq!(chunks[0].len, 2);
        assert_eq!(unsafe { chunk_bytes(&chunks[0]) }, &[0x01, 0xf4]);
        assert_eq!(chunks[1].tag, tag::NAME);
        assert_eq!(chunks[1].len, 5);
        assert_eq!(unsafe { chunk_bytes(&chunks[1]) }, b"misha");
    }

    #[test]
    fn user_change_with_color_emits_three_chunks_in_order() {
        let req = UserChangeRequest {
            icon: 0x0005,
            name: b"alice",
            nick_color: Some(0x00ff_8800),
        };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 8];
        let hc = build_user_change_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(hc, 3);
        assert_eq!(chunks[0].tag, tag::ICON);
        assert_eq!(chunks[1].tag, tag::NAME);
        assert_eq!(chunks[2].tag, tag::COLOR);
        assert_eq!(chunks[2].len, 4);
        assert_eq!(unsafe { chunk_bytes(&chunks[2]) }, &[0x00, 0xff, 0x88, 0x00]);
    }

    #[test]
    fn user_change_empty_name_legal() {
        let req = UserChangeRequest {
            icon: 0,
            name: b"",
            nick_color: None,
        };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 8];
        let hc = build_user_change_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(hc, 2);
        assert_eq!(chunks[1].len, 0);
        assert!(!chunks[1].data.is_null());
    }

    #[test]
    fn user_change_rejects_short_buffers() {
        let req = UserChangeRequest {
            icon: 0,
            name: b"",
            nick_color: Some(0),
        };
        let mut chunks_short = [HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 8];
        assert_eq!(
            build_user_change_chunks(&req, &mut chunks_short, &mut scratch),
            0
        );

        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch_short = [0u8; 5];
        assert_eq!(
            build_user_change_chunks(&req, &mut chunks, &mut scratch_short),
            0
        );
    }

    #[test]
    fn user_change_rejects_name_larger_than_u16_max() {
        let big = vec![b'q'; u16::MAX as usize + 1];
        let req = UserChangeRequest {
            icon: 0,
            name: &big,
            nick_color: None,
        };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 8];
        assert_eq!(
            build_user_change_chunks(&req, &mut chunks, &mut scratch),
            0
        );
    }

    // ---- user kick ----

    #[test]
    fn user_kick_no_ban_emits_just_uid() {
        let req = UserKickRequest { uid: 0x1234, ban: 0 };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 4];
        let hc = build_user_kick_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(hc, 1);
        assert_eq!(chunks[0].tag, tag::UID);
        assert_eq!(chunks[0].len, 2);
        assert_eq!(unsafe { chunk_bytes(&chunks[0]) }, &[0x12, 0x34]);
    }

    #[test]
    fn user_kick_with_ban_emits_ban_then_uid() {
        // BAN comes BEFORE UID — matches the C call-site ordering, which
        // mhxd cares about (the kick handler reads chunks in order).
        let req = UserKickRequest { uid: 0x1234, ban: 1 };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 4];
        let hc = build_user_kick_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(hc, 2);
        assert_eq!(chunks[0].tag, tag::BAN);
        assert_eq!(chunks[0].len, 2);
        assert_eq!(unsafe { chunk_bytes(&chunks[0]) }, &[0x00, 0x01]);
        assert_eq!(chunks[1].tag, tag::UID);
        assert_eq!(chunks[1].len, 2);
        assert_eq!(unsafe { chunk_bytes(&chunks[1]) }, &[0x12, 0x34]);
    }

    #[test]
    fn user_kick_rejects_short_buffers() {
        let req = UserKickRequest { uid: 1, ban: 1 };
        let mut chunks_short = [HxChunk::EMPTY];
        let mut scratch = [0u8; 4];
        assert_eq!(
            build_user_kick_chunks(&req, &mut chunks_short, &mut scratch),
            0
        );

        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch_short = [0u8; 3];
        assert_eq!(
            build_user_kick_chunks(&req, &mut chunks, &mut scratch_short),
            0
        );
    }

    // ---- user get info ----

    #[test]
    fn user_getinfo_emits_uid_chunk() {
        let mut chunks = [HxChunk::EMPTY];
        let mut scratch = [0u8; 4];
        let hc = build_user_getinfo_chunks(0xabcd, &mut chunks, &mut scratch);
        assert_eq!(hc, 1);
        assert_eq!(chunks[0].tag, tag::UID);
        assert_eq!(chunks[0].len, 2);
        assert_eq!(unsafe { chunk_bytes(&chunks[0]) }, &[0xab, 0xcd]);
    }

    #[test]
    fn user_getinfo_rejects_short_buffers() {
        let mut chunks_short: [HxChunk; 0] = [];
        let mut scratch = [0u8; 4];
        assert_eq!(
            build_user_getinfo_chunks(1, &mut chunks_short, &mut scratch),
            0
        );

        let mut chunks = [HxChunk::EMPTY];
        let mut scratch_short = [0u8; 1];
        assert_eq!(
            build_user_getinfo_chunks(1, &mut chunks, &mut scratch_short),
            0
        );
    }
}
