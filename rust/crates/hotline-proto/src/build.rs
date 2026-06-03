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
}
