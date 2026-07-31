//! Inline-media extension wire protocol (fogWraith
//! `Capabilities-Inline-Media.md`).
//!
//! Phase A (per `docs/inline-media.md`) lands the wire-format
//! layer: builders for `TranUploadMedia` (750) and `TranDownloadMedia`
//! (751) in single-shot and chunked variants, a parser for the
//! advisory limits the server advertises in the LOGIN reply, a
//! parser for the receive-side chat fields (`DATA_CHAT_MEDIA_ID` +
//! `DATA_CHAT_MEDIA_TYPE` companions plus the server-supplied
//! width/height/bytes), an upload-success-reply parser (handle +
//! canonical metadata), a download-reply parser (payload + chunk
//! bookkeeping), and the [`MediaErrorCode`] enum for the optional
//! machine-readable rejection category on error replies.
//!
//! The shape mirrors [`crate::voice`]: builders return [`HxChunk`]
//! arrays into caller-provided scratch + chunk slices, parsers walk
//! borrowed byte slices and produce plain Rust structs with no
//! allocator hits on the common path. Same FFI discipline — the C
//! side hand-declares the extern prototypes in `src/hotline_proto.h`
//! and a signature drift surfaces as an undefined symbol at link
//! time.
//!
//! ## Wire shapes (quick reference)
//!
//! All fields use the standard Hotline TLV framing (`u16 tag`,
//! `u16 len`, payload). All integer payloads are big-endian.
//!
//! ### TranUploadMedia (750), client → server
//!
//! Single-shot (bytes fit in one chunk):
//!
//! - `CHAT_MEDIA_PAYLOAD` (0x0203) — image bytes.
//! - `CHAT_MEDIA_DECLARED_TYPE` (0x0204) — optional MIME hint.
//! - `CHAT_MEDIA_PART_FINAL` (0x020b) — non-zero u8.
//!
//! Chunked, first chunk:
//!
//! - `CHAT_MEDIA_PAYLOAD` — this chunk's bytes.
//! - `CHAT_MEDIA_DECLARED_TYPE` — optional.
//! - `CHAT_MEDIA_PART_INDEX` (0x0209) — optional u16 BE (defaults 0).
//! - `CHAT_MEDIA_PART_COUNT` (0x020a) — u16 BE, total chunk count.
//! - `CHAT_MEDIA_PART_FINAL` — zero u8 (or absent).
//!
//! Chunked, subsequent chunks:
//!
//! - `CHAT_MEDIA_UPLOAD_TOKEN` (0x0208) — echoed from first reply.
//! - `CHAT_MEDIA_PART_INDEX` — u16 BE.
//! - `CHAT_MEDIA_PAYLOAD` — this chunk's bytes.
//! - `CHAT_MEDIA_PART_FINAL` — non-zero u8 on final chunk only.
//!
//! Reply on final-chunk success:
//!
//! - `CHAT_MEDIA_ID` (0x0202) — opaque handle.
//! - `CHAT_MEDIA_TYPE` (0x0201) — canonical MIME after re-encoding.
//! - `CHAT_MEDIA_WIDTH` / `HEIGHT` / `BYTES` (0x0205/06/07) — u32 BE.
//!
//! Reply on intermediate chunks: server echoes
//! `CHAT_MEDIA_UPLOAD_TOKEN` (only required in the first reply; safe
//! to echo throughout).
//!
//! Reply on failure: standard task-error transaction with
//! `DATA_ERROR` text, optionally `CHAT_MEDIA_ERROR_CODE` (0x0212) u16
//! BE machine-readable category.
//!
//! ### TranDownloadMedia (751), client → server
//!
//! Request:
//!
//! - `CHAT_MEDIA_ID` — the handle to fetch.
//! - `CHAT_MEDIA_PART_INDEX` — optional u16 BE; absent on first
//!   request, present on subsequent chunk fetches.
//!
//! Reply:
//!
//! - `CHAT_MEDIA_PAYLOAD` — canonical bytes (this chunk).
//! - `CHAT_MEDIA_TYPE` — canonical MIME.
//! - `CHAT_MEDIA_PART_COUNT` — total chunk count.
//! - `CHAT_MEDIA_PART_FINAL` — non-zero on the final chunk.
//!
//! Reply on failure: as above. Unauthorized vs expired collapse to
//! the same coarse code (`0` or `4`) per spec.
//!
//! ### Receive-side chat companion fields (105/106/108/104)
//!
//! When the relayed chat carries media, the server adds:
//!
//! - `CHAT_MEDIA_ID` + `CHAT_MEDIA_TYPE` (always paired).
//! - `CHAT_MEDIA_WIDTH` / `HEIGHT` / `BYTES` (server-supplied
//!   advisory metadata for UI placeholder sizing).
//!
//! [`extract_chat_media_meta`] walks the chunk stream and pulls
//! those fields into a typed [`ChatMediaMeta`] for the receive
//! handler in C.
//!
//! ### LOGIN reply advisory limits
//!
//! When the server confirms `CAPABILITY_INLINE_MEDIA` in the LOGIN
//! reply it MUST also include the [`LimitsAdvertisement`] fields
//! alongside `DATA_CAPABILITIES`. [`extract_limits`] picks them out
//! of the chunk stream — each field is independently optional and
//! falls back to the spec recommended default on the C side
//! (`HX_MEDIA_DEFAULT_*` in `src/hotline.h`).
//!
//! ## Builder lifetime convention
//!
//! Builders return `HxChunk` triples that borrow into the caller's
//! `scratch` slice (integer fields) and into any payload slices the
//! request struct points at (payload bytes, declared type, upload
//! token). The C-side `hlwrite_chunks` consumes those pointers
//! immediately to copy bytes onto the socket — same discipline as
//! the rest of `build`.

use crate::build::HxChunk;
use crate::messages::tag;
use crate::wire::{Chunk, ChunkIter};

// ---- Error code -----------------------------------------------------------

