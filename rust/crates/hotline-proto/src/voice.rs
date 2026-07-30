//! Voice-chat extension wire protocol (fogWraith
//! `Capabilities-Voice.md`, commit `525e94e`).
//!
//! Phase 8.A lands the wire-format layer: builders for outgoing
//! transactions (600, 601, 603, 604, 606) and parsers for inbound
//! payloads (the JOIN reply / 602 / 605 SDP / participant blobs /
//! mid labels / ICE JSON). The state machine, GStreamer runtime,
//! audio I/O, and chat-tab UI all land in later sub-phases (8.B
//! onward); nothing here touches GLib / GStreamer / GTK.
//!
//! The shape mirrors the rest of `hotline-proto`: builders return
//! [`HxChunk`] arrays into caller-provided scratch + chunk slices,
//! parsers walk borrowed byte slices and produce plain Rust structs
//! with no allocator hits on the common path.
//!
//! ## Sub-modules
//!
//! - [`sdp`] — minimal SDP shape parser. Just enough to extract the
//!   `a=mid:` labels, the `a=group:BUNDLE` list, and disabled-slot
//!   detection (`m=audio 0`) so the eventual state machine can
//!   sanity-check what it's about to set as the remote description.
//!   Real SDP parsing stays in `gst_sdp::SDPMessage` (Phase 8.C).
//! - [`ice`] — tiny build + parse for the `RTCIceCandidateInit` JSON
//!   dict (`candidate`, `sdpMid`, `sdpMLineIndex`,
//!   `usernameFragment`). Hand-rolled rather than pulling
//!   `serde_json` into the crate's dep graph; the 4-field shape is
//!   bounded and easy to walk.
//!
//! ## Wire shapes (quick reference)
//!
//! All fields use the standard Hotline TLV framing (`u16 tag`,
//! `u16 len`, payload). All integer payloads are big-endian.
//!
//! | Opcode | Direction | Fields |
//! |---|---|---|
//! | 600 `VOICE_JOIN` | C→S | `CHAT_ID` (u32) |
//! | 601 `VOICE_LEAVE` | C→S | `CHAT_ID` (u32) |
//! | 602 `VOICE_SDP_OFFER` | S→C | `CHAT_ID` + `VOICE_SDP` |
//! | 603 `VOICE_SDP_ANSWER` | C→S | `CHAT_ID` + `VOICE_SDP` |
//! | 604 `VOICE_ICE` | both | `CHAT_ID` + `VOICE_ICE` |
//! | 605 `VOICE_ROOM_STATUS` | S→C | `CHAT_ID` + `VOICE_PARTICIPANTS` |
//! | 606 `VOICE_MUTE` | C→S | `CHAT_ID` + `VOICE_MUTED` (u16) |
//!
//! The JOIN reply additionally carries `VOICE_SDP`, `VOICE_CODEC`,
//! and `VOICE_PARTICIPANTS`; see [`parse_voice_join_reply`].
//!
//! ## Builder lifetime convention
//!
//! Builders return `HxChunk` triples that borrow into the caller's
//! `scratch` slice (integer fields) and into any payload slices the
//! request struct points at (SDP, ICE JSON, codec name). The C-side
//! `hlwrite_chunks` consumes those pointers immediately to copy
//! bytes onto the socket — same discipline as the rest of `build`.

use crate::build::HxChunk;
use crate::messages::tag;
use crate::wire::ChunkIter;

// ---- Outgoing builders -------------------------------------------------

/// Single-`CHAT_ID` chunk builder used by both VOICE_JOIN (600) and
/// VOICE_LEAVE (601). Wire shape:
///
/// - `HTLC_DATA_CHAT_ID` (`tag::CHAT_ID`, 0x0072) — u32 BE, 4 bytes.
///
/// Returns 1 on success, or 0 on validation failure (`chunks` empty
/// or `scratch` shorter than 4 bytes).
fn build_chat_id_only(cid: u32, chunks: &mut [HxChunk], scratch: &mut [u8]) -> usize {
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

/// Build chunks for `HTLC_HDR_VOICE_JOIN` (600). Request shape per the
/// spec is just the chat-room id; the public-chat case (`cid == 0`)
/// still emits the CHAT_ID chunk so the server can disambiguate the
/// lobby from "missing field, fall back to legacy chat-history
/// behaviour" elsewhere in its dispatcher.
pub fn build_voice_join_chunks(cid: u32, chunks: &mut [HxChunk], scratch: &mut [u8]) -> usize {
    build_chat_id_only(cid, chunks, scratch)
}

/// Build chunks for `HTLC_HDR_VOICE_LEAVE` (601). Same wire shape as
/// JOIN: a single CHAT_ID chunk.
pub fn build_voice_leave_chunks(cid: u32, chunks: &mut [HxChunk], scratch: &mut [u8]) -> usize {
    build_chat_id_only(cid, chunks, scratch)
}

/// Build chunks for `HTLC_HDR_VOICE_SDP_ANSWER` (603). Wire shape:
///
/// - `HTLC_DATA_CHAT_ID` — u32 BE, 4 bytes.
/// - `HTLC_DATA_VOICE_SDP` — UTF-8 SDP answer bytes.
///
/// Empty SDP is rejected (the spec disallows empty answers — a client
/// that cannot offer PCMU has nothing useful to say). Oversize SDP
/// (>`u16::MAX`) is rejected: chunk lengths are 16 bits on the wire
/// and silently wrapping would produce a malformed frame. The 32 KB
/// soft cap the spec mentions is a SHOULD; we honour the hard wire
/// limit and let the consumer impose the SHOULD if it wants to.
pub fn build_voice_answer_chunks(
    cid: u32,
    sdp: &[u8],
    chunks: &mut [HxChunk],
    scratch: &mut [u8],
) -> usize {
    if chunks.len() < 2 || scratch.len() < 4 || sdp.is_empty() || sdp.len() > u16::MAX as usize {
        return 0;
    }
    scratch[0..4].copy_from_slice(&cid.to_be_bytes());
    chunks[0] = HxChunk {
        tag: tag::CHAT_ID,
        len: 4,
        data: scratch.as_ptr(),
    };
    chunks[1] = HxChunk {
        tag: tag::VOICE_SDP,
        len: sdp.len() as u16,
        data: sdp.as_ptr(),
    };
    2
}

/// Build chunks for `HTLC_HDR_VOICE_ICE` (604, client-side). Wire shape:
///
/// - `HTLC_DATA_CHAT_ID` — u32 BE, 4 bytes.
/// - `HTLC_DATA_VOICE_ICE` — UTF-8 JSON-encoded RTCIceCandidateInit, or
///   the empty string to signal end-of-candidates.
///
/// Empty ICE bytes are valid (end-of-candidates marker per spec). The
/// builder accepts them and emits a zero-length VOICE_ICE chunk;
/// receivers interpret the absence of body bytes as the marker.
///
/// `ice` must fit in 16 bits (per wire framing). A pathological SDP
/// attribute string longer than 65535 bytes is rejected; in practice
/// ICE candidates are around 100 bytes.
pub fn build_voice_ice_chunks(
    cid: u32,
    ice: &[u8],
    chunks: &mut [HxChunk],
    scratch: &mut [u8],
) -> usize {
    if chunks.len() < 2 || scratch.len() < 4 || ice.len() > u16::MAX as usize {
        return 0;
    }
    scratch[0..4].copy_from_slice(&cid.to_be_bytes());
    chunks[0] = HxChunk {
        tag: tag::CHAT_ID,
        len: 4,
        data: scratch.as_ptr(),
    };
    chunks[1] = HxChunk {
        tag: tag::VOICE_ICE,
        len: ice.len() as u16,
        data: if ice.is_empty() {
            b"".as_ptr()
        } else {
            ice.as_ptr()
        },
    };
    2
}

/// Build chunks for `HTLC_HDR_VOICE_MUTE` (606). Wire shape:
///
/// - `HTLC_DATA_CHAT_ID` — u32 BE, 4 bytes.
/// - `HTLC_DATA_VOICE_MUTED` — u16 BE, 2 bytes. `0` = unmuted, `1` =
///   muted. Values other than 0/1 are reserved; the builder accepts
///   any u16 the caller hands in to mirror the C side's convention,
///   and the C wrapper will normalise to 0/1.
///
/// Scratch usage: 6 bytes (cid at +0, muted at +4).
pub fn build_voice_mute_chunks(
    cid: u32,
    muted: u16,
    chunks: &mut [HxChunk],
    scratch: &mut [u8],
) -> usize {
    if chunks.len() < 2 || scratch.len() < 6 {
        return 0;
    }
    scratch[0..4].copy_from_slice(&cid.to_be_bytes());
    scratch[4..6].copy_from_slice(&muted.to_be_bytes());
    chunks[0] = HxChunk {
        tag: tag::CHAT_ID,
        len: 4,
        data: scratch.as_ptr(),
    };
    chunks[1] = HxChunk {
        tag: tag::VOICE_MUTED,
        len: 2,
        data: scratch[4..6].as_ptr(),
    };
    2
}

// ---- Inbound parsers --------------------------------------------------

/// One voice participant, decoded from a 6-byte entry of the packed
/// `DATA_VOICE_PARTICIPANTS` blob.
///
/// Layout on the wire:
/// ```text
///   [2] user_id   (big-endian u16)
///   [2] flags     (big-endian u16: bit 0 = muted, bits 1-15 reserved)
///   [2] codec_id  (big-endian u16: see Codec ID Table)
/// ```
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Participant {
    pub user_id: u16,
    pub flags: u16,
    pub codec_id: u16,
}

impl Participant {
    /// Convenience accessor for flags bit 0.
    pub fn is_muted(&self) -> bool {
        self.flags & 0x0001 != 0
    }
}

/// Iterator over participants in a `DATA_VOICE_PARTICIPANTS` blob.
///
/// The blob is a packed array of 6-byte entries; the iterator walks
/// it without copying. A trailing partial entry (length not a
/// multiple of 6) is treated as a truncated wire frame: iteration
/// stops at the last full entry, matching the defensive shape the
/// rest of `hotline-proto`'s parsers use. The fogWraith spec does
/// not specify a hard cap on the participant count; we rely on the
/// wire framing's u16 length (so at most 65535 / 6 ≈ 10922 entries
/// could fit in one chunk, well beyond the room-size limits the spec
/// recommends).
pub fn parse_voice_participants(blob: &[u8]) -> impl Iterator<Item = Participant> + '_ {
    blob.chunks_exact(6).map(|c| Participant {
        user_id: u16::from_be_bytes([c[0], c[1]]),
        flags: u16::from_be_bytes([c[2], c[3]]),
        codec_id: u16::from_be_bytes([c[4], c[5]]),
    })
}

