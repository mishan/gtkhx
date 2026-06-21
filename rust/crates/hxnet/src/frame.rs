//! Plaintext Hotline frame: header + body.
//!
//! A frame is a 22-byte `hl_hdr` (decoded via
//! [`hotline_proto::parse::decode_header_full`] into a
//! [`hotline_proto::parse::HeaderDecoded`]) plus a body of
//! `wire_len - 2` bytes. The minus-2 is for the `hc`
//! (chunk-count) field, which the wire-protocol header counts
//! as part of `len` but `HeaderDecoded::body_len` has already
//! stripped — see `parse::decode_header_full` for the
//! arithmetic. The Frame holds the decoded form (not the raw
//! `Header`) so consumers get `body_len` and `wire_len` as
//! pre-computed fields rather than re-deriving them.
//!
//! Frames are the protocol's read-side unit; the actor in
//! [`crate::connection`] reads one at a time off the wire and
//! emits an [`Event::Frame`](crate::Event::Frame) per frame.
//! Higher-level event decoding (Chat, Msg, UserCreate, …) layers
//! on top by calling the relevant `hotline_proto::parse` functions
//! against `frame.body`.

use hotline_proto::parse::{decode_header_full, HeaderDecoded};
use hotline_proto::HL_HDR_LEN;

/// Maximum body byte count the actor will accept on a single
/// frame. Matches `MAX_HOTLINE_PACKET_LEN` from `src/compat.h`
/// (1 MiB). The actor enforces this **before any body bytes are
/// read**: if a header's on-wire `len` exceeds `MAX_BODY_LEN + 2`
/// (the +2 is the `hc` field that wire `len` counts), the read
/// loop emits [`crate::ShutdownReason::FrameTooLarge`] and exits
/// without allocating or reading a body. There is no clamping,
/// no partial read — the frame is refused outright so we never
/// mis-align subsequent frames on the stream. See
/// [`crate::connection`]'s `read_one_frame` for the policy.
pub const MAX_BODY_LEN: u32 = 1024 * 1024;

/// A complete Hotline frame as the actor reads it off the wire.
///
/// Memory: the body is owned by the frame, allocated by the
/// actor's read loop. The actor does not pool buffers — each frame
/// gets its own `Vec`. Profile-driven follow-up if allocation
/// pressure shows up.
#[derive(Debug, Clone)]
pub struct Frame {
    /// 22-byte transaction header, already decoded.
    pub header: HeaderDecoded,
    /// Body bytes following the header. Length equals
    /// `header.body_len`; the actor enforces this on the way in.
    pub body: Vec<u8>,
}

impl Frame {
    /// Build a frame from a header and a body buffer. Used by
    /// tests; the actor builds frames internally.
    pub fn new(header: HeaderDecoded, body: Vec<u8>) -> Self {
        Self { header, body }
    }

    /// Reconstruct a frame from its full on-wire bytes: the
    /// 22-byte `HL_HDR_LEN` header (which already includes the
    /// 2-byte chunk-count field) followed by `body_len` bytes of
    /// chunk data. Returns `None` if the header doesn't decode or
    /// the buffer is shorter than `HL_HDR_LEN + body_len`.
    ///
    /// This is the Phase G login-reply replay path
    /// (`docs/phase-g-migration.md`, "Option B"). The orchestrator
    /// reads + parses the LOGIN reply once to decide
    /// success/failure, then re-emits the verbatim wire bytes as an
    /// `Event::Frame` so the C-side `rcv.c` dispatch (`rcv_task_login`)
    /// runs unchanged and produces the post-login side effects. The
    /// slicing here is deliberately identical to the actor's
    /// `read_one_frame` (`HL_HDR_LEN` header + `body_len` body), so a
    /// replayed frame and an actor-read frame are byte-for-byte the
    /// same shape by the time they reach the FFI.
    pub fn from_raw(raw: &[u8]) -> Option<Frame> {
        // Decode WITHOUT clamping (u32::MAX) so body_len reflects the
        // real on-wire length, then reject oversized frames by the raw
        // wire_len — same non-clamping policy as
        // connection::read_one_frame. Passing MAX_BODY_LEN+2 as the clamp
        // would let decode_header_full silently truncate body_len for an
        // oversized header and yield a Frame that misrepresents the
        // on-wire bytes. (wire_len includes the 2-byte hc, hence +2.)
        let header = decode_header_full(raw, u32::MAX)?;
        if header.wire_len > MAX_BODY_LEN + 2 {
            return None;
        }
        let body_len = header.body_len as usize;
        let end = HL_HDR_LEN.checked_add(body_len)?;
        if raw.len() < end {
            return None;
        }
        Some(Frame {
            header,
            body: raw[HL_HDR_LEN..end].to_vec(),
        })
    }
}
