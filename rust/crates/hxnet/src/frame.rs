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

use hotline_proto::parse::HeaderDecoded;

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
}