/// Result of parsing an SDP `a=mid:` label.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MidLabel {
    /// `a=mid:send` — the local client's send track.
    Send,
    /// `a=mid:user-{UID}` — receive track for the user with the given
    /// id. `0` is reserved by the spec and rejected by the parser.
    User(u16),
}

/// Parse an SDP `a=mid:` label value.
///
/// Returns `Some(MidLabel)` for the well-formed shapes (`send` /
/// `user-N`), `None` for anything else. The strict shape matches the
/// spec:
///
/// - `user-N` where N is `1..=65535` in decimal, **no leading zeros**.
///   `user-05` is rejected — the spec explicitly forbids leading zeros.
/// - `user-0` is rejected — uid 0 is reserved by the Hotline base
///   protocol.
/// - Trailing whitespace / arbitrary suffixes are rejected.
///
/// Defensive parsing matters here: the spec calls out parse failure
/// as a "reject the track" condition rather than an assertion.
pub fn parse_voice_mid_label(s: &[u8]) -> Option<MidLabel> {
    if s == b"send" {
        return Some(MidLabel::Send);
    }
    let rest = s.strip_prefix(b"user-")?;
    if rest.is_empty() {
        return None;
    }
    // Reject leading zeros. Single "0" is also rejected (UID 0 reserved).
    if rest.first() == Some(&b'0') {
        return None;
    }
    if !rest.iter().all(|b| b.is_ascii_digit()) {
        return None;
    }
    // u32 to bound the parse, then range-check into u16.
    let mut acc: u32 = 0;
    for b in rest {
        acc = acc.checked_mul(10)?.checked_add((b - b'0') as u32)?;
        if acc > u16::MAX as u32 {
            return None;
        }
    }
    if acc == 0 {
        return None;
    }
    Some(MidLabel::User(acc as u16))
}

pub mod sdp {
    //! Minimal SDP shape parser.
    //!
    //! Just enough surface for the Phase 8.A logging path and the
    //! future state machine's sanity-check ("does this offer
    //! contain PCMU? does its BUNDLE list match the participant
    //! list?"). Real SDP parsing lives in `gst_sdp::SDPMessage`,
    //! reached via gstreamer-rs in Phase 8.C.

    use super::{parse_voice_mid_label, MidLabel};

    /// Distilled summary of an SDP offer/answer payload, sufficient
    /// for defensive checks before handing the SDP to the WebRTC
    /// stack.
    #[derive(Debug, Default, Clone, PartialEq, Eq)]
    pub struct SdpSummary {
        /// All `a=mid:` labels seen, in document order. Duplicates
        /// are preserved — they'd indicate a malformed SDP, and the
        /// caller may want to surface that.
        pub mids: Vec<MidLabel>,
        /// Raw `a=mid:` strings that did not parse as `send` or
        /// `user-N`. Useful for the proto-trace log line so a
        /// human can spot why a slot was rejected.
        pub unknown_mids: Vec<String>,
        /// The space-separated token list from the `a=group:BUNDLE`
        /// line, in declaration order. The spec mandates a single
        /// BUNDLE group covering every media section.
        pub bundle: Vec<String>,
        /// True if any `m=audio 0 ` line was seen — a disabled slot
        /// after a participant leaves (RFC 8829 §5.2.2 recycling).
        pub has_disabled_slot: bool,
        /// True if the SDP advertises PCMU (`a=rtpmap:0 PCMU/8000`).
        /// The spec requires it on every offer; we surface the
        /// boolean so the runtime can reject offers that lack it.
        pub has_pcmu: bool,
    }

    /// Walk the SDP one line at a time and collect the subset of
    /// attributes the runtime needs. Lines are CRLF or LF
    /// terminated; both are accepted (the spec is RFC-8866 CRLF but
    /// real-world stacks vary).
    pub fn summarize(sdp: &[u8]) -> SdpSummary {
        let mut out = SdpSummary::default();

        for line in sdp.split(|&b| b == b'\n') {
            // Strip a trailing CR if present.
            let line = match line.split_last() {
                Some((b'\r', rest)) => rest,
                _ => line,
            };

            if let Some(rest) = line.strip_prefix(b"a=mid:") {
                match parse_voice_mid_label(rest) {
                    Some(m) => out.mids.push(m),
                    None => out
                        .unknown_mids
                        .push(String::from_utf8_lossy(rest).into_owned()),
                }
            } else if let Some(rest) = line.strip_prefix(b"a=group:BUNDLE ") {
                out.bundle = rest
                    .split(|&b| b == b' ')
                    .filter(|s| !s.is_empty())
                    .map(|s| String::from_utf8_lossy(s).into_owned())
                    .collect();
            } else if line.starts_with(b"m=audio 0 ") {
                out.has_disabled_slot = true;
            } else if line.starts_with(b"a=rtpmap:0 PCMU/8000") {
                out.has_pcmu = true;
            }
        }

        out
    }
}

pub mod ice {
    //! Minimal `RTCIceCandidateInit` JSON build + parse.
    //!
    //! The dict has four documented fields, all string- or u32-typed.
    //! No nested objects, no arrays. Hand-rolled rather than pulling
    //! `serde_json` into the crate's dep graph — the bounded shape
    //! makes a small custom parser the cheaper option both at
    //! compile time and at audit time.
    //!
    //! The build path emits the keys in a stable order
    //! (`candidate`, `sdpMid`, `sdpMLineIndex`, `usernameFragment`).
    //! The parse path is order-insensitive and tolerates trailing
    //! whitespace, but does NOT accept JSON5 / comments / unquoted
    //! keys — RFC 8259-conformant input only.

    /// Parsed `RTCIceCandidateInit` dict.
    ///
    /// Per fogWraith Capabilities-Voice.md §"ICE Candidate Format",
    /// `candidate` and `sdpMid` are required on every payload;
    /// `sdp_mline_index` and `username_fragment` are optional.
    /// [`parse`] enforces the required-fields gate and refuses
    /// payloads missing either of the first two — on the value
    /// this returns, `candidate` and `sdp_mid` are always `Some`
    /// (test pinned by `ice_parse_rejects_missing_required_keys`).
    ///
    /// The `Option` typing on `candidate` / `sdp_mid` is retained
    /// so the same struct shape lets [`build`] emit JSON for
    /// partial in-flight candidates the runtime might construct
    /// internally — only the parse path is strict.
    ///
    /// An empty `candidate` string is the end-of-candidates marker
    /// per spec; the parser doesn't interpret that — callers branch
    /// on [`IceCandidate::is_end_of_candidates`] (which checks
    /// `candidate.as_deref() == Some("")`).
    #[derive(Debug, Default, Clone, PartialEq, Eq)]
    pub struct IceCandidate {
        pub candidate: Option<String>,
        pub sdp_mid: Option<String>,
        pub sdp_mline_index: Option<u32>,
        pub username_fragment: Option<String>,
    }

