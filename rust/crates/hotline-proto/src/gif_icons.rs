//! GIF-icons extension wire protocol (fogWraith `GIF-Icons.md`).
//!
//! Per-user custom GIF avatars, independent of the standard 16-bit
//! icon ID. Four transactions ride the control channel:
//!
//! - `ICON_GETLIST` (1861 / 0x0745), client → server. No request
//!   fields. Reply carries 0..N [`tag::ICON_LIST`] (0x0301) chunks,
//!   one packed entry per user.
//! - `ICON_SET` (1862 / 0x0746), client → server. One
//!   [`tag::ICON_GIF`] (0x0300) chunk with the raw GIF; an empty
//!   chunk clears the avatar. Reply is a bare task completion.
//! - `ICON_GET` (1863 / 0x0747), client → server. Request carries
//!   [`tag::UID`] (0x0067); reply carries `UID` + `ICON_GIF`.
//! - `ICON_CHANGE` (1864 / 0x0748), server → clients. Broadcast
//!   carrying only `UID`; recipients re-fetch with `ICON_GET`.
//!
//! Parsing lives entirely here — whole-message walkers over
//! [`ChunkIter::over_message`], same discipline as
//! [`crate::inline_media`]. The C rcv handlers hand `htlc->in.buf` /
//! `htlc->in.pos` straight in and act only on the typed result; they
//! do no chunk walking of their own. Builders return [`HxChunk`]
//! arrays borrowing caller scratch, consumed immediately by
//! `hlwrite_chunks` on the C side.
//!
//! ## Packed Icon List Entry (0x0301)
//!
//! | Offset | Size | Field        |
//! |--------|------|--------------|
//! | 0      | 2    | uid (u16 BE) |
//! | 2      | 2    | gif_len (u16 BE) |
//! | 4      | N    | gif bytes    |
//!
//! The 2-byte length caps a single entry's GIF at 64 KiB on the
//! wire, well above the spec's 32 KiB upload recommendation.

use crate::build::HxChunk;
use crate::messages::tag;
use crate::wire::Chunk;

// ---- GIF signature --------------------------------------------------------

/// True if `data` begins with a valid GIF signature (`GIF87a` or
/// `GIF89a`). Servers reject non-GIF uploads per spec; we mirror the
/// check client-side before `ICON_SET` and before decoding a fetched
/// avatar. An empty slice (a clear) is not a GIF and returns false —
/// callers special-case the clear path.
pub fn is_gif(data: &[u8]) -> bool {
    matches!(data.get(0..6), Some(b"GIF87a") | Some(b"GIF89a"))
}

// ---- Parsed avatar entry --------------------------------------------------

/// One user's avatar, borrowed from the source buffer. Produced by
/// [`parse_icon_get_reply`] and by unpacking each [`tag::ICON_LIST`]
/// entry in [`parse_icon_list`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct IconEntry<'a> {
    pub uid: u16,
    /// Raw GIF bytes. May be empty when a user has no avatar and the
    /// server lists them anyway with a zero-length payload.
    pub gif: &'a [u8],
}

fn u16_at(d: &[u8], off: usize) -> Option<u16> {
    Some(u16::from_be_bytes([*d.get(off)?, *d.get(off + 1)?]))
}

// ---- ICON_GETLIST reply ---------------------------------------------------

/// Unpack one packed `ICON_LIST` (0x0301) entry body:
/// `u16 uid` + `u16 gif_len` + `gif` (all big-endian).
///
/// Returns `None` on truncation (< 4 bytes) or a declared `gif_len`
/// that runs past the buffer — a malformed entry the caller should
/// skip rather than half-read.
pub fn parse_icon_list_entry(data: &[u8]) -> Option<IconEntry<'_>> {
    let uid = u16_at(data, 0)?;
    let len = u16_at(data, 2)? as usize;
    let gif = data.get(4..4 + len)?;
    Some(IconEntry { uid, gif })
}

/// Walk an `ICON_GETLIST` reply and yield each user's avatar entry.
/// Each 0x0301 chunk holds one packed entry; malformed entries are
/// silently skipped, so the yielded count may be fewer than the number
/// of 0x0301 chunks present.
pub fn parse_icon_list<'a>(
    chunks: impl Iterator<Item = Chunk<'a>>,
) -> impl Iterator<Item = IconEntry<'a>> {
    chunks
        .filter(|c| c.tag == tag::ICON_LIST)
        .filter_map(|c| parse_icon_list_entry(c.data))
}