/// Optional machine-readable category for an inline-media error
/// reply (`DATA_CHAT_MEDIA_ERROR_CODE`, 0x0212). The spec calls
/// this out as "coarse" — the human `DATA_ERROR` text remains the
/// source of truth for display. Clients MUST treat any unknown
/// code as equivalent to [`Generic`](Self::Generic).
///
/// See the spec's "Error Codes" table.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u16)]
pub enum MediaErrorCode {
    /// Default when no other code applies.
    Generic = 0,
    /// Encoded size, dimensions, pixel count, frame count, or
    /// duration exceeded a server cap.
    PayloadTooLarge = 1,
    /// Magic-byte sniff failed, container rejected, or
    /// canonicalisation impossible.
    UnsupportedFormat = 2,
    /// Per-account or per-IP rate or volume cap hit.
    RateLimited = 3,
    /// Sender lacks `AccessSendMedia`, recipient is not on the
    /// handle's authorization set, or handle expired. (Server
    /// collapses "expired" and "not authorized" into the same
    /// code to avoid enumerating handle existence.)
    NotAuthorized = 4,
    /// Decode budget exhausted, too many concurrent upload
    /// sessions, transient resource pressure.
    ServerBusy = 5,
}

impl MediaErrorCode {
    /// Map a raw wire value to the enum. Unknown values collapse
    /// to [`Generic`](Self::Generic) per spec.
    pub fn from_u16(v: u16) -> MediaErrorCode {
        match v {
            1 => MediaErrorCode::PayloadTooLarge,
            2 => MediaErrorCode::UnsupportedFormat,
            3 => MediaErrorCode::RateLimited,
            4 => MediaErrorCode::NotAuthorized,
            5 => MediaErrorCode::ServerBusy,
            _ => MediaErrorCode::Generic,
        }
    }

    /// Wire value.
    pub fn as_u16(self) -> u16 {
        self as u16
    }
}

// ---- LOGIN reply advisory limits -----------------------------------------

/// Server-advertised advisory limits, parsed out of the LOGIN
/// reply when `CAPABILITY_INLINE_MEDIA` is confirmed.
///
/// Each field is independently optional on the wire. `None` means
/// the server didn't advertise that field; the spec says the
/// client SHOULD fall back to a sensible default (`HX_MEDIA_DEFAULT_*`
/// in `src/hotline.h`).
///
/// Values larger than `u32::MAX` aren't representable on the wire
/// (the spec uses u32 BE for each field), so there's no overflow
/// path to worry about — the parser just reads 4-byte chunks.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct LimitsAdvertisement {
    pub max_bytes: Option<u32>,
    pub max_dimension: Option<u32>,
    pub max_pixels: Option<u32>,
    pub chunk_size: Option<u32>,
    pub max_frames: Option<u32>,
    pub max_duration_ms: Option<u32>,
}

impl LimitsAdvertisement {
    /// Empty advertisement — every field `None`.
    pub fn empty() -> LimitsAdvertisement {
        LimitsAdvertisement::default()
    }

    /// True if any field was set. Useful for the "did the server
    /// echo the cap and advertise anything?" log line on the
    /// C side.
    pub fn any(&self) -> bool {
        self.max_bytes.is_some()
            || self.max_dimension.is_some()
            || self.max_pixels.is_some()
            || self.chunk_size.is_some()
            || self.max_frames.is_some()
            || self.max_duration_ms.is_some()
    }
}

/// Walk a LOGIN-reply body and pluck out the inline-media advisory
/// limits. Other chunks are ignored.
///
/// The caller passes the full message buffer; iteration starts at
/// the standard 22-byte transaction header offset via
/// `ChunkIter::over_message`.
pub fn extract_limits_from_message(body: &[u8], body_len: usize) -> LimitsAdvertisement {
    extract_limits(ChunkIter::over_message(body, body_len))
}

/// Walk an arbitrary chunk iterator (e.g. one already positioned
/// past a count prefix) and pluck out the advisory limits. The
/// duplicate of [`extract_limits_from_message`] is here so the
/// Tier 2 fixtures can drive the walker without re-wrapping a
/// full message header.
pub fn extract_limits<'a>(chunks: impl Iterator<Item = Chunk<'a>>) -> LimitsAdvertisement {
    let mut out = LimitsAdvertisement::default();
    for c in chunks {
        match c.tag {
            tag::CHAT_MEDIA_MAX_BYTES => {
                if let Some(v) = u32_from_chunk(&c) {
                    out.max_bytes = Some(v);
                }
            }
            tag::CHAT_MEDIA_MAX_DIMENSION => {
                if let Some(v) = u32_from_chunk(&c) {
                    out.max_dimension = Some(v);
                }
            }
            tag::CHAT_MEDIA_MAX_PIXELS => {
                if let Some(v) = u32_from_chunk(&c) {
                    out.max_pixels = Some(v);
                }
            }
            tag::CHAT_MEDIA_CHUNK_SIZE => {
                if let Some(v) = u32_from_chunk(&c) {
                    out.chunk_size = Some(v);
                }
            }
            tag::CHAT_MEDIA_MAX_FRAMES => {
                if let Some(v) = u32_from_chunk(&c) {
                    out.max_frames = Some(v);
                }
            }
            tag::CHAT_MEDIA_MAX_DURATION_MS => {
                if let Some(v) = u32_from_chunk(&c) {
                    out.max_duration_ms = Some(v);
                }
            }
            _ => {}
        }
    }
    out
}

fn u32_from_chunk(c: &Chunk<'_>) -> Option<u32> {
    if c.data.len() != 4 {
        return None;
    }
    Some(u32::from_be_bytes([
        c.data[0], c.data[1], c.data[2], c.data[3],
    ]))
}

fn u16_from_chunk(c: &Chunk<'_>) -> Option<u16> {
    if c.data.len() != 2 {
        return None;
    }
    Some(u16::from_be_bytes([c.data[0], c.data[1]]))
}

// ---- Chat-relay receive companion fields ---------------------------------

/// Server-supplied media metadata extracted from an inbound chat
/// transaction (`TranChatMsg` 106 or `TranServerMsg` 104).
///
/// Both `id` and `type_` are required — the spec says either both
/// are present or both are absent; a receiver MUST reject a
/// transaction with exactly one of the two. The width / height /
/// bytes fields are server-supplied hints for UI placeholder
/// sizing; they are advisory and clients MUST NOT trust them as a
/// substitute for actually decoding the bytes [`extract_chat_media_meta`]
/// returns.
///
/// Borrowed against the original message buffer — same lifetime
/// discipline as `Chunk<'a>`. The C-side wrapper copies bytes out
/// of the handle / type strings before the buffer is recycled.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ChatMediaMeta<'a> {
    /// Opaque server-issued handle. ≤ 64 bytes per spec; we don't
    /// enforce that here — bounding is the C side's job, where the
    /// handle gets copied into a fixed buffer.
    pub id: &'a [u8],
    /// Canonical MIME type after server re-encoding. ASCII per
    /// MIME RFC; not validated as UTF-8 here.
    pub type_: &'a [u8],
    /// Server-advertised image width in pixels, if present.
    pub width: Option<u32>,
    /// Server-advertised image height in pixels, if present.
    pub height: Option<u32>,
    /// Server-advertised canonical byte size, if present.
    pub bytes: Option<u32>,
}