    impl IceCandidate {
        /// True when this is the end-of-candidates marker: the spec
        /// uses an empty `candidate` string OR an entirely empty
        /// `DATA_VOICE_ICE` field. Callers check the latter at the
        /// chunk-walk site; this method is the former.
        pub fn is_end_of_candidates(&self) -> bool {
            matches!(self.candidate.as_deref(), Some(""))
        }
    }

    /// Build the JSON for an outgoing ICE candidate. Caller owns the
    /// returned `String`; the C-side FFI shim copies it into a
    /// caller-provided buffer.
    ///
    /// `candidate` is the SDP candidate-attribute string (RFC 8839).
    /// Pass an empty string to emit the end-of-candidates marker.
    pub fn build(c: &IceCandidate) -> String {
        // Bounded preallocation: each field is at most a few hundred
        // bytes in practice; round up to 512 to absorb the keys +
        // quoting overhead.
        let mut s = String::with_capacity(512);
        s.push('{');
        let mut first = true;

        if let Some(v) = c.candidate.as_deref() {
            push_string_field(&mut s, &mut first, "candidate", v);
        }
        if let Some(v) = c.sdp_mid.as_deref() {
            push_string_field(&mut s, &mut first, "sdpMid", v);
        }
        if let Some(v) = c.sdp_mline_index {
            push_separator(&mut s, &mut first);
            s.push_str("\"sdpMLineIndex\":");
            s.push_str(&v.to_string());
        }
        if let Some(v) = c.username_fragment.as_deref() {
            push_string_field(&mut s, &mut first, "usernameFragment", v);
        }

        s.push('}');
        s
    }

    fn push_separator(s: &mut String, first: &mut bool) {
        if *first {
            *first = false;
        } else {
            s.push(',');
        }
    }

    fn push_string_field(s: &mut String, first: &mut bool, key: &str, value: &str) {
        push_separator(s, first);
        s.push('"');
        s.push_str(key);
        s.push_str("\":\"");
        escape_into(value, s);
        s.push('"');
    }

    fn escape_into(value: &str, s: &mut String) {
        for c in value.chars() {
            match c {
                '"' => s.push_str("\\\""),
                '\\' => s.push_str("\\\\"),
                '\n' => s.push_str("\\n"),
                '\r' => s.push_str("\\r"),
                '\t' => s.push_str("\\t"),
                c if (c as u32) < 0x20 => {
                    s.push_str(&format!("\\u{:04x}", c as u32));
                }
                c => s.push(c),
            }
        }
    }

    /// Parse the inner JSON payload carried in `DATA_VOICE_ICE`.
    ///
    /// Per fogWraith `Capabilities-Voice.md` §"ICE Candidate Format",
    /// the JSON object MUST carry `candidate` (string; may be empty
    /// for end-of-candidates) and `sdpMid` (string). `sdpMLineIndex`
    /// and `usernameFragment` are optional. This function rejects
    /// payloads that omit either required key — handing the runtime
    /// crate a partially populated `IceCandidate` would just push
    /// the missing-field check downstream, where it would surface
    /// as "ICE add failed" without the diagnostic that the wire
    /// payload was malformed.
    ///
    /// Returns `None` if the input is not a JSON object, if either
    /// required key is missing, or if any value fails the JSON
    /// grammar.
    ///
    /// Note: an empty wire payload (zero-length `DATA_VOICE_ICE`
    /// chunk) is the spec's end-of-candidates shorthand. The C-side
    /// dispatcher (`hx_rcv_voice_ice` in `src/rcv.c`) intercepts
    /// zero-length chunks before they reach this function and
    /// emits the EOC log line directly — `parse` here is only
    /// called on non-empty payloads.
    pub fn parse(json: &[u8]) -> Option<IceCandidate> {
        let mut p = Parser::new(json);
        p.skip_ws();
        if p.next()? != b'{' {
            return None;
        }
        p.skip_ws();

        let mut out = IceCandidate::default();

        if p.peek() == Some(b'}') {
            // Empty `{}` violates the spec — both `candidate` and
            // `sdpMid` are required keys. Fail closed here rather
            // than below so the policy is enforced uniformly.
            return None;
        }

        loop {
            p.skip_ws();
            let key = p.parse_string()?;
            p.skip_ws();
            if p.next()? != b':' {
                return None;
            }
            p.skip_ws();

            match key.as_str() {
                "candidate" => out.candidate = Some(p.parse_string()?),
                "sdpMid" => out.sdp_mid = Some(p.parse_string()?),
                "sdpMLineIndex" => {
                    let n = p.parse_uint()?;
                    if n > u32::MAX as u64 {
                        return None;
                    }
                    out.sdp_mline_index = Some(n as u32);
                }
                "usernameFragment" => out.username_fragment = Some(p.parse_string()?),
                // Unknown fields are ignored — forward compatibility
                // with spec extensions. Skip the value.
                _ => p.skip_value()?,
            }

            p.skip_ws();
            match p.next()? {
                b',' => continue,
                b'}' => break,
                _ => return None,
            }
        }
        p.skip_ws();
        // Allow trailing whitespace but reject trailing garbage.
        if p.peek().is_some() {
            return None;
        }
        // Spec compliance: both required keys must have been seen
        // somewhere in the member list.
        if out.candidate.is_none() || out.sdp_mid.is_none() {
            return None;
        }
        Some(out)
    }