// ---- ICON_GET reply -------------------------------------------------------

/// Parse an `ICON_GET` reply: `UID` (0x0067) plus an optional
/// `ICON_GIF` (0x0300).
///
/// Only `UID` is required. A **missing** `ICON_GIF` is parsed as a
/// valid *cleared* reply — `gif` is an empty slice — because servers
/// differ in how they report "this user has no avatar": Janus omits
/// the field entirely, while a server that stored a zero-length set
/// echoes an empty field. Both mean the same thing, and the receive
/// path needs to clear a stale cached avatar either way. A genuinely
/// absent `UID` is the only failure (returns `None`). Validating the
/// GIF signature on a non-empty payload is the caller's job
/// ([`is_gif`]).
pub fn parse_icon_get_reply<'a>(
    chunks: impl Iterator<Item = Chunk<'a>>,
) -> Option<IconEntry<'a>> {
    let mut uid: Option<u16> = None;
    let mut gif: &[u8] = &[];
    for c in chunks {
        match c.tag {
            tag::UID if c.data.len() == 2 => {
                uid = u16_at(c.data, 0);
            }
            tag::ICON_GIF => gif = c.data,
            _ => {}
        }
    }
    Some(IconEntry { uid: uid?, gif })
}

// ---- ICON_CHANGE broadcast ------------------------------------------------

/// Parse an `ICON_CHANGE` broadcast: only `UID` (0x0067). Returns
/// `None` if absent or not exactly 2 bytes wide.
pub fn parse_icon_change<'a>(chunks: impl Iterator<Item = Chunk<'a>>) -> Option<u16> {
    for c in chunks {
        if c.tag == tag::UID && c.data.len() == 2 {
            return u16_at(c.data, 0);
        }
    }
    None
}

// ---- Builders -------------------------------------------------------------

/// Build the single `ICON_SET` (1862) chunk: an `ICON_GIF` field
/// carrying `gif`. An empty `gif` is a valid *clear* request per
/// spec, so it still emits a zero-length field.
///
/// Returns 1 on success, or 0 if `chunks` is empty or `gif` exceeds
/// the `u16` wire-length limit. Does **not** validate the GIF
/// signature — the caller checks [`is_gif`] for the non-clear path.
pub fn build_icon_set_chunks(gif: &[u8], chunks: &mut [HxChunk]) -> usize {
    if chunks.is_empty() || gif.len() > u16::MAX as usize {
        return 0;
    }
    chunks[0] = HxChunk {
        tag: tag::ICON_GIF,
        len: gif.len() as u16,
        // An empty slice's as_ptr() is a non-NULL dangling pointer;
        // hand a real NULL across the FFI for the clear case so no
        // current-or-future consumer can deref it on len == 0.
        data: if gif.is_empty() {
            std::ptr::null()
        } else {
            gif.as_ptr()
        },
    };
    1
}