/// Walk the chunks of a chat transaction body and extract the
/// media-companion fields, if present.
///
/// Returns:
///
/// - `Ok(Some(meta))` when both ID and TYPE are present.
/// - `Ok(None)` when neither ID nor TYPE is present (no media on
///   this chat).
/// - `Err(MediaMetaError::OnlyOnePresent)` when exactly one of
///   them is present, per the spec's "receivers MUST reject"
///   requirement.
///
/// Other chunks (the chat body / chat-id / style etc.) are ignored
/// — the caller is expected to walk for them separately.
pub fn extract_chat_media_meta<'a>(
    chunks: impl Iterator<Item = Chunk<'a>>,
) -> Result<Option<ChatMediaMeta<'a>>, MediaMetaError> {
    let mut id: Option<&[u8]> = None;
    let mut type_: Option<&[u8]> = None;
    let mut width: Option<u32> = None;
    let mut height: Option<u32> = None;
    let mut bytes: Option<u32> = None;
    for c in chunks {
        match c.tag {
            tag::CHAT_MEDIA_ID => {
                id = Some(c.data);
            }
            tag::CHAT_MEDIA_TYPE => {
                type_ = Some(c.data);
            }
            tag::CHAT_MEDIA_WIDTH => {
                width = u32_from_chunk(&c);
            }
            tag::CHAT_MEDIA_HEIGHT => {
                height = u32_from_chunk(&c);
            }
            tag::CHAT_MEDIA_BYTES => {
                bytes = u32_from_chunk(&c);
            }
            _ => {}
        }
    }
    match (id, type_) {
        (Some(id), Some(type_)) => Ok(Some(ChatMediaMeta {
            id,
            type_,
            width,
            height,
            bytes,
        })),
        (None, None) => Ok(None),
        _ => Err(MediaMetaError::OnlyOnePresent),
    }
}

/// Companion-field validation error for [`extract_chat_media_meta`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MediaMetaError {
    /// Exactly one of `CHAT_MEDIA_ID` / `CHAT_MEDIA_TYPE` was
    /// present. Per spec the receiver MUST reject such a
    /// transaction — receivers cannot heuristically pair the
    /// orphan with anything else, and silently dropping it would
    /// mask a server-side bug.
    OnlyOnePresent,
}

// ---- Outgoing builders: TranUploadMedia ----------------------------------

/// Request data for [`build_upload_media_single_chunks`].
///
/// Single-shot uploads are the common case — the spec recommends a
/// 256 KB default cap, well under the `u16::MAX` wire-length limit,
/// so most images fit in one chunk.
pub struct UploadMediaSingle<'a> {
    /// Image bytes. Non-empty; the spec recommends rejecting
    /// payloads below 64 bytes server-side (no plausible image is
    /// shorter than its magic), and zero-length is universally
    /// invalid. Must fit in u16 wire length.
    pub payload: &'a [u8],
    /// Optional sender-declared MIME type (e.g. `b"image/png"`).
    /// Server treats this as a hint only; the magic-byte sniff is
    /// authoritative. Empty is treated identically to `None`.
    pub declared_type: Option<&'a [u8]>,
}

/// Build chunks for a single-shot `HTLC_HDR_UPLOAD_MEDIA` (750).
/// Wire shape (in canonical order):
///
/// - `CHAT_MEDIA_PAYLOAD` — `payload.len()` bytes.
/// - `CHAT_MEDIA_DECLARED_TYPE` — optional, omitted when `None` or
///   empty.
/// - `CHAT_MEDIA_PART_FINAL` — u8 `1`.
///
/// Scratch usage: 1 byte (PART_FINAL at +0).
///
/// Returns 2 (no declared type) or 3 (with declared type) on
/// success, or 0 on validation failure: empty payload, payload
/// exceeds `u16::MAX`, declared type exceeds `u16::MAX`, undersized
/// `chunks` or `scratch`.
pub fn build_upload_media_single_chunks(
    req: &UploadMediaSingle<'_>,
    chunks: &mut [HxChunk],
    scratch: &mut [u8],
) -> usize {
    if chunks.len() < 3 || scratch.is_empty() {
        return 0;
    }
    if req.payload.is_empty() || req.payload.len() > u16::MAX as usize {
        return 0;
    }
    let declared = req
        .declared_type
        .filter(|d| !d.is_empty() && d.len() <= u16::MAX as usize);
    let mut n = 0;
    chunks[n] = HxChunk {
        tag: tag::CHAT_MEDIA_PAYLOAD,
        len: req.payload.len() as u16,
        data: req.payload.as_ptr(),
    };
    n += 1;
    if let Some(d) = declared {
        chunks[n] = HxChunk {
            tag: tag::CHAT_MEDIA_DECLARED_TYPE,
            len: d.len() as u16,
            data: d.as_ptr(),
        };
        n += 1;
    }
    scratch[0] = 1;
    chunks[n] = HxChunk {
        tag: tag::CHAT_MEDIA_PART_FINAL,
        len: 1,
        data: scratch.as_ptr(),
    };
    n + 1
}

/// Request data for [`build_upload_media_first_chunks`] — first
/// chunk of a chunked upload session.
pub struct UploadMediaFirst<'a> {
    /// This chunk's bytes.
    pub payload: &'a [u8],
    /// Optional sender-declared MIME hint, first chunk only.
    pub declared_type: Option<&'a [u8]>,
    /// Total chunk count; included so the server can pre-allocate.
    pub part_count: u16,
}