    struct Parser<'a> {
        buf: &'a [u8],
        pos: usize,
    }

    impl<'a> Parser<'a> {
        fn new(buf: &'a [u8]) -> Self {
            Parser { buf, pos: 0 }
        }

        fn peek(&self) -> Option<u8> {
            self.buf.get(self.pos).copied()
        }

        fn next(&mut self) -> Option<u8> {
            let b = *self.buf.get(self.pos)?;
            self.pos += 1;
            Some(b)
        }

        fn skip_ws(&mut self) {
            while let Some(b) = self.peek() {
                if b == b' ' || b == b'\t' || b == b'\n' || b == b'\r' {
                    self.pos += 1;
                } else {
                    break;
                }
            }
        }

        fn parse_string(&mut self) -> Option<String> {
            if self.next()? != b'"' {
                return None;
            }
            // JSON text is UTF-8 (RFC 8259 §8.1). The literal-byte
            // path buffers the raw slice between escapes / terminator
            // and validates it as UTF-8 in one shot via
            // `std::str::from_utf8` — pushing one `b as char` per byte
            // would have mis-decoded any multi-byte UTF-8 sequence
            // (each continuation byte landing in its Latin-1
            // codepoint, mangling non-ASCII content). Escapes get
            // appended as already-validated `char`s and reset the
            // literal-start cursor.
            let mut s = String::new();
            let mut lit_start = self.pos;
            loop {
                let b = *self.buf.get(self.pos)?;
                match b {
                    b'"' => {
                        if self.pos > lit_start {
                            s.push_str(std::str::from_utf8(&self.buf[lit_start..self.pos]).ok()?);
                        }
                        self.pos += 1;
                        return Some(s);
                    }
                    b'\\' => {
                        if self.pos > lit_start {
                            s.push_str(std::str::from_utf8(&self.buf[lit_start..self.pos]).ok()?);
                        }
                        self.pos += 1;
                        let esc = self.next()?;
                        match esc {
                            b'"' => s.push('"'),
                            b'\\' => s.push('\\'),
                            b'/' => s.push('/'),
                            b'n' => s.push('\n'),
                            b'r' => s.push('\r'),
                            b't' => s.push('\t'),
                            b'b' => s.push('\u{08}'),
                            b'f' => s.push('\u{0c}'),
                            b'u' => {
                                // Four hex digits → 16-bit code unit.
                                // Non-BMP characters are encoded as a
                                // UTF-16 surrogate pair per RFC 8259
                                // §7 / ECMA-404, e.g. U+1F980 (🦀)
                                // arrives as `🦀`. Detect a
                                // high surrogate, consume the
                                // mandatory `\u` + low surrogate, and
                                // assemble the supplementary codepoint.
                                // Lone surrogates of either kind are
                                // rejected (char::from_u32 wouldn't
                                // accept them anyway, but we surface
                                // the spec violation explicitly).
                                let unit = self.parse_hex4()?;
                                let cp = match unit {
                                    0xD800..=0xDBFF => {
                                        // High surrogate — must be
                                        // followed by `\u` and a low
                                        // surrogate (0xDC00..=0xDFFF).
                                        if self.next()? != b'\\' || self.next()? != b'u' {
                                            return None;
                                        }
                                        let low = self.parse_hex4()?;
                                        if !(0xDC00..=0xDFFF).contains(&low) {
                                            return None;
                                        }
                                        0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00)
                                    }
                                    0xDC00..=0xDFFF => {
                                        // Lone low surrogate.
                                        return None;
                                    }
                                    _ => unit,
                                };
                                s.push(char::from_u32(cp)?);
                            }
                            _ => return None,
                        }
                        lit_start = self.pos;
                    }
                    // Control bytes are not valid in JSON strings; the
                    // spec says we may reject them. Tolerant parsers
                    // accept them inline — we reject to match RFC
                    // 8259 strictly. (UTF-8 continuation bytes are
                    // ≥ 0x80, so this gate doesn't catch them.)
                    b if b < 0x20 => return None,
                    _ => self.pos += 1,
                }
            }
        }

        /// Consume exactly four hex digits and return them as a
        /// 16-bit code unit packed into a u32. Used by
        /// `parse_string`'s `\u` escape branch — refactored out
        /// because the branch handles surrogate pairs and needs to
        /// read TWO 4-digit groups in succession.
        fn parse_hex4(&mut self) -> Option<u32> {
            let mut cp: u32 = 0;
            for _ in 0..4 {
                cp = cp.checked_mul(16)?;
                let h = self.next()?;
                cp += match h {
                    b'0'..=b'9' => (h - b'0') as u32,
                    b'a'..=b'f' => (h - b'a' + 10) as u32,
                    b'A'..=b'F' => (h - b'A' + 10) as u32,
                    _ => return None,
                };
            }
            Some(cp)
        }

        fn parse_uint(&mut self) -> Option<u64> {
            let start = self.pos;
            while let Some(b) = self.peek() {
                if b.is_ascii_digit() {
                    self.pos += 1;
                } else {
                    break;
                }
            }
            if self.pos == start {
                return None;
            }
            // Reject leading zeros except for the single-digit "0"
            // (RFC 8259 §6).
            let digits = &self.buf[start..self.pos];
            if digits.len() > 1 && digits[0] == b'0' {
                return None;
            }
            let mut acc: u64 = 0;
            for &b in digits {
                acc = acc.checked_mul(10)?.checked_add((b - b'0') as u64)?;
            }
            Some(acc)
        }

        /// Skip a JSON value. Used for forward-compat unknown keys.
        fn skip_value(&mut self) -> Option<()> {
            self.skip_ws();
            match self.peek()? {
                b'"' => {
                    self.parse_string()?;
                }
                b'{' => self.skip_object()?,
                b'[' => self.skip_array()?,
                b't' => self.consume_lit(b"true")?,
                b'f' => self.consume_lit(b"false")?,
                b'n' => self.consume_lit(b"null")?,
                b'-' | b'0'..=b'9' => self.skip_number()?,
                _ => return None,
            }
            Some(())
        }

        /// Skip a JSON number per RFC 8259 §6 grammar.
        ///
        /// ```text
        ///   number = [ minus ] int [ frac ] [ exp ]
        ///   int    = zero / ( digit1-9 *DIGIT )
        ///   frac   = decimal-point 1*DIGIT
        ///   exp    = e [ minus / plus ] 1*DIGIT
        /// ```
        ///
        /// We don't need the value — but the structural shape still
        /// matters: an unknown field with a malformed number like `01`
        /// or `1.` or `1e` must reject the whole object, not be
        /// silently glossed over. Earlier revisions skipped a run of
        /// digit/`.`/`e`/`E`/`+`/`-` which accepted invalid input the
        /// `parse` doc page promises to reject (RFC 8259 conformant
        /// input only).
        fn skip_number(&mut self) -> Option<()> {
            // Optional minus.
            if self.peek() == Some(b'-') {
                self.pos += 1;
            }
            // int: either single 0, or digit1-9 followed by 0+ DIGITs.
            match self.peek()? {
                b'0' => {
                    self.pos += 1;
                }
                b'1'..=b'9' => {
                    self.pos += 1;
                    while matches!(self.peek(), Some(b'0'..=b'9')) {
                        self.pos += 1;
                    }
                }
                _ => return None,
            }
            // Optional fraction: '.' DIGIT+.
            if self.peek() == Some(b'.') {
                self.pos += 1;
                if !matches!(self.peek(), Some(b'0'..=b'9')) {
                    return None;
                }
                while matches!(self.peek(), Some(b'0'..=b'9')) {
                    self.pos += 1;
                }
            }
            // Optional exponent: e/E ('+'|'-')? DIGIT+.
            if matches!(self.peek(), Some(b'e') | Some(b'E')) {
                self.pos += 1;
                if matches!(self.peek(), Some(b'+') | Some(b'-')) {
                    self.pos += 1;
                }
                if !matches!(self.peek(), Some(b'0'..=b'9')) {
                    return None;
                }
                while matches!(self.peek(), Some(b'0'..=b'9')) {
                    self.pos += 1;
                }
            }
            Some(())
        }

        /// Skip a JSON object per RFC 8259 §4 grammar.
        ///
        /// ```text
        ///   object = begin-object [ member *( value-separator member ) ] end-object
        ///   member = string name-separator value
        /// ```
        ///
        /// The earlier revision counted balanced braces only — it
        /// accepted arbitrary bytes between them, which would let
        /// `{"extra":{1abc}}` slide through. That contradicted the
        /// module's stated "RFC 8259-conformant input only" policy.
        /// The walker now enforces the grammar: each member is
        /// `string ws : ws value`, and adjacent members are
        /// separated by `ws , ws`. Inner values are delegated to
        /// `skip_value`, which recurses into nested objects /
        /// arrays — so malformed JSON at any depth fails the parse
        /// closed.
        fn skip_object(&mut self) -> Option<()> {
            if self.next()? != b'{' {
                return None;
            }
            self.skip_ws();
            // Empty object: `{}` (with optional ws between).
            if self.peek() == Some(b'}') {
                self.pos += 1;
                return Some(());
            }
            loop {
                // member = string name-separator value
                self.skip_ws();
                if self.peek() != Some(b'"') {
                    return None;
                }
                self.parse_string()?;
                self.skip_ws();
                if self.next()? != b':' {
                    return None;
                }
                // skip_value handles its own leading ws.
                self.skip_value()?;
                self.skip_ws();
                match self.next()? {
                    b',' => continue,
                    b'}' => return Some(()),
                    _ => return None,
                }
            }
        }

        /// Skip a JSON array per RFC 8259 §5 grammar.
        ///
        /// ```text
        ///   array = begin-array [ value *( value-separator value ) ] end-array
        /// ```
        ///
        /// Same shape as `skip_object`: enforce the value/separator
        /// alternation rather than counting balanced brackets.
        fn skip_array(&mut self) -> Option<()> {
            if self.next()? != b'[' {
                return None;
            }
            self.skip_ws();
            // Empty array: `[]` (with optional ws between).
            if self.peek() == Some(b']') {
                self.pos += 1;
                return Some(());
            }
            loop {
                // skip_value handles its own leading ws.
                self.skip_value()?;
                self.skip_ws();
                match self.next()? {
                    b',' => continue,
                    b']' => return Some(()),
                    _ => return None,
                }
            }
        }

        fn consume_lit(&mut self, lit: &[u8]) -> Option<()> {
            if self.pos + lit.len() > self.buf.len() {
                return None;
            }
            if &self.buf[self.pos..self.pos + lit.len()] != lit {
                return None;
            }
            self.pos += lit.len();
            Some(())
        }
    }
}

// ---- JOIN-reply / 602 / 605 chunk-walk extractors ---------------------

/// Distilled `HTLC_HDR_VOICE_JOIN` reply (or 602 / 605 server
/// notification, which share most fields).
///
/// All fields default to "absent" when the matching chunk wasn't on
/// the wire. The fixed-shape fields (cid) parse as u32 BE; the
/// variable-length payloads (sdp, ice, codec, participants) are
/// borrowed slices into the caller's message buffer and stay valid
/// only for the lifetime of the parse call. The C-side FFI shim
/// copies them out before the parse call returns.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct VoiceReply<'a> {
    pub cid: u32,
    pub sdp: Option<&'a [u8]>,
    pub ice: Option<&'a [u8]>,
    pub codec: Option<&'a [u8]>,
    pub muted: Option<u16>,
    pub participants: Option<&'a [u8]>,
}