/// Build the single `ICON_GET` (1863) chunk: a `UID` field. `scratch`
/// holds the big-endian uid for the duration of the chunk's life.
///
/// Returns 1 on success, or 0 if buffers are undersized.
pub fn build_icon_get_chunks(uid: u16, chunks: &mut [HxChunk], scratch: &mut [u8]) -> usize {
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

// `ICON_GETLIST` (1861) has no request fields — the C sender calls
// `hlwrite_chunks(..., 0)` directly, so there is no builder here.

// ---- Tests ----------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use crate::wire::{ChunkIter, Encoder};
    use crate::HL_HDR_LEN;

    fn header_padded(extra: &[u8]) -> Vec<u8> {
        let mut buf = vec![0u8; HL_HDR_LEN];
        buf.extend_from_slice(extra);
        buf
    }

    /// A minimal valid 1x1 GIF89a, the same blob the Janus probe used.
    const TINY_GIF: &[u8] = &[
        0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x01, 0x00, 0x01, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00,
        0x00, 0xff, 0xff, 0xff, 0x21, 0xf9, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00,
        0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x02, 0x02, 0x44, 0x01, 0x00, 0x3b,
    ];

    fn packed_entry(uid: u16, gif: &[u8]) -> Vec<u8> {
        let mut v = Vec::new();
        v.extend_from_slice(&uid.to_be_bytes());
        v.extend_from_slice(&(gif.len() as u16).to_be_bytes());
        v.extend_from_slice(gif);
        v
    }

    // ---- is_gif -----------------------------------------------------------

    #[test]
    fn is_gif_accepts_both_signatures() {
        assert!(is_gif(b"GIF87a\x00\x00"));
        assert!(is_gif(b"GIF89a and more"));
        assert!(is_gif(TINY_GIF));
    }

    #[test]
    fn is_gif_rejects_non_gif_and_short() {
        assert!(!is_gif(b"\x89PNG\r\n\x1a\n"));
        assert!(!is_gif(b"GIF8")); // too short
        assert!(!is_gif(b"")); // clear, not a GIF
        assert!(!is_gif(b"GIF99a")); // wrong version digits
    }

    // ---- packed entry -----------------------------------------------------

    #[test]
    fn icon_list_entry_roundtrips() {
        let buf = packed_entry(1707, TINY_GIF);
        let e = parse_icon_list_entry(&buf).expect("valid entry");
        assert_eq!(e.uid, 1707);
        assert_eq!(e.gif, TINY_GIF);
    }

    #[test]
    fn icon_list_entry_rejects_truncated_header() {
        assert!(parse_icon_list_entry(&[0x00, 0x01, 0x00]).is_none());
        assert!(parse_icon_list_entry(&[]).is_none());
    }

    #[test]
    fn icon_list_entry_rejects_len_past_buffer() {
        // declares 100-byte gif but only 2 bytes follow
        let mut v = Vec::new();
        v.extend_from_slice(&7u16.to_be_bytes());
        v.extend_from_slice(&100u16.to_be_bytes());
        v.extend_from_slice(&[0xaa, 0xbb]);
        assert!(parse_icon_list_entry(&v).is_none());
    }

    #[test]
    fn icon_list_entry_zero_length_gif_ok() {
        let buf = packed_entry(42, b"");
        let e = parse_icon_list_entry(&buf).expect("zero-len entry valid");
        assert_eq!(e.uid, 42);
        assert!(e.gif.is_empty());
    }

    // ---- ICON_GETLIST reply walk -----------------------------------------

    #[test]
    fn icon_list_walk_collects_every_entry() {
        let mut e = Encoder::new();
        e.put_chunk(tag::ICON_LIST, &packed_entry(1, b"GIF89a-one"));
        e.put_chunk(tag::ICON_LIST, &packed_entry(2, TINY_GIF));
        let buf = header_padded(e.as_slice());
        let got: Vec<_> = parse_icon_list(ChunkIter::over_message(&buf, buf.len())).collect();
        assert_eq!(got.len(), 2);
        assert_eq!(got[0].uid, 1);
        assert_eq!(got[0].gif, b"GIF89a-one");
        assert_eq!(got[1].uid, 2);
        assert_eq!(got[1].gif, TINY_GIF);
    }

    #[test]
    fn icon_list_walk_skips_malformed_entries() {
        let mut e = Encoder::new();
        e.put_chunk(tag::ICON_LIST, &packed_entry(1, b"good"));
        // malformed: declares more than it carries
        e.put_chunk(tag::ICON_LIST, &[0x00, 0x02, 0xff, 0xff, 0x01]);
        e.put_chunk(tag::ICON_LIST, &packed_entry(3, b"also-good"));
        let buf = header_padded(e.as_slice());
        let got: Vec<_> = parse_icon_list(ChunkIter::over_message(&buf, buf.len())).collect();
        assert_eq!(got.len(), 2);
        assert_eq!(got[0].uid, 1);
        assert_eq!(got[1].uid, 3);
    }

    #[test]
    fn icon_list_walk_ignores_other_tags() {
        let mut e = Encoder::new();
        e.put_chunk(tag::UID, &7u16.to_be_bytes());
        e.put_chunk(tag::ICON_LIST, &packed_entry(9, b"x"));
        let buf = header_padded(e.as_slice());
        let got: Vec<_> = parse_icon_list(ChunkIter::over_message(&buf, buf.len())).collect();
        assert_eq!(got.len(), 1);
        assert_eq!(got[0].uid, 9);
    }

    #[test]
    fn icon_list_walk_empty_reply_is_empty() {
        let buf = header_padded(&[]);
        let got: Vec<_> = parse_icon_list(ChunkIter::over_message(&buf, buf.len())).collect();
        assert!(got.is_empty());
    }

    // ---- ICON_GET reply ---------------------------------------------------

    #[test]
    fn icon_get_reply_parses_uid_and_gif() {
        let mut e = Encoder::new();
        e.put_chunk(tag::UID, &1708u16.to_be_bytes());
        e.put_chunk(tag::ICON_GIF, TINY_GIF);
        let buf = header_padded(e.as_slice());
        let r = parse_icon_get_reply(ChunkIter::over_message(&buf, buf.len())).expect("present");
        assert_eq!(r.uid, 1708);
        assert_eq!(r.gif, TINY_GIF);
    }

    #[test]
    fn icon_get_reply_requires_uid_only() {
        // UID alone (no ICON_GIF) is a valid "cleared" reply — the
        // avatar is gone, gif is empty. Some servers (Janus) report a
        // clear this way.
        let mut only_uid = Encoder::new();
        only_uid.put_chunk(tag::UID, &7u16.to_be_bytes());
        let buf = header_padded(only_uid.as_slice());
        let r = parse_icon_get_reply(ChunkIter::over_message(&buf, buf.len()))
            .expect("uid alone is a valid cleared reply");
        assert_eq!(r.uid, 7);
        assert!(r.gif.is_empty());

        // ICON_GIF without a UID is malformed — we can't attribute the
        // avatar to anyone.
        let mut only_gif = Encoder::new();
        only_gif.put_chunk(tag::ICON_GIF, TINY_GIF);
        let buf = header_padded(only_gif.as_slice());
        assert!(parse_icon_get_reply(ChunkIter::over_message(&buf, buf.len())).is_none());
    }

    // ---- ICON_CHANGE broadcast -------------------------------------------

    #[test]
    fn icon_change_parses_uid() {
        let mut e = Encoder::new();
        e.put_chunk(tag::UID, &1706u16.to_be_bytes());
        let buf = header_padded(e.as_slice());
        assert_eq!(
            parse_icon_change(ChunkIter::over_message(&buf, buf.len())),
            Some(1706)
        );
    }

    #[test]
    fn icon_change_none_when_uid_absent_or_malformed() {
        let buf = header_padded(&[]);
        assert!(parse_icon_change(ChunkIter::over_message(&buf, buf.len())).is_none());

        let mut wrong = Encoder::new();
        wrong.put_chunk(tag::UID, &[0x01]); // 1 byte, not 2
        let buf = header_padded(wrong.as_slice());
        assert!(parse_icon_change(ChunkIter::over_message(&buf, buf.len())).is_none());
    }

    // ---- Builders ---------------------------------------------------------

    #[test]
    fn build_set_emits_gif_field() {
        let mut chunks = [HxChunk::EMPTY; 1];
        let n = build_icon_set_chunks(TINY_GIF, &mut chunks);
        assert_eq!(n, 1);
        assert_eq!(chunks[0].tag, tag::ICON_GIF);
        assert_eq!(chunks[0].len as usize, TINY_GIF.len());
    }

    #[test]
    fn build_set_clear_emits_empty_field() {
        let mut chunks = [HxChunk::EMPTY; 1];
        let n = build_icon_set_chunks(b"", &mut chunks);
        assert_eq!(n, 1);
        assert_eq!(chunks[0].tag, tag::ICON_GIF);
        assert_eq!(chunks[0].len, 0);
    }

    #[test]
    fn build_set_rejects_oversize_and_no_room() {
        let mut chunks = [HxChunk::EMPTY; 1];
        let huge = vec![0u8; u16::MAX as usize + 1];
        assert_eq!(build_icon_set_chunks(&huge, &mut chunks), 0);
        let mut empty: [HxChunk; 0] = [];
        assert_eq!(build_icon_set_chunks(TINY_GIF, &mut empty), 0);
    }

    #[test]
    fn build_get_emits_uid_be() {
        let mut chunks = [HxChunk::EMPTY; 1];
        let mut scratch = [0u8; 2];
        let n = build_icon_get_chunks(0x1234, &mut chunks, &mut scratch);
        assert_eq!(n, 1);
        assert_eq!(chunks[0].tag, tag::UID);
        assert_eq!(chunks[0].len, 2);
        assert_eq!(&scratch, &[0x12, 0x34]);
    }

    #[test]
    fn build_get_rejects_undersized_buffers() {
        let mut chunks = [HxChunk::EMPTY; 1];
        let mut tiny = [0u8; 1];
        assert_eq!(build_icon_get_chunks(1, &mut chunks, &mut tiny), 0);
    }
}