/// Build chunks for the FIRST chunk of a chunked
/// `HTLC_HDR_UPLOAD_MEDIA` (750). Wire shape:
///
/// - `CHAT_MEDIA_PAYLOAD` — this chunk's bytes.
/// - `CHAT_MEDIA_DECLARED_TYPE` — optional.
/// - `CHAT_MEDIA_PART_INDEX` — u16 BE = 0.
/// - `CHAT_MEDIA_PART_COUNT` — u16 BE.
/// - `CHAT_MEDIA_PART_FINAL` — u8 = 0.
///
/// Scratch usage: 5 bytes (PART_INDEX at +0, PART_COUNT at +2,
/// PART_FINAL at +4).
///
/// Returns 4 (no declared type) or 5 (with declared type) on
/// success, or 0 on validation failure (empty payload, oversized,
/// part_count < 2, undersized buffers).
pub fn build_upload_media_first_chunks(
    req: &UploadMediaFirst<'_>,
    chunks: &mut [HxChunk],
    scratch: &mut [u8],
) -> usize {
    if chunks.len() < 5 || scratch.len() < 5 {
        return 0;
    }
    if req.payload.is_empty() || req.payload.len() > u16::MAX as usize {
        return 0;
    }
    // A "chunked" upload with part_count <= 1 is nonsense — it
    // would be a single-shot, and the caller should use that path.
    if req.part_count < 2 {
        return 0;
    }
    let declared = req
        .declared_type
        .filter(|d| !d.is_empty() && d.len() <= u16::MAX as usize);
    scratch[0..2].copy_from_slice(&0u16.to_be_bytes());
    scratch[2..4].copy_from_slice(&req.part_count.to_be_bytes());
    scratch[4] = 0;
    let mut n = 0;
    chunks[n] = HxChunk {
        tag: tag::CHAT_MEDIA_PAYLOAD,
        len: req.payload.len() as u16,
        data: req.payload.as_ptr(),
    };
    n += 1;
    if let Some(d) = declared {
        chunks[n] = HxChunk {
            tag: tag::CHAT_MEDIA_DECLARED_TYPE,
            len: d.len() as u16,
            data: d.as_ptr(),
        };
        n += 1;
    }
    chunks[n] = HxChunk {
        tag: tag::CHAT_MEDIA_PART_INDEX,
        len: 2,
        data: scratch.as_ptr(),
    };
    n += 1;
    chunks[n] = HxChunk {
        tag: tag::CHAT_MEDIA_PART_COUNT,
        len: 2,
        data: scratch[2..4].as_ptr(),
    };
    n += 1;
    chunks[n] = HxChunk {
        tag: tag::CHAT_MEDIA_PART_FINAL,
        len: 1,
        data: scratch[4..5].as_ptr(),
    };
    n + 1
}

/// Request data for [`build_upload_media_followup_chunks`] —
/// subsequent chunks of a chunked upload session (not the first).
pub struct UploadMediaFollowup<'a> {
    /// Upload-session token echoed from the first chunk's reply.
    /// Non-empty; the server requires it on every non-first chunk.
    pub upload_token: &'a [u8],
    /// This chunk's bytes.
    pub payload: &'a [u8],
    /// Zero-based chunk index. ≥ 1 (first chunk uses the FIRST
    /// builder).
    pub part_index: u16,
    /// True if this is the final chunk. Sets PART_FINAL = 1.
    pub final_chunk: bool,
}

/// Build chunks for a non-first chunked-upload `HTLC_HDR_UPLOAD_MEDIA`
/// (750). Wire shape:
///
/// - `CHAT_MEDIA_UPLOAD_TOKEN` — token bytes.
/// - `CHAT_MEDIA_PART_INDEX` — u16 BE ≥ 1.
/// - `CHAT_MEDIA_PAYLOAD` — this chunk's bytes.
/// - `CHAT_MEDIA_PART_FINAL` — u8 = 1 on final chunk, 0 otherwise.
///
/// Scratch usage: 3 bytes (PART_INDEX at +0, PART_FINAL at +2).
///
/// Returns 4 on success, or 0 on validation failure (empty token /
/// payload, oversized, part_index == 0, undersized buffers).
pub fn build_upload_media_followup_chunks(
    req: &UploadMediaFollowup<'_>,
    chunks: &mut [HxChunk],
    scratch: &mut [u8],
) -> usize {
    if chunks.len() < 4 || scratch.len() < 3 {
        return 0;
    }
    if req.upload_token.is_empty() || req.upload_token.len() > u16::MAX as usize {
        return 0;
    }
    if req.payload.is_empty() || req.payload.len() > u16::MAX as usize {
        return 0;
    }
    if req.part_index == 0 {
        // First chunk uses the FIRST builder — followup must be ≥ 1.
        return 0;
    }
    scratch[0..2].copy_from_slice(&req.part_index.to_be_bytes());
    scratch[2] = if req.final_chunk { 1 } else { 0 };
    chunks[0] = HxChunk {
        tag: tag::CHAT_MEDIA_UPLOAD_TOKEN,
        len: req.upload_token.len() as u16,
        data: req.upload_token.as_ptr(),
    };
    chunks[1] = HxChunk {
        tag: tag::CHAT_MEDIA_PART_INDEX,
        len: 2,
        data: scratch.as_ptr(),
    };
    chunks[2] = HxChunk {
        tag: tag::CHAT_MEDIA_PAYLOAD,
        len: req.payload.len() as u16,
        data: req.payload.as_ptr(),
    };
    chunks[3] = HxChunk {
        tag: tag::CHAT_MEDIA_PART_FINAL,
        len: 1,
        data: scratch[2..3].as_ptr(),
    };
    4
}

// ---- Outgoing builders: TranDownloadMedia --------------------------------

/// Request data for [`build_download_media_chunks`].
pub struct DownloadMedia<'a> {
    /// Handle to fetch. Non-empty.
    pub media_id: &'a [u8],
    /// Optional zero-based chunk index. `None` on the first request;
    /// `Some(N)` on follow-up requests for chunked replies.
    pub part_index: Option<u16>,
}

/// Build chunks for `HTLC_HDR_DOWNLOAD_MEDIA` (751). Wire shape:
///
/// - `CHAT_MEDIA_ID` — handle bytes.
/// - `CHAT_MEDIA_PART_INDEX` — optional u16 BE, only on follow-up
///   chunk requests.
///
/// Scratch usage: 2 bytes (PART_INDEX at +0, only when present).
///
/// Returns 1 (no part index) or 2 (with part index) on success, or
/// 0 on validation failure (empty / oversize handle, undersized
/// buffers).
pub fn build_download_media_chunks(
    req: &DownloadMedia<'_>,
    chunks: &mut [HxChunk],
    scratch: &mut [u8],
) -> usize {
    if chunks.is_empty() {
        return 0;
    }
    if req.media_id.is_empty() || req.media_id.len() > u16::MAX as usize {
        return 0;
    }
    chunks[0] = HxChunk {
        tag: tag::CHAT_MEDIA_ID,
        len: req.media_id.len() as u16,
        data: req.media_id.as_ptr(),
    };
    if let Some(idx) = req.part_index {
        if chunks.len() < 2 || scratch.len() < 2 {
            return 0;
        }
        scratch[0..2].copy_from_slice(&idx.to_be_bytes());
        chunks[1] = HxChunk {
            tag: tag::CHAT_MEDIA_PART_INDEX,
            len: 2,
            data: scratch.as_ptr(),
        };
        return 2;
    }
    1
}