/// Parse a voice-reply / -notification body. Walks the chunks once,
/// populating every voice field present. Unknown chunk tags are
/// ignored (per the rest of `parse::*`).
pub fn parse_voice_reply(buf: &[u8], len: usize) -> VoiceReply<'_> {
    let mut out = VoiceReply::default();
    for chunk in ChunkIter::over_message(buf, len) {
        match chunk.tag {
            tag::CHAT_ID => out.cid = chunk.as_uint(),
            tag::VOICE_SDP => out.sdp = Some(chunk.data),
            tag::VOICE_ICE => out.ice = Some(chunk.data),
            tag::VOICE_CODEC => out.codec = Some(chunk.data),
            tag::VOICE_MUTED => out.muted = Some(chunk.as_uint() as u16),
            tag::VOICE_PARTICIPANTS => out.participants = Some(chunk.data),
            _ => {}
        }
    }
    out
}

/// Convenience: the same chunk-walk, but returns the variant the JOIN
/// reply uses (`sdp`, `codec`, `participants` all expected). Provided
/// so the C-side dispatcher can document its expectation at the call
/// site even though the parse implementation is the same.
pub fn parse_voice_join_reply(buf: &[u8], len: usize) -> VoiceReply<'_> {
    parse_voice_reply(buf, len)
}