// ---- Inbound parsers: upload reply ---------------------------------------

/// Parsed TranUploadMedia success reply on the final chunk.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct UploadFinalReply<'a> {
    /// Opaque server-issued media handle.
    pub media_id: &'a [u8],
    /// Canonical MIME type after server re-encoding.
    pub media_type: &'a [u8],
    /// Canonical image width in pixels. `None` if the server
    /// omitted (out-of-spec but tolerated — common values are
    /// reachable by re-fetching).
    pub width: Option<u32>,
    pub height: Option<u32>,
    pub bytes: Option<u32>,
}

/// Parse a TranUploadMedia final-success reply body.
///
/// Returns `Some` if both `CHAT_MEDIA_ID` and `CHAT_MEDIA_TYPE`
/// are present. Returns `None` if either is missing — the reply
/// shape is malformed and the caller should treat it as an error
/// (the spec mandates both fields on success).
pub fn parse_upload_final_reply<'a>(
    chunks: impl Iterator<Item = Chunk<'a>>,
) -> Option<UploadFinalReply<'a>> {
    let mut id: Option<&[u8]> = None;
    let mut type_: Option<&[u8]> = None;
    let mut width: Option<u32> = None;
    let mut height: Option<u32> = None;
    let mut bytes: Option<u32> = None;
    for c in chunks {
        match c.tag {
            tag::CHAT_MEDIA_ID => id = Some(c.data),
            tag::CHAT_MEDIA_TYPE => type_ = Some(c.data),
            tag::CHAT_MEDIA_WIDTH => width = u32_from_chunk(&c),
            tag::CHAT_MEDIA_HEIGHT => height = u32_from_chunk(&c),
            tag::CHAT_MEDIA_BYTES => bytes = u32_from_chunk(&c),
            _ => {}
        }
    }
    Some(UploadFinalReply {
        media_id: id?,
        media_type: type_?,
        width,
        height,
        bytes,
    })
}

/// Parse the upload-session token from a TranUploadMedia
/// intermediate-chunk reply. Returns the token bytes if present.
///
/// Followup chunks echo this token on every subsequent
/// TranUploadMedia request; the C-side state machine stashes it
/// on the upload session struct.
pub fn parse_upload_token_reply<'a>(chunks: impl Iterator<Item = Chunk<'a>>) -> Option<&'a [u8]> {
    for c in chunks {
        if c.tag == tag::CHAT_MEDIA_UPLOAD_TOKEN {
            return Some(c.data);
        }
    }
    None
}

// ---- Inbound parsers: download reply -------------------------------------

/// Parsed TranDownloadMedia reply.
///
/// The reply carries one chunk's worth of canonical bytes plus
/// chunk-bookkeeping fields. The C side accumulates payload bytes
/// across multiple round-trips until `final_chunk` is set.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DownloadReply<'a> {
    /// This chunk's payload bytes.
    pub payload: &'a [u8],
    /// Canonical MIME type. Set on every reply per spec.
    pub media_type: &'a [u8],
    /// Total chunk count.
    pub part_count: u16,
    /// True on the final chunk (PART_FINAL ≠ 0).
    pub final_chunk: bool,
}

/// Parse a TranDownloadMedia reply body. Returns `None` if any
/// required field is missing (`PAYLOAD`, `TYPE`).
///
/// The spec also mandates `PART_COUNT`; we tolerate its absence
/// and default to 1 (single-chunk reply) so a strictly-conforming
/// server doesn't have to emit a useless 0x020a chunk on
/// single-shot responses. `PART_FINAL` absent is treated as 0
/// (non-final).
pub fn parse_download_reply<'a>(
    chunks: impl Iterator<Item = Chunk<'a>>,
) -> Option<DownloadReply<'a>> {
    let mut payload: Option<&[u8]> = None;
    let mut media_type: Option<&[u8]> = None;
    let mut part_count: u16 = 1;
    let mut final_chunk = false;
    for c in chunks {
        match c.tag {
            tag::CHAT_MEDIA_PAYLOAD => payload = Some(c.data),
            tag::CHAT_MEDIA_TYPE => media_type = Some(c.data),
            tag::CHAT_MEDIA_PART_COUNT => {
                if let Some(v) = u16_from_chunk(&c) {
                    part_count = v;
                }
            }
            tag::CHAT_MEDIA_PART_FINAL => {
                // Non-zero u8 == final. The spec uses u8 length 1
                // but tolerate any non-zero byte the server emits.
                final_chunk = c.data.iter().copied().any(|b| b != 0);
            }
            _ => {}
        }
    }
    Some(DownloadReply {
        payload: payload?,
        media_type: media_type?,
        part_count,
        final_chunk,
    })
}

/// Pull the optional [`MediaErrorCode`] out of an error reply body.
///
/// Per spec the human-readable `DATA_ERROR` text is the source of
/// truth for display; this is just the coarse category for UX
/// branching (offer to resize on `1`, suggest another file on `2`,
/// back off on `3`/`5`).
pub fn extract_error_code<'a>(chunks: impl Iterator<Item = Chunk<'a>>) -> MediaErrorCode {
    for c in chunks {
        if c.tag == tag::CHAT_MEDIA_ERROR_CODE {
            if let Some(v) = u16_from_chunk(&c) {
                return MediaErrorCode::from_u16(v);
            }
        }
    }
    MediaErrorCode::Generic
}