// ---- Tests ------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    /// Compose a fake message body around `payload` for the chunk-walk
    /// helpers: the 22-byte transaction header followed by `hc` raw
    /// payload bytes. Mirrors what `htlc->in.buf` looks like at the
    /// parse call site.
    fn frame(payload: &[u8]) -> Vec<u8> {
        let mut v = vec![0u8; crate::HL_HDR_LEN];
        v.extend_from_slice(payload);
        v
    }

    fn chunk(tag: u16, data: &[u8]) -> Vec<u8> {
        let mut v = Vec::with_capacity(4 + data.len());
        v.extend_from_slice(&tag.to_be_bytes());
        v.extend_from_slice(&(data.len() as u16).to_be_bytes());
        v.extend_from_slice(data);
        v
    }

    // ---- Builders ----

    #[test]
    fn voice_join_builds_chat_id_only() {
        let mut chunks = [HxChunk::EMPTY; 2];
        let mut scratch = [0u8; 16];
        let hc = build_voice_join_chunks(42, &mut chunks, &mut scratch);
        assert_eq!(hc, 1);
        assert_eq!(chunks[0].tag, tag::CHAT_ID);
        assert_eq!(chunks[0].len, 4);
        // Read back the scratch bytes through the chunk pointer.
        unsafe {
            let s = std::slice::from_raw_parts(chunks[0].data, 4);
            assert_eq!(s, &42u32.to_be_bytes());
        }
    }

    #[test]
    fn voice_join_rejects_tiny_buffers() {
        let mut chunks: [HxChunk; 0] = [];
        let mut scratch = [0u8; 4];
        assert_eq!(build_voice_join_chunks(1, &mut chunks, &mut scratch), 0);

        let mut chunks = [HxChunk::EMPTY; 1];
        let mut scratch = [0u8; 3];
        assert_eq!(build_voice_join_chunks(1, &mut chunks, &mut scratch), 0);
    }

    #[test]
    fn voice_leave_builds_chat_id_only() {
        let mut chunks = [HxChunk::EMPTY; 2];
        let mut scratch = [0u8; 16];
        let hc = build_voice_leave_chunks(0, &mut chunks, &mut scratch);
        assert_eq!(hc, 1);
        assert_eq!(chunks[0].tag, tag::CHAT_ID);
        assert_eq!(chunks[0].len, 4);
    }

    #[test]
    fn voice_answer_builds_two_chunks() {
        let mut chunks = [HxChunk::EMPTY; 3];
        let mut scratch = [0u8; 16];
        let sdp = b"v=0\r\no=-\r\n";
        let hc = build_voice_answer_chunks(7, sdp, &mut chunks, &mut scratch);
        assert_eq!(hc, 2);
        assert_eq!(chunks[0].tag, tag::CHAT_ID);
        assert_eq!(chunks[1].tag, tag::VOICE_SDP);
        assert_eq!(chunks[1].len as usize, sdp.len());
        unsafe {
            let s = std::slice::from_raw_parts(chunks[1].data, sdp.len());
            assert_eq!(s, sdp);
        }
    }

    #[test]
    fn voice_answer_rejects_empty_sdp() {
        let mut chunks = [HxChunk::EMPTY; 3];
        let mut scratch = [0u8; 16];
        // An empty SDP answer would tell the server we accept nothing
        // — silently corrupt the session. Reject at the builder.
        assert_eq!(
            build_voice_answer_chunks(7, b"", &mut chunks, &mut scratch),
            0
        );
    }

    #[test]
    fn voice_ice_accepts_empty_end_of_candidates() {
        // End-of-candidates marker per spec: empty VOICE_ICE field.
        let mut chunks = [HxChunk::EMPTY; 3];
        let mut scratch = [0u8; 16];
        let hc = build_voice_ice_chunks(7, b"", &mut chunks, &mut scratch);
        assert_eq!(hc, 2);
        assert_eq!(chunks[1].tag, tag::VOICE_ICE);
        assert_eq!(chunks[1].len, 0);
    }

    #[test]
    fn voice_ice_builds_with_payload() {
        let mut chunks = [HxChunk::EMPTY; 3];
        let mut scratch = [0u8; 16];
        let ice = br#"{"candidate":"candidate:1 1 UDP 1 192.0.2.1 5004 typ host","sdpMid":"send"}"#;
        let hc = build_voice_ice_chunks(7, ice, &mut chunks, &mut scratch);
        assert_eq!(hc, 2);
        assert_eq!(chunks[1].len as usize, ice.len());
    }

    #[test]
    fn voice_mute_emits_chat_id_then_muted() {
        let mut chunks = [HxChunk::EMPTY; 3];
        let mut scratch = [0u8; 16];
        let hc = build_voice_mute_chunks(42, 1, &mut chunks, &mut scratch);
        assert_eq!(hc, 2);
        assert_eq!(chunks[0].tag, tag::CHAT_ID);
        assert_eq!(chunks[0].len, 4);
        assert_eq!(chunks[1].tag, tag::VOICE_MUTED);
        assert_eq!(chunks[1].len, 2);
        unsafe {
            let s = std::slice::from_raw_parts(chunks[1].data, 2);
            assert_eq!(s, &1u16.to_be_bytes());
        }
    }

    // ---- Participant blob walker ----

    #[test]
    fn participants_walker_decodes_packed_entries() {
        // Three participants: uids 5, 12, 23 — 5 muted, 12 unmuted with
        // PCMU, 23 unmuted with PCMU.
        let mut blob = Vec::new();
        for (uid, flags, codec) in [(5u16, 1u16, 0u16), (12, 0, 0), (23, 0, 0)] {
            blob.extend_from_slice(&uid.to_be_bytes());
            blob.extend_from_slice(&flags.to_be_bytes());
            blob.extend_from_slice(&codec.to_be_bytes());
        }
        let v: Vec<Participant> = parse_voice_participants(&blob).collect();
        assert_eq!(v.len(), 3);
        assert_eq!(
            v[0],
            Participant {
                user_id: 5,
                flags: 1,
                codec_id: 0
            }
        );
        assert!(v[0].is_muted());
        assert!(!v[1].is_muted());
        assert_eq!(v[2].user_id, 23);
    }

    #[test]
    fn participants_walker_drops_trailing_partial() {
        // 6 + 4 bytes — the partial entry is discarded.
        let mut blob = vec![0u8; 6];
        blob.extend_from_slice(&[0, 5, 0, 0]);
        let v: Vec<Participant> = parse_voice_participants(&blob).collect();
        assert_eq!(v.len(), 1);
    }

    #[test]
    fn participants_walker_empty() {
        let v: Vec<Participant> = parse_voice_participants(&[]).collect();
        assert!(v.is_empty());
    }

    // ---- mid label parsing ----

    #[test]
    fn mid_label_send_and_user() {
        assert_eq!(parse_voice_mid_label(b"send"), Some(MidLabel::Send));
        assert_eq!(parse_voice_mid_label(b"user-1"), Some(MidLabel::User(1)));
        assert_eq!(
            parse_voice_mid_label(b"user-12345"),
            Some(MidLabel::User(12345))
        );
        assert_eq!(
            parse_voice_mid_label(b"user-65535"),
            Some(MidLabel::User(65535))
        );
    }

    #[test]
    fn mid_label_rejects_spec_violations() {
        // Leading zero.
        assert_eq!(parse_voice_mid_label(b"user-05"), None);
        // UID 0 reserved.
        assert_eq!(parse_voice_mid_label(b"user-0"), None);
        // Overflow.
        assert_eq!(parse_voice_mid_label(b"user-65536"), None);
        assert_eq!(parse_voice_mid_label(b"user-99999"), None);
        // No prefix.
        assert_eq!(parse_voice_mid_label(b"send "), None);
        assert_eq!(parse_voice_mid_label(b"user-12 "), None);
        // Empty.
        assert_eq!(parse_voice_mid_label(b""), None);
        assert_eq!(parse_voice_mid_label(b"user-"), None);
        // Non-digit.
        assert_eq!(parse_voice_mid_label(b"user-1a"), None);
    }

    // ---- SDP summary ----

    #[test]
    fn sdp_summary_extracts_mids_and_bundle() {
        let sdp = "v=0\r\n\
                   o=- 1234567890 1 IN IP4 0.0.0.0\r\n\
                   a=group:BUNDLE user-12 user-23 send\r\n\
                   m=audio 9 UDP/TLS/RTP/SAVPF 0\r\n\
                   a=mid:user-12\r\n\
                   a=rtpmap:0 PCMU/8000\r\n\
                   m=audio 9 UDP/TLS/RTP/SAVPF 0\r\n\
                   a=mid:user-23\r\n\
                   m=audio 9 UDP/TLS/RTP/SAVPF 0\r\n\
                   a=mid:send\r\n";
        let s = sdp::summarize(sdp.as_bytes());
        assert_eq!(
            s.mids,
            vec![MidLabel::User(12), MidLabel::User(23), MidLabel::Send]
        );
        assert!(s.unknown_mids.is_empty());
        assert_eq!(s.bundle, vec!["user-12", "user-23", "send"]);
        assert!(s.has_pcmu);
        assert!(!s.has_disabled_slot);
    }

    #[test]
    fn sdp_summary_detects_disabled_slot() {
        let sdp = "v=0\nm=audio 0 UDP/TLS/RTP/SAVPF 0\na=mid:user-23\n";
        let s = sdp::summarize(sdp.as_bytes());
        assert!(s.has_disabled_slot);
        assert!(!s.has_pcmu);
    }

    #[test]
    fn sdp_summary_collects_unknown_mids() {
        let sdp = "a=mid:foo\na=mid:user-0\na=mid:user-05\n";
        let s = sdp::summarize(sdp.as_bytes());
        assert!(s.mids.is_empty());
        // user-0 and user-05 are both spec violations; foo is unknown.
        assert_eq!(s.unknown_mids.len(), 3);
    }

    // ---- ICE JSON ----

    #[test]
    fn ice_build_full_dict() {
        let c = ice::IceCandidate {
            candidate: Some("candidate:1 1 UDP 2130706431 192.0.2.1 5004 typ host".to_string()),
            sdp_mid: Some("0".to_string()),
            sdp_mline_index: Some(0),
            username_fragment: Some("abc123".to_string()),
        };
        let s = ice::build(&c);
        // The build path is order-stable; pin the exact output.
        let expected = "{\"candidate\":\"candidate:1 1 UDP 2130706431 192.0.2.1 5004 typ host\",\
                        \"sdpMid\":\"0\",\
                        \"sdpMLineIndex\":0,\
                        \"usernameFragment\":\"abc123\"}";
        assert_eq!(s, expected);
    }

    #[test]
    fn ice_build_end_of_candidates_marker() {
        let c = ice::IceCandidate {
            candidate: Some("".to_string()),
            sdp_mid: Some("send".to_string()),
            sdp_mline_index: Some(0),
            ..Default::default()
        };
        let s = ice::build(&c);
        assert!(s.starts_with("{\"candidate\":\"\","));
        assert!(s.ends_with("\"sdpMLineIndex\":0}"));
    }

    #[test]
    fn ice_parse_roundtrip() {
        let c = ice::IceCandidate {
            candidate: Some("candidate:1 1 UDP 1 1.2.3.4 5004 typ host".to_string()),
            sdp_mid: Some("send".to_string()),
            sdp_mline_index: Some(2),
            username_fragment: Some("xyz".to_string()),
        };
        let s = ice::build(&c);
        let parsed = ice::parse(s.as_bytes()).expect("parse");
        assert_eq!(parsed, c);
    }

    #[test]
    fn ice_parse_partial_dict() {
        let json = br#"{"candidate":"","sdpMid":"send","sdpMLineIndex":0}"#;
        let c = ice::parse(json).expect("parse");
        assert!(c.is_end_of_candidates());
        assert_eq!(c.sdp_mid.as_deref(), Some("send"));
        assert_eq!(c.sdp_mline_index, Some(0));
        assert!(c.username_fragment.is_none());
    }

    #[test]
    fn ice_parse_unknown_fields_skipped() {
        let json = br#"{"candidate":"c","extraKey":"ignore","sdpMid":"send"}"#;
        let c = ice::parse(json).expect("parse");
        assert_eq!(c.candidate.as_deref(), Some("c"));
        assert_eq!(c.sdp_mid.as_deref(), Some("send"));
    }

    #[test]
    fn ice_parse_rejects_garbage() {
        assert!(ice::parse(b"").is_none());
        assert!(ice::parse(b"[]").is_none());
        assert!(ice::parse(b"{").is_none());
        // Value not a string (and missing sdpMid — either rejection
        // is sufficient).
        assert!(ice::parse(br#"{"candidate":1}"#).is_none());
        // Leading zero on number (and missing required keys).
        assert!(ice::parse(br#"{"sdpMLineIndex":01}"#).is_none());
        // Trailing garbage.
        assert!(ice::parse(br#"{"candidate":"c","sdpMid":"send"} junk"#).is_none());
    }

    /// Regression (Copilot review): `ice::parse` used to accept
    /// payloads that omitted required `candidate` / `sdpMid` keys.
    /// Per fogWraith Capabilities-Voice.md §"ICE Candidate Format",
    /// both are required on every non-shorthand DATA_VOICE_ICE
    /// payload; accepting a partial object would surface as a
    /// "successful" parse with empty fields, which the runtime would
    /// then try to feed to `webrtcbin.add-ice-candidate` and fail
    /// downstream without the diagnostic that the wire payload was
    /// malformed.
    #[test]
    fn ice_parse_rejects_missing_required_keys() {
        // Empty object — both required keys missing.
        assert!(ice::parse(br#"{}"#).is_none());
        // Only sdpMid.
        assert!(ice::parse(br#"{"sdpMid":"send"}"#).is_none());
        // Only candidate.
        assert!(ice::parse(br#"{"candidate":"c"}"#).is_none());
        // candidate missing, optional fields present.
        assert!(ice::parse(br#"{"sdpMid":"send","sdpMLineIndex":0}"#).is_none());
        // sdpMid missing, optional fields present.
        assert!(
            ice::parse(br#"{"candidate":"c","sdpMLineIndex":0,"usernameFragment":"x"}"#).is_none()
        );
        // Both keys present even with optional whitespace / order
        // variations succeed — pinned alongside as a positive
        // control so the strict check doesn't over-reject.
        assert!(ice::parse(br#"{ "candidate" : "" , "sdpMid" : "send" }"#).is_some());
        // Empty candidate (end-of-candidates) is valid as long as
        // sdpMid is present.
        let eoc = ice::parse(br#"{"candidate":"","sdpMid":"send","sdpMLineIndex":0}"#)
            .expect("EOC with sdpMid should parse");
        assert!(eoc.is_end_of_candidates());
        assert_eq!(eoc.sdp_mid.as_deref(), Some("send"));
    }

    #[test]
    fn ice_build_escapes_metacharacters() {
        // sdp_mid is required by parse(), so include it in the
        // round-trip — the assert below is about escape correctness,
        // not the wire-conformance gate.
        let c = ice::IceCandidate {
            candidate: Some("with \"quotes\" and \\slashes\\".to_string()),
            sdp_mid: Some("send".to_string()),
            ..Default::default()
        };
        let s = ice::build(&c);
        assert!(s.contains(r#"\"quotes\""#));
        assert!(s.contains(r#"\\slashes\\"#));
        // Round-trip — parse should recover the original.
        let parsed = ice::parse(s.as_bytes()).expect("parse");
        assert_eq!(
            parsed.candidate.as_deref(),
            Some("with \"quotes\" and \\slashes\\")
        );
        assert_eq!(parsed.sdp_mid.as_deref(), Some("send"));
    }

    /// Regression: an unknown JSON field with a malformed RFC 8259
    /// number (`01`, `1.`, `1e`, bare `-`, etc.) used to be silently
    /// accepted by the skip_value branch — a `peek-and-eat`
    /// digit/`.`/exp blob that didn't enforce the spec's number
    /// grammar. Now skip_number walks the grammar properly and the
    /// whole parse fails closed on any of these.
    #[test]
    fn ice_parse_rejects_malformed_number_in_skipped_field() {
        // Leading zero (skipped field).
        assert!(ice::parse(br#"{"extra":01,"candidate":"c","sdpMid":"send"}"#).is_none());
        // Bare minus.
        assert!(ice::parse(br#"{"extra":-,"candidate":"c","sdpMid":"send"}"#).is_none());
        // Minus with no integer following.
        assert!(ice::parse(br#"{"extra":-.5,"candidate":"c","sdpMid":"send"}"#).is_none());
        // Fraction with no following digit.
        assert!(ice::parse(br#"{"extra":1.,"candidate":"c","sdpMid":"send"}"#).is_none());
        // Exponent with no following digit.
        assert!(ice::parse(br#"{"extra":1e,"candidate":"c","sdpMid":"send"}"#).is_none());
        // Exponent with sign but no digit.
        assert!(ice::parse(br#"{"extra":1e+,"candidate":"c","sdpMid":"send"}"#).is_none());
    }

    /// skip_value still accepts valid numbers — pin the legal-shape
    /// cases so the tightened grammar doesn't over-reject.
    #[test]
    fn ice_parse_accepts_valid_numbers_in_skipped_field() {
        // Each body satisfies the spec's required-keys policy
        // (candidate + sdpMid) so we exercise the skip-number path
        // in isolation, not the required-field rejection.
        for body in [
            br#"{"extra":0,"candidate":"c","sdpMid":"send"}"#.as_slice(),
            br#"{"extra":1,"candidate":"c","sdpMid":"send"}"#.as_slice(),
            br#"{"extra":-1,"candidate":"c","sdpMid":"send"}"#.as_slice(),
            br#"{"extra":1.5,"candidate":"c","sdpMid":"send"}"#.as_slice(),
            br#"{"extra":-0.25,"candidate":"c","sdpMid":"send"}"#.as_slice(),
            br#"{"extra":1e10,"candidate":"c","sdpMid":"send"}"#.as_slice(),
            br#"{"extra":1E10,"candidate":"c","sdpMid":"send"}"#.as_slice(),
            br#"{"extra":1.5e-3,"candidate":"c","sdpMid":"send"}"#.as_slice(),
            br#"{"extra":-1.5E+3,"candidate":"c","sdpMid":"send"}"#.as_slice(),
        ] {
            let parsed = ice::parse(body).unwrap_or_else(|| {
                panic!(
                    "parse should accept legal JSON: {}",
                    String::from_utf8_lossy(body)
                )
            });
            assert_eq!(parsed.candidate.as_deref(), Some("c"));
        }
    }

    /// Regression: parse_string used to `s.push(b as char)` for every
    /// literal byte, which treated each input byte as a Unicode
    /// scalar — multi-byte UTF-8 sequences arrived as a run of
    /// Latin-1 codepoints, mangling any non-ASCII content. JSON
    /// text is required to be UTF-8 (RFC 8259 §8.1), so the parser
    /// now buffers the literal slice and decodes it as UTF-8 in one
    /// shot. Pin a round-trip through every common non-ASCII shape.
    #[test]
    fn ice_parse_roundtrips_utf8_strings() {
        // Two-byte sequence (Latin-1 supplement), three-byte (BMP),
        // four-byte (supplementary plane / emoji).
        for s in &[
            "café",                // two-byte é (0xC3 0xA9)
            "münch",               // umlaut
            "日本語",              // three-byte CJK
            "🦀 Rust",             // four-byte emoji (U+1F980)
            "mix: café 日本語 🦀", // all together
            "user-12345",          // pure ASCII still works
        ] {
            let c = ice::IceCandidate {
                candidate: Some((*s).to_string()),
                sdp_mid: Some("send".to_string()),
                ..Default::default()
            };
            let json = ice::build(&c);
            let parsed =
                ice::parse(json.as_bytes()).unwrap_or_else(|| panic!("parse failed for: {s:?}"));
            assert_eq!(
                parsed.candidate.as_deref(),
                Some(*s),
                "round-trip mismatch for {s:?}"
            );
        }
    }

    /// Regression: skip_object used to count balanced braces only
    /// — it accepted arbitrary bytes inside, so malformed JSON
    /// inside an unknown object-typed field would slip through. The
    /// strict grammar walker now enforces `member (, member)*`
    /// with `member = string : value`, and inner values recurse
    /// into skip_value (so nested malformed JSON fails closed at
    /// any depth).
    #[test]
    fn ice_parse_rejects_malformed_object_in_skipped_field() {
        // Each body includes the required candidate + sdpMid so the
        // rejection under test is the malformed-inner-object path,
        // not the required-fields gate.
        // Missing colon.
        assert!(ice::parse(br#"{"extra":{"k" 1},"candidate":"c","sdpMid":"send"}"#).is_none());
        // Bare bytes between members.
        assert!(
            ice::parse(br#"{"extra":{"k":1 garbage},"candidate":"c","sdpMid":"send"}"#).is_none()
        );
        // Unquoted key.
        assert!(ice::parse(br#"{"extra":{k:1},"candidate":"c","sdpMid":"send"}"#).is_none());
        // Trailing comma after member.
        assert!(ice::parse(br#"{"extra":{"k":1,},"candidate":"c","sdpMid":"send"}"#).is_none());
        // Nested object with malformed inner number — recursion
        // must catch it.
        assert!(
            ice::parse(br#"{"extra":{"k":{"inner":01}},"candidate":"c","sdpMid":"send"}"#)
                .is_none()
        );
        // Nested object that drops the closing brace.
        assert!(ice::parse(br#"{"extra":{"k":1,"candidate":"c","sdpMid":"send"}"#).is_none());
    }

    /// skip_object still accepts valid object shapes — pin the
    /// legal cases so the strict walker doesn't over-reject.
    #[test]
    fn ice_parse_accepts_valid_objects_in_skipped_field() {
        // Each body carries the required candidate + sdpMid keys so
        // the assert exercises the skip-object grammar in isolation,
        // not the required-field rejection added alongside.
        for body in [
            br#"{"extra":{},"candidate":"c","sdpMid":"send"}"#.as_slice(),
            br#"{"extra":{"k":1},"candidate":"c","sdpMid":"send"}"#.as_slice(),
            br#"{"extra":{"k":1,"j":2},"candidate":"c","sdpMid":"send"}"#.as_slice(),
            br#"{"extra":{ "k" : 1 , "j" : 2 },"candidate":"c","sdpMid":"send"}"#.as_slice(),
            br#"{"extra":{"k":{"nested":true}},"candidate":"c","sdpMid":"send"}"#.as_slice(),
            br#"{"extra":{"k":[1,2,3]},"candidate":"c","sdpMid":"send"}"#.as_slice(),
        ] {
            let parsed = ice::parse(body).unwrap_or_else(|| {
                panic!(
                    "parse should accept legal JSON: {}",
                    String::from_utf8_lossy(body)
                )
            });
            assert_eq!(parsed.candidate.as_deref(), Some("c"));
        }
    }

    /// Regression: skip_array used to count balanced brackets only,
    /// with the same shortcoming. The strict walker now enforces
    /// `value (, value)*`.
    #[test]
    fn ice_parse_rejects_malformed_array_in_skipped_field() {
        // Each body includes the required candidate + sdpMid so the
        // rejection under test is the malformed-inner-array path.
        // Trailing comma.
        assert!(ice::parse(br#"{"extra":[1,2,],"candidate":"c","sdpMid":"send"}"#).is_none());
        // Two adjacent values with no separator.
        assert!(ice::parse(br#"{"extra":[1 2],"candidate":"c","sdpMid":"send"}"#).is_none());
        // Stray separator after open.
        assert!(ice::parse(br#"{"extra":[,1],"candidate":"c","sdpMid":"send"}"#).is_none());
        // Nested object with malformed number — recursion must
        // catch it.
        assert!(ice::parse(br#"{"extra":[{"k":01}],"candidate":"c","sdpMid":"send"}"#).is_none());
        // Unclosed array.
        assert!(ice::parse(br#"{"extra":[1,2,"candidate":"c","sdpMid":"send"}"#).is_none());
    }

    /// skip_array still accepts valid array shapes.
    #[test]
    fn ice_parse_accepts_valid_arrays_in_skipped_field() {
        // Each body carries the required candidate + sdpMid keys —
        // same rationale as ice_parse_accepts_valid_objects_in_skipped_field.
        for body in [
            br#"{"extra":[],"candidate":"c","sdpMid":"send"}"#.as_slice(),
            br#"{"extra":[1],"candidate":"c","sdpMid":"send"}"#.as_slice(),
            br#"{"extra":[1,2,3],"candidate":"c","sdpMid":"send"}"#.as_slice(),
            br#"{"extra":[ 1 , 2 , 3 ],"candidate":"c","sdpMid":"send"}"#.as_slice(),
            br#"{"extra":[true,false,null],"candidate":"c","sdpMid":"send"}"#.as_slice(),
            br#"{"extra":["a","b"],"candidate":"c","sdpMid":"send"}"#.as_slice(),
            br#"{"extra":[[1,2],[3,4]],"candidate":"c","sdpMid":"send"}"#.as_slice(),
            br#"{"extra":[{"k":1}],"candidate":"c","sdpMid":"send"}"#.as_slice(),
        ] {
            let parsed = ice::parse(body).unwrap_or_else(|| {
                panic!(
                    "parse should accept legal JSON: {}",
                    String::from_utf8_lossy(body)
                )
            });
            assert_eq!(parsed.candidate.as_deref(), Some("c"));
        }
    }

    /// Regression (Copilot review): `\uXXXX` escapes used to convert
    /// the 16-bit code unit directly via `char::from_u32`, rejecting
    /// any valid RFC 8259 input that encoded a non-BMP character as
    /// a UTF-16 surrogate pair (high D800-DBFF + low DC00-DFFF). Now
    /// the parser detects the high half, consumes the mandatory
    /// `\u` + low half, and assembles the supplementary codepoint.
    /// Both BMP escapes and surrogate pairs round-trip via
    /// `from_u32` to their expected Rust `char`.
    #[test]
    fn ice_parse_accepts_utf16_surrogate_pairs() {
        // 🦀 = U+1F980 = 🦀.
        let json = r#"{"candidate":"crab 🦀 here","sdpMid":"send"}"#;
        let parsed = ice::parse(json.as_bytes()).expect("surrogate pair should decode");
        assert_eq!(parsed.candidate.as_deref(), Some("crab \u{1F980} here"));

        // 𠮷 (U+20BB7, supplementary plane) = 𠮷.
        let json2 = r#"{"candidate":"𠮷","sdpMid":"send"}"#;
        let parsed2 = ice::parse(json2.as_bytes()).expect("surrogate pair should decode");
        assert_eq!(parsed2.candidate.as_deref(), Some("\u{20BB7}"));

        // BMP escapes still work (regression for the refactor).
        // é = é.
        let bmp = r#"{"candidate":"café","sdpMid":"send"}"#;
        let parsed_bmp = ice::parse(bmp.as_bytes()).expect("BMP \\u should still decode");
        assert_eq!(parsed_bmp.candidate.as_deref(), Some("café"));

        // ASCII-range \u escape (A = 'A').
        let ascii = r#"{"candidate":"A","sdpMid":"send"}"#;
        let parsed_ascii = ice::parse(ascii.as_bytes()).expect("ASCII \\u should decode");
        assert_eq!(parsed_ascii.candidate.as_deref(), Some("A"));
    }

    /// Lone surrogates (high without a following low, low without a
    /// preceding high, or high followed by non-surrogate escape) are
    /// invalid per RFC 8259 / ECMA-404; the parser rejects them.
    #[test]
    fn ice_parse_rejects_lone_surrogates() {
        // Lone high surrogate at end of string.
        assert!(ice::parse(br#"{"candidate":"\uD83E","sdpMid":"send"}"#).is_none());

        // Lone high surrogate followed by a regular character (no
        // \u trailer).
        assert!(ice::parse(br#"{"candidate":"\uD83Ex","sdpMid":"send"}"#).is_none());

        // High surrogate followed by a \u escape that isn't a low
        // surrogate (A = 'A').
        assert!(ice::parse(br#"{"candidate":"\uD83EA","sdpMid":"send"}"#).is_none());

        // Lone low surrogate.
        assert!(ice::parse(br#"{"candidate":"\uDD80","sdpMid":"send"}"#).is_none());

        // High surrogate followed by a non-\u escape ('\\n' is legal
        // JSON in isolation but invalid as the low half of a
        // surrogate pair).
        assert!(ice::parse(br#"{"candidate":"\uD83E\n","sdpMid":"send"}"#).is_none());
    }

    /// Invalid UTF-8 (lone continuation byte / truncated 2-byte
    /// sequence) inside a JSON string fails the parse. Pinned so
    /// the UTF-8 validation path stays load-bearing.
    #[test]
    fn ice_parse_rejects_invalid_utf8_in_string() {
        // 0x80 is a lone continuation byte — never legal at the
        // start of a UTF-8 codepoint.
        let bad = b"{\"candidate\":\"abc\x80def\"}";
        assert!(ice::parse(bad).is_none());

        // 0xC3 is the lead byte of a 2-byte sequence; without a
        // continuation byte (0x80-0xBF) following, the sequence is
        // truncated.
        let truncated = b"{\"candidate\":\"abc\xC3\"}";
        assert!(ice::parse(truncated).is_none());
    }

    // ---- VoiceReply chunk-walk ----

    #[test]
    fn voice_reply_extracts_join_reply_shape() {
        // The shape the spec describes for the JOIN reply: cid + SDP +
        // codec name + participants.
        let mut body = Vec::new();
        body.extend(chunk(tag::CHAT_ID, &42u32.to_be_bytes()));
        body.extend(chunk(tag::VOICE_SDP, b"v=0\r\n"));
        body.extend(chunk(tag::VOICE_CODEC, b"PCMU"));
        let mut parts = Vec::new();
        parts.extend_from_slice(&5u16.to_be_bytes());
        parts.extend_from_slice(&0u16.to_be_bytes());
        parts.extend_from_slice(&0u16.to_be_bytes());
        body.extend(chunk(tag::VOICE_PARTICIPANTS, &parts));

        let buf = frame(&body);
        let r = parse_voice_join_reply(&buf, buf.len());
        assert_eq!(r.cid, 42);
        assert_eq!(r.sdp, Some(&b"v=0\r\n"[..]));
        assert_eq!(r.codec, Some(&b"PCMU"[..]));
        assert!(r.participants.is_some());
        let v: Vec<Participant> = parse_voice_participants(r.participants.unwrap()).collect();
        assert_eq!(v.len(), 1);
        assert_eq!(v[0].user_id, 5);
    }

    #[test]
    fn voice_reply_extracts_602_offer_shape() {
        // 602 server notification: just cid + SDP.
        let mut body = Vec::new();
        body.extend(chunk(tag::CHAT_ID, &7u32.to_be_bytes()));
        body.extend(chunk(tag::VOICE_SDP, b"sdp body"));
        let buf = frame(&body);
        let r = parse_voice_reply(&buf, buf.len());
        assert_eq!(r.cid, 7);
        assert_eq!(r.sdp, Some(&b"sdp body"[..]));
        assert!(r.codec.is_none());
        assert!(r.participants.is_none());
    }

    #[test]
    fn voice_reply_extracts_604_ice_shape() {
        let mut body = Vec::new();
        body.extend(chunk(tag::CHAT_ID, &7u32.to_be_bytes()));
        body.extend(chunk(tag::VOICE_ICE, b"{\"candidate\":\"\"}"));
        let buf = frame(&body);
        let r = parse_voice_reply(&buf, buf.len());
        assert_eq!(r.cid, 7);
        assert_eq!(r.ice, Some(&b"{\"candidate\":\"\"}"[..]));
    }

    #[test]
    fn voice_reply_extracts_mute_shape() {
        let mut body = Vec::new();
        body.extend(chunk(tag::CHAT_ID, &7u32.to_be_bytes()));
        body.extend(chunk(tag::VOICE_MUTED, &1u16.to_be_bytes()));
        let buf = frame(&body);
        let r = parse_voice_reply(&buf, buf.len());
        assert_eq!(r.muted, Some(1));
    }

    #[test]
    fn voice_reply_empty_body() {
        // Empty reply body (the spec's "success reply, no fields"
        // shape for 601 / 603 / 606): every field None / 0.
        let buf = frame(&[]);
        let r = parse_voice_reply(&buf, buf.len());
        assert_eq!(r.cid, 0);
        assert!(r.sdp.is_none());
        assert!(r.ice.is_none());
        assert!(r.codec.is_none());
        assert!(r.muted.is_none());
        assert!(r.participants.is_none());
    }
}