// ---- Tests ----------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use crate::wire::Encoder;
    use crate::HL_HDR_LEN;

    fn header_padded(extra: &[u8]) -> Vec<u8> {
        let mut buf = vec![0u8; HL_HDR_LEN];
        buf.extend_from_slice(extra);
        buf
    }

    fn empty_chunks_array<const N: usize>() -> [HxChunk; N] {
        [HxChunk::EMPTY; N]
    }

    // ---- MediaErrorCode ---------------------------------------------------

    #[test]
    fn error_code_roundtrip_known_values() {
        assert_eq!(MediaErrorCode::from_u16(0), MediaErrorCode::Generic);
        assert_eq!(MediaErrorCode::from_u16(1), MediaErrorCode::PayloadTooLarge);
        assert_eq!(
            MediaErrorCode::from_u16(2),
            MediaErrorCode::UnsupportedFormat
        );
        assert_eq!(MediaErrorCode::from_u16(3), MediaErrorCode::RateLimited);
        assert_eq!(MediaErrorCode::from_u16(4), MediaErrorCode::NotAuthorized);
        assert_eq!(MediaErrorCode::from_u16(5), MediaErrorCode::ServerBusy);
        // Wire values: 0..=5
        assert_eq!(MediaErrorCode::Generic.as_u16(), 0);
        assert_eq!(MediaErrorCode::ServerBusy.as_u16(), 5);
    }

    #[test]
    fn error_code_unknown_collapses_to_generic() {
        // Per spec: unknown codes MUST be treated as 0.
        assert_eq!(MediaErrorCode::from_u16(6), MediaErrorCode::Generic);
        assert_eq!(MediaErrorCode::from_u16(0xffff), MediaErrorCode::Generic);
    }

    // ---- LimitsAdvertisement ---------------------------------------------

    #[test]
    fn limits_walk_picks_up_every_field() {
        let mut e = Encoder::new();
        e.put_chunk(tag::CHAT_MEDIA_MAX_BYTES, &(262_144u32).to_be_bytes());
        e.put_chunk(tag::CHAT_MEDIA_MAX_DIMENSION, &(2048u32).to_be_bytes());
        e.put_chunk(tag::CHAT_MEDIA_MAX_PIXELS, &(4_194_304u32).to_be_bytes());
        e.put_chunk(tag::CHAT_MEDIA_CHUNK_SIZE, &(60_000u32).to_be_bytes());
        e.put_chunk(tag::CHAT_MEDIA_MAX_FRAMES, &(150u32).to_be_bytes());
        e.put_chunk(tag::CHAT_MEDIA_MAX_DURATION_MS, &(15_000u32).to_be_bytes());
        let buf = header_padded(e.as_slice());
        let limits = extract_limits_from_message(&buf, buf.len());
        assert_eq!(limits.max_bytes, Some(262_144));
        assert_eq!(limits.max_dimension, Some(2048));
        assert_eq!(limits.max_pixels, Some(4_194_304));
        assert_eq!(limits.chunk_size, Some(60_000));
        assert_eq!(limits.max_frames, Some(150));
        assert_eq!(limits.max_duration_ms, Some(15_000));
        assert!(limits.any());
    }

    #[test]
    fn limits_walk_tolerates_absent_fields() {
        // Server confirms the cap but only advertises max_bytes; the
        // spec says clients MUST tolerate any individual field
        // being absent.
        let mut e = Encoder::new();
        e.put_chunk(tag::CHAT_MEDIA_MAX_BYTES, &(1024u32).to_be_bytes());
        let buf = header_padded(e.as_slice());
        let limits = extract_limits_from_message(&buf, buf.len());
        assert_eq!(limits.max_bytes, Some(1024));
        assert_eq!(limits.max_dimension, None);
        assert_eq!(limits.max_pixels, None);
        assert_eq!(limits.chunk_size, None);
        assert_eq!(limits.max_frames, None);
        assert_eq!(limits.max_duration_ms, None);
        assert!(limits.any());
    }

    #[test]
    fn limits_walk_empty_is_empty_advertisement() {
        let buf = header_padded(&[]);
        let limits = extract_limits_from_message(&buf, buf.len());
        assert_eq!(limits, LimitsAdvertisement::default());
        assert!(!limits.any());
    }

    #[test]
    fn limits_walk_rejects_wrong_width_chunks() {
        // A server that emits a 2-byte payload for what should be
        // a u32 field is malformed. Parser drops the value rather
        // than half-reading it.
        let mut e = Encoder::new();
        e.put_chunk(tag::CHAT_MEDIA_MAX_BYTES, &[0x01, 0x02]);
        let buf = header_padded(e.as_slice());
        let limits = extract_limits_from_message(&buf, buf.len());
        assert_eq!(limits.max_bytes, None);
    }

    // ---- ChatMediaMeta ---------------------------------------------------

    #[test]
    fn chat_media_meta_with_full_metadata() {
        let mut e = Encoder::new();
        e.put_chunk(tag::BODY, b"hi");
        e.put_chunk(tag::CHAT_MEDIA_ID, b"opaque-handle-bytes");
        e.put_chunk(tag::CHAT_MEDIA_TYPE, b"image/png");
        e.put_chunk(tag::CHAT_MEDIA_WIDTH, &(800u32).to_be_bytes());
        e.put_chunk(tag::CHAT_MEDIA_HEIGHT, &(600u32).to_be_bytes());
        e.put_chunk(tag::CHAT_MEDIA_BYTES, &(124_000u32).to_be_bytes());
        let buf = header_padded(e.as_slice());
        let meta = extract_chat_media_meta(ChunkIter::over_message(&buf, buf.len()))
            .expect("paired fields parse")
            .expect("media present");
        assert_eq!(meta.id, b"opaque-handle-bytes");
        assert_eq!(meta.type_, b"image/png");
        assert_eq!(meta.width, Some(800));
        assert_eq!(meta.height, Some(600));
        assert_eq!(meta.bytes, Some(124_000));
    }

    #[test]
    fn chat_media_meta_absent_when_neither_present() {
        let mut e = Encoder::new();
        e.put_chunk(tag::BODY, b"plain chat");
        let buf = header_padded(e.as_slice());
        let meta = extract_chat_media_meta(ChunkIter::over_message(&buf, buf.len()))
            .expect("no media is OK");
        assert!(meta.is_none());
    }

    #[test]
    fn chat_media_meta_rejects_orphan_id() {
        let mut e = Encoder::new();
        e.put_chunk(tag::BODY, b"hi");
        e.put_chunk(tag::CHAT_MEDIA_ID, b"orphan-handle");
        let buf = header_padded(e.as_slice());
        let err = extract_chat_media_meta(ChunkIter::over_message(&buf, buf.len()))
            .expect_err("missing TYPE companion rejects");
        assert_eq!(err, MediaMetaError::OnlyOnePresent);
    }

    #[test]
    fn chat_media_meta_rejects_orphan_type() {
        let mut e = Encoder::new();
        e.put_chunk(tag::CHAT_MEDIA_TYPE, b"image/png");
        let buf = header_padded(e.as_slice());
        let err = extract_chat_media_meta(ChunkIter::over_message(&buf, buf.len()))
            .expect_err("missing ID companion rejects");
        assert_eq!(err, MediaMetaError::OnlyOnePresent);
    }

    // ---- Upload single-shot builder --------------------------------------

    #[test]
    fn upload_single_builds_payload_plus_final() {
        let mut chunks = empty_chunks_array::<3>();
        let mut scratch = [0u8; 1];
        let req = UploadMediaSingle {
            payload: b"\x89PNG\r\n\x1a\nfake-png-bytes",
            declared_type: None,
        };
        let n = build_upload_media_single_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(n, 2);
        assert_eq!(chunks[0].tag, tag::CHAT_MEDIA_PAYLOAD);
        assert_eq!(chunks[0].len as usize, req.payload.len());
        assert_eq!(chunks[1].tag, tag::CHAT_MEDIA_PART_FINAL);
        assert_eq!(chunks[1].len, 1);
        // PART_FINAL pointer must reference scratch[0] holding 1.
        assert_eq!(scratch[0], 1);
    }

    #[test]
    fn upload_single_with_declared_type() {
        let mut chunks = empty_chunks_array::<3>();
        let mut scratch = [0u8; 1];
        let req = UploadMediaSingle {
            payload: b"fake-bytes",
            declared_type: Some(b"image/png"),
        };
        let n = build_upload_media_single_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(n, 3);
        assert_eq!(chunks[0].tag, tag::CHAT_MEDIA_PAYLOAD);
        assert_eq!(chunks[1].tag, tag::CHAT_MEDIA_DECLARED_TYPE);
        assert_eq!(chunks[2].tag, tag::CHAT_MEDIA_PART_FINAL);
    }

    #[test]
    fn upload_single_rejects_empty_payload() {
        let mut chunks = empty_chunks_array::<3>();
        let mut scratch = [0u8; 1];
        let req = UploadMediaSingle {
            payload: b"",
            declared_type: None,
        };
        assert_eq!(
            build_upload_media_single_chunks(&req, &mut chunks, &mut scratch),
            0
        );
    }

    #[test]
    fn upload_single_treats_empty_declared_as_absent() {
        let mut chunks = empty_chunks_array::<3>();
        let mut scratch = [0u8; 1];
        let req = UploadMediaSingle {
            payload: b"fake",
            declared_type: Some(b""),
        };
        let n = build_upload_media_single_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(n, 2);
        assert_eq!(chunks[1].tag, tag::CHAT_MEDIA_PART_FINAL);
    }

    #[test]
    fn upload_single_rejects_undersized_chunks_slice() {
        let mut chunks = empty_chunks_array::<2>();
        let mut scratch = [0u8; 1];
        let req = UploadMediaSingle {
            payload: b"fake",
            declared_type: None,
        };
        assert_eq!(
            build_upload_media_single_chunks(&req, &mut chunks, &mut scratch),
            0
        );
    }

    // ---- Upload first-chunk builder --------------------------------------

    #[test]
    fn upload_first_builds_with_count() {
        let mut chunks = empty_chunks_array::<5>();
        let mut scratch = [0u8; 5];
        let req = UploadMediaFirst {
            payload: b"first-chunk-bytes",
            declared_type: None,
            part_count: 4,
        };
        let n = build_upload_media_first_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(n, 4);
        assert_eq!(chunks[0].tag, tag::CHAT_MEDIA_PAYLOAD);
        assert_eq!(chunks[1].tag, tag::CHAT_MEDIA_PART_INDEX);
        assert_eq!(chunks[2].tag, tag::CHAT_MEDIA_PART_COUNT);
        assert_eq!(chunks[3].tag, tag::CHAT_MEDIA_PART_FINAL);
        // PART_INDEX == 0 (BE u16).
        assert_eq!(&scratch[0..2], &[0, 0]);
        // PART_COUNT == 4 (BE u16).
        assert_eq!(&scratch[2..4], &[0, 4]);
        // PART_FINAL == 0.
        assert_eq!(scratch[4], 0);
    }

    #[test]
    fn upload_first_rejects_part_count_under_two() {
        let mut chunks = empty_chunks_array::<5>();
        let mut scratch = [0u8; 5];
        let req = UploadMediaFirst {
            payload: b"x",
            declared_type: None,
            part_count: 1,
        };
        assert_eq!(
            build_upload_media_first_chunks(&req, &mut chunks, &mut scratch),
            0
        );
    }

    // ---- Upload followup-chunk builder -----------------------------------

    #[test]
    fn upload_followup_final_chunk_sets_final_byte() {
        let mut chunks = empty_chunks_array::<4>();
        let mut scratch = [0u8; 3];
        let req = UploadMediaFollowup {
            upload_token: b"token-bytes",
            payload: b"last-chunk",
            part_index: 3,
            final_chunk: true,
        };
        let n = build_upload_media_followup_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(n, 4);
        assert_eq!(chunks[0].tag, tag::CHAT_MEDIA_UPLOAD_TOKEN);
        assert_eq!(chunks[1].tag, tag::CHAT_MEDIA_PART_INDEX);
        assert_eq!(chunks[2].tag, tag::CHAT_MEDIA_PAYLOAD);
        assert_eq!(chunks[3].tag, tag::CHAT_MEDIA_PART_FINAL);
        // PART_INDEX = 3 (BE u16).
        assert_eq!(&scratch[0..2], &[0, 3]);
        // PART_FINAL = 1.
        assert_eq!(scratch[2], 1);
    }

    #[test]
    fn upload_followup_rejects_part_index_zero() {
        let mut chunks = empty_chunks_array::<4>();
        let mut scratch = [0u8; 3];
        let req = UploadMediaFollowup {
            upload_token: b"tok",
            payload: b"data",
            part_index: 0,
            final_chunk: false,
        };
        assert_eq!(
            build_upload_media_followup_chunks(&req, &mut chunks, &mut scratch),
            0
        );
    }

    #[test]
    fn upload_followup_rejects_empty_token() {
        let mut chunks = empty_chunks_array::<4>();
        let mut scratch = [0u8; 3];
        let req = UploadMediaFollowup {
            upload_token: b"",
            payload: b"data",
            part_index: 1,
            final_chunk: false,
        };
        assert_eq!(
            build_upload_media_followup_chunks(&req, &mut chunks, &mut scratch),
            0
        );
    }

    // ---- Download builder ------------------------------------------------

    #[test]
    fn download_first_request_omits_part_index() {
        let mut chunks = empty_chunks_array::<2>();
        let mut scratch = [0u8; 2];
        let req = DownloadMedia {
            media_id: b"handle",
            part_index: None,
        };
        let n = build_download_media_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(n, 1);
        assert_eq!(chunks[0].tag, tag::CHAT_MEDIA_ID);
    }

    #[test]
    fn download_followup_includes_part_index() {
        let mut chunks = empty_chunks_array::<2>();
        let mut scratch = [0u8; 2];
        let req = DownloadMedia {
            media_id: b"handle",
            part_index: Some(2),
        };
        let n = build_download_media_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(n, 2);
        assert_eq!(chunks[0].tag, tag::CHAT_MEDIA_ID);
        assert_eq!(chunks[1].tag, tag::CHAT_MEDIA_PART_INDEX);
        assert_eq!(&scratch[0..2], &[0, 2]);
    }

    #[test]
    fn download_rejects_empty_handle() {
        let mut chunks = empty_chunks_array::<2>();
        let mut scratch = [0u8; 2];
        let req = DownloadMedia {
            media_id: b"",
            part_index: None,
        };
        assert_eq!(
            build_download_media_chunks(&req, &mut chunks, &mut scratch),
            0
        );
    }

    // ---- Upload-reply parser ---------------------------------------------

    #[test]
    fn upload_final_reply_parses_handle_and_metadata() {
        let mut e = Encoder::new();
        e.put_chunk(tag::CHAT_MEDIA_ID, b"opaque-handle");
        e.put_chunk(tag::CHAT_MEDIA_TYPE, b"image/png");
        e.put_chunk(tag::CHAT_MEDIA_WIDTH, &(1024u32).to_be_bytes());
        e.put_chunk(tag::CHAT_MEDIA_HEIGHT, &(768u32).to_be_bytes());
        e.put_chunk(tag::CHAT_MEDIA_BYTES, &(98_765u32).to_be_bytes());
        let buf = header_padded(e.as_slice());
        let reply =
            parse_upload_final_reply(ChunkIter::over_message(&buf, buf.len())).expect("present");
        assert_eq!(reply.media_id, b"opaque-handle");
        assert_eq!(reply.media_type, b"image/png");
        assert_eq!(reply.width, Some(1024));
        assert_eq!(reply.height, Some(768));
        assert_eq!(reply.bytes, Some(98_765));
    }

    #[test]
    fn upload_final_reply_rejects_missing_id() {
        let mut e = Encoder::new();
        e.put_chunk(tag::CHAT_MEDIA_TYPE, b"image/png");
        let buf = header_padded(e.as_slice());
        let reply = parse_upload_final_reply(ChunkIter::over_message(&buf, buf.len()));
        assert!(reply.is_none());
    }

    #[test]
    fn upload_token_reply_extracts_token() {
        let mut e = Encoder::new();
        e.put_chunk(tag::CHAT_MEDIA_UPLOAD_TOKEN, b"token-12345");
        let buf = header_padded(e.as_slice());
        let token =
            parse_upload_token_reply(ChunkIter::over_message(&buf, buf.len())).expect("present");
        assert_eq!(token, b"token-12345");
    }

    #[test]
    fn upload_token_reply_none_when_absent() {
        let buf = header_padded(&[]);
        assert!(parse_upload_token_reply(ChunkIter::over_message(&buf, buf.len())).is_none());
    }

    // ---- Download-reply parser -------------------------------------------

    #[test]
    fn download_reply_parses_single_shot_chunk() {
        let mut e = Encoder::new();
        e.put_chunk(tag::CHAT_MEDIA_PAYLOAD, b"\x89PNG\r\n\x1ametablob");
        e.put_chunk(tag::CHAT_MEDIA_TYPE, b"image/png");
        e.put_chunk(tag::CHAT_MEDIA_PART_COUNT, &(1u16).to_be_bytes());
        e.put_chunk(tag::CHAT_MEDIA_PART_FINAL, &[1u8]);
        let buf = header_padded(e.as_slice());
        let reply =
            parse_download_reply(ChunkIter::over_message(&buf, buf.len())).expect("present");
        assert_eq!(reply.payload, b"\x89PNG\r\n\x1ametablob");
        assert_eq!(reply.media_type, b"image/png");
        assert_eq!(reply.part_count, 1);
        assert!(reply.final_chunk);
    }

    #[test]
    fn download_reply_defaults_part_count_when_absent() {
        let mut e = Encoder::new();
        e.put_chunk(tag::CHAT_MEDIA_PAYLOAD, b"data");
        e.put_chunk(tag::CHAT_MEDIA_TYPE, b"image/jpeg");
        // No PART_COUNT, no PART_FINAL.
        let buf = header_padded(e.as_slice());
        let reply =
            parse_download_reply(ChunkIter::over_message(&buf, buf.len())).expect("present");
        assert_eq!(reply.part_count, 1);
        assert!(!reply.final_chunk);
    }

    #[test]
    fn download_reply_rejects_missing_payload() {
        let mut e = Encoder::new();
        e.put_chunk(tag::CHAT_MEDIA_TYPE, b"image/png");
        let buf = header_padded(e.as_slice());
        assert!(parse_download_reply(ChunkIter::over_message(&buf, buf.len())).is_none());
    }

    // ---- Error-code extraction -------------------------------------------

    #[test]
    fn extract_error_code_picks_up_when_present() {
        let mut e = Encoder::new();
        e.put_chunk(tag::TASK_ERROR, b"Media rejected");
        e.put_chunk(tag::CHAT_MEDIA_ERROR_CODE, &(1u16).to_be_bytes());
        let buf = header_padded(e.as_slice());
        let code = extract_error_code(ChunkIter::over_message(&buf, buf.len()));
        assert_eq!(code, MediaErrorCode::PayloadTooLarge);
    }

    #[test]
    fn extract_error_code_defaults_to_generic_when_absent() {
        let mut e = Encoder::new();
        e.put_chunk(tag::TASK_ERROR, b"Media rejected");
        let buf = header_padded(e.as_slice());
        let code = extract_error_code(ChunkIter::over_message(&buf, buf.len()));
        assert_eq!(code, MediaErrorCode::Generic);
    }
}
