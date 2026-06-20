//! HOPE-aware Blowfish-OFB-64 stream cipher adapter.
//!
//! [`HopeBlowfishStream`] wraps an `AsyncRead + AsyncWrite` inner
//! stream and runs the legacy HOPE per-message rekey protocol on
//! both directions:
//!
//! - **Read side.** Decode 22 bytes (one Hotline header), inspect
//!   the high byte of the type field. If non-zero, that's the
//!   rekey marker — N HMAC iterations of the read key against
//!   the session key. Rotate the Blowfish schedule by those N
//!   iterations (keeping ivec/num untouched — OFB position
//!   continues across the rotation), strip the marker from the
//!   type byte, then read the body with the new schedule.
//! - **Write side.** Mirror: per outgoing frame, with ~3/16
//!   probability stamp a random 1..63 iteration count into the
//!   type field's high byte, encrypt the header with the current
//!   schedule, HMAC-rotate the write key by that count, then
//!   encrypt the body with the rotated schedule.
//!
//! Both sides preserve the same wire contract the legacy C
//! `cipher_check_rekey_marker` + `cipher_change_decode_key`
//! pair has carried since the original HOPE rollout — this
//! module is the Rust port of that behaviour, dressed up as an
//! `AsyncRead + AsyncWrite` so the rest of the hxnet stack
//! (transform composition, Connection actor) doesn't need to
//! know about the rekey at all.
//!
//! # Why this exists
//!
//! [`crate::cipher::BlowfishStream`] is a pure byte-streaming
//! cipher with no notion of Hotline frames. R3.3.e-4d's first
//! attempt at HOPE-Blowfish-over-hxnet ran into this: as soon as
//! Janus tripped the rekey marker on any outgoing frame the
//! `BlowfishStream`-decrypted bytes diverged from the wire and
//! the dispatcher tore the connection down ("unknown opcode
//! 0x03010000"-style log). The 4d gate kept Blowfish on the
//! legacy GIOStream path until this module exists.

use std::io;
use std::pin::Pin;
use std::task::{Context, Poll};

use hxcrypto_stream::BlowfishOfb64State;
use tokio::io::{AsyncRead, AsyncWrite, ReadBuf};

/// Size of one Hotline wire header. `type` (u32) + `trans` (u32)
/// + `flag` (u32) + `len` (u32) + `len2` (u32) + `hc` (u16).
pub(crate) const HL_HDR_SIZE: usize = 22;

/// Size of the `hc` field in bytes. The Hotline wire encodes
/// `len = body_len + sizeof(hc)` — the `hc` field is part of
/// the 22-byte header on the wire but the `len` field at offset
/// 12 counts it as data. Subtract this to convert
/// `wire_len → body_len` on read and add it on write.
pub(crate) const HC_SIZE: usize = 2;

/// Maximum body length we will accept on the read side before
/// erroring out. Matches the cap in `hxnet::frame` (which is
/// `hotline_proto::MAX_BODY_LEN` = 1 MiB), so the dispatcher
/// doesn't need to do its own ceiling check after our parse.
const MAX_BODY_LEN: usize = 1024 * 1024;

/// HMAC algorithms HOPE may negotiate. The string form
/// matches the protocol-level chunk and is what
/// [`hxcrypto_hash::hmac_xxx`] takes.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum HopeMacAlg {
    Sha256,
    Sha1,
    Md5,
}

impl HopeMacAlg {
    fn name(self) -> &'static str {
        match self {
            HopeMacAlg::Sha256 => "HMAC-SHA256",
            HopeMacAlg::Sha1 => "HMAC-SHA1",
            HopeMacAlg::Md5 => "HMAC-MD5",
        }
    }

}

/// Rotate `key` in place by `iterations` HMAC rounds against
/// `session_key`. Mirrors the legacy `cipher_change_decode_key`
/// / `cipher_change_encode_key` loop in `src/cipher.c`:
///
/// ```c
/// for (i = 0; i < num; i++) {
///     len = hmac_xxx (key, key, keylen, sessionkey, sklen, macalg);
/// }
/// ```
///
/// Note the legacy code's arg shape is `hmac_xxx(md_out, key, keylen,
/// text, textlen, alg)` — key is the HMAC key, sessionkey is the
/// text. After each iteration the new key length equals the HMAC
/// digest size (16 / 20 / 32). The first iteration takes the
/// current key as both input and HMAC key, then subsequent
/// iterations consume the previous output. Returns the new key
/// length, or 0 if the alg is unrecognised (caller should treat
/// that as a protocol error).
fn rotate_key_in_place(
    key: &mut Vec<u8>,
    session_key: &[u8],
    macalg: HopeMacAlg,
    iterations: u32,
) -> u16 {
    let alg = macalg.name();
    let mut md = [0u8; 32];
    let mut len: u16 = 0;
    for _ in 0..iterations {
        len = hxcrypto_hash::hmac_xxx(&mut md, key, session_key, alg);
        // Caller's `md` is already `[u8; 32]`; `hmac_xxx` enforces
        // that array-length at compile time.
        if len == 0 {
            return 0;
        }
        key.clear();
        key.extend_from_slice(&md[..len as usize]);
    }
    len
}

// ------------------------------------------------------------------
// Random source for the send-side marker
// ------------------------------------------------------------------

/// Trait abstracting the random source so tests can pin marker
/// firing deterministically. Production uses [`OsRng`] which
/// reads from `getrandom(2)`; tests pass a seeded fake.
pub trait HopeRng: Send + 'static {
    /// Fill `buf` with random bytes. Must not fail in production
    /// (a `getrandom` failure in the legacy code aborted the
    /// frame, which we mirror by panicking — same fail-loud
    /// posture).
    fn fill(&mut self, buf: &mut [u8]);
}

/// Production randomness: pulls from the OS via the `getrandom`
/// crate (same backend `src/rand.c` uses on the C side, so the
/// security posture matches one-to-one).
#[derive(Default)]
pub struct OsRng;

impl HopeRng for OsRng {
    fn fill(&mut self, buf: &mut [u8]) {
        if let Err(e) = getrandom::fill(buf) {
            // The legacy random_bytes() does the same: a
            // failure here means the OS entropy pool is broken
            // and continuing would emit predictable cipher
            // material. Abort loud rather than send weak bytes.
            panic!("hope_blowfish: getrandom failed: {e}");
        }
    }
}

// ------------------------------------------------------------------
// HopeBlowfishStream
// ------------------------------------------------------------------

/// Read-side parser state.
#[derive(Debug)]
enum ReadState {
    /// Accumulating bytes for the next header. `pos` is the
    /// count already inside `header_buf`.
    Header { pos: usize, header_buf: [u8; HL_HDR_SIZE] },
    /// Accumulating body bytes. `remaining` is what's still to
    /// come from inner.
    Body { remaining: usize },
}

/// Write-side parser state.
#[derive(Debug)]
enum WriteState {
    /// Accumulating the next plaintext frame from the caller.
    /// `pos` is the count already inside `plaintext_buf`.
    /// `frame_len` is `Some` once we've seen the full header
    /// (and therefore know how many body bytes to expect).
    AccumulateFrame { pos: usize, frame_len: Option<usize> },
    /// Encrypted bytes ready to push to the inner stream.
    DrainCiphertext,
}

/// HOPE-aware Blowfish-OFB-64 adapter.
pub struct HopeBlowfishStream<S, R: HopeRng = OsRng> {
    inner: S,
    /// Read-side OFB state. `read_key` is the HOPE-derived
    /// initial key; it gets HMAC-rotated each time the read
    /// path sees a marker. The schedule inside `read_state`
    /// stays in sync with `read_key` (we call `set_key` after
    /// every rotation).
    read_state: BlowfishOfb64State,
    read_key: Vec<u8>,
    read_sm: ReadState,
    /// Decrypted bytes (post-marker-strip) waiting for delivery
    /// to the caller. Filled by the state machine, drained by
    /// `poll_read`.
    read_pending: Vec<u8>,
    read_pending_pos: usize,

    /// Write-side OFB state. Parallel to the read side.
    write_state: BlowfishOfb64State,
    write_key: Vec<u8>,
    write_sm: WriteState,
    /// Plaintext bytes accumulated from the caller until we
    /// have a full frame and can encrypt it as a unit.
    write_plaintext: Vec<u8>,
    /// Ciphertext ready to ship to the inner stream.
    write_ciphertext: Vec<u8>,
    write_ciphertext_pos: usize,

    /// Session key, fed into the HMAC rotation as `text`.
    /// Immutable for the lifetime of the connection.
    session_key: Vec<u8>,
    /// HMAC algorithm HOPE negotiated.
    macalg: HopeMacAlg,
    /// Source of marker-decision random bytes on the write side.
    rng: R,
    /// Whether the write side may insert HOPE rekey markers into
    /// outgoing frames. The legacy C send path (src/cipher.c —
    /// the `compress_encode_type == COMPRESS_NONE` gate around
    /// `cipher_check_rekey_marker`) only inserts markers when
    /// compression is disabled; firing them on top of a
    /// compression layer would produce a frame the server
    /// doesn't expect because pre-spec servers assume the
    /// legacy "no-marker-when-compression-on" invariant. The
    /// read side still checks for markers either way — servers
    /// that DO send markers are honored — but on our side
    /// markers stay off whenever compression is in the stack.
    /// Builder in `transform::compose` sets this based on the
    /// compression kind.
    write_marker_enabled: bool,
}

impl<S> HopeBlowfishStream<S, OsRng> {
    /// Build a HOPE-aware Blowfish adapter on top of `inner`.
    /// `read_state` / `write_state` come from the HOPE
    /// handshake's post-Step-2 cipher init (matching what
    /// `gtkhx_blowfish_ofb64_new(read_key, ...)` would produce
    /// on the C side). `read_key` / `write_key` are the HOPE-
    /// derived initial keys; they get HMAC-rotated as markers
    /// fire on the wire. `session_key` is the HOPE session-key
    /// bytes that the rotation hashes against. `macalg` is the
    /// HMAC algorithm HOPE negotiated.
    pub fn new(
        inner: S,
        read_state: BlowfishOfb64State,
        read_key: Vec<u8>,
        write_state: BlowfishOfb64State,
        write_key: Vec<u8>,
        session_key: Vec<u8>,
        macalg: HopeMacAlg,
        write_marker_enabled: bool,
    ) -> Self {
        Self::with_rng(
            inner,
            read_state,
            read_key,
            write_state,
            write_key,
            session_key,
            macalg,
            write_marker_enabled,
            OsRng,
        )
    }
}

impl<S, R: HopeRng> HopeBlowfishStream<S, R> {
    /// Constructor that lets the test suite inject a
    /// deterministic [`HopeRng`].
    pub fn with_rng(
        inner: S,
        read_state: BlowfishOfb64State,
        read_key: Vec<u8>,
        write_state: BlowfishOfb64State,
        write_key: Vec<u8>,
        session_key: Vec<u8>,
        macalg: HopeMacAlg,
        write_marker_enabled: bool,
        rng: R,
    ) -> Self {
        Self {
            inner,
            read_state,
            read_key,
            read_sm: ReadState::Header {
                pos: 0,
                header_buf: [0u8; HL_HDR_SIZE],
            },
            read_pending: Vec::new(),
            read_pending_pos: 0,
            write_state,
            write_key,
            write_sm: WriteState::AccumulateFrame {
                pos: 0,
                frame_len: None,
            },
            write_plaintext: Vec::new(),
            write_ciphertext: Vec::new(),
            write_ciphertext_pos: 0,
            session_key,
            macalg,
            rng,
            write_marker_enabled,
        }
    }

    /// Borrow the inner stream. Tests poke the underlying
    /// duplex this way.
    pub fn inner(&self) -> &S {
        &self.inner
    }

    /// Consume the adapter and return the inner stream.
    pub fn into_inner(self) -> S {
        self.inner
    }

    /// Apply `iterations` HMAC rotations to the read key + state.
    /// Returns `Ok(())` on success, `Err` if the digest length
    /// from HMAC came back 0 (unrecognised alg — shouldn't
    /// happen post-handshake, but we surface it as a protocol
    /// error rather than panicking).
    fn rotate_read(&mut self, iterations: u32) -> io::Result<()> {
        let new_len = rotate_key_in_place(
            &mut self.read_key,
            &self.session_key,
            self.macalg,
            iterations,
        );
        if new_len == 0 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!("hope_blowfish: HMAC rotation failed (alg={:?})", self.macalg),
            ));
        }
        if !self.read_state.set_key(&self.read_key) {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "hope_blowfish: rotated read key rejected by Blowfish",
            ));
        }
        Ok(())
    }

    /// Mirror of [`Self::rotate_read`] for the write direction.
    fn rotate_write(&mut self, iterations: u32) -> io::Result<()> {
        let new_len = rotate_key_in_place(
            &mut self.write_key,
            &self.session_key,
            self.macalg,
            iterations,
        );
        if new_len == 0 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!("hope_blowfish: HMAC rotation failed (alg={:?})", self.macalg),
            ));
        }
        if !self.write_state.set_key(&self.write_key) {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "hope_blowfish: rotated write key rejected by Blowfish",
            ));
        }
        Ok(())
    }
}

// ------------------------------------------------------------------
// AsyncRead
// ------------------------------------------------------------------

impl<S: AsyncRead + Unpin, R: HopeRng + Unpin> AsyncRead for HopeBlowfishStream<S, R> {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut ReadBuf<'_>,
    ) -> Poll<io::Result<()>> {
        let this = self.get_mut();

        loop {
            // 1. Drain any already-processed plaintext bytes
            //    waiting for the caller. We deliver in caller-
            //    sized chunks regardless of where frame
            //    boundaries fall, mirroring how the
            //    BlowfishStream sibling works.
            if this.read_pending_pos < this.read_pending.len() {
                let remaining = &this.read_pending[this.read_pending_pos..];
                let n = remaining.len().min(buf.remaining());
                buf.put_slice(&remaining[..n]);
                this.read_pending_pos += n;
                if this.read_pending_pos == this.read_pending.len() {
                    this.read_pending.clear();
                    this.read_pending_pos = 0;
                }
                return Poll::Ready(Ok(()));
            }

            // 2. No pending; drive the state machine. Figure out
            //    how many encrypted bytes we still need before
            //    we can make any progress.
            let need = match this.read_sm {
                ReadState::Header { pos, .. } => HL_HDR_SIZE - pos,
                ReadState::Body { remaining } => remaining,
            };
            debug_assert!(need > 0);

            // 3. Read from the inner stream into a stack scratch
            //    bounded by `min(need, READ_CHUNK_SIZE)`. The
            //    `need` cap preserves the frame-boundary
            //    invariant — we never over-read into the next
            //    frame's header before applying any pending key
            //    rotation. The fixed-size cap avoids the
            //    pathological case where a 1 MiB body with a
            //    slow-feeding socket would otherwise allocate
            //    and zero-fill a fresh 1 MiB Vec per poll. With
            //    the cap, body-state reads cycle through 8 KiB
            //    of stack scratch and loop; header-state reads
            //    are always <= HL_HDR_SIZE (22 B) so they hit
            //    the cap before READ_CHUNK_SIZE matters.
            const READ_CHUNK_SIZE: usize = 8 * 1024;
            let mut scratch = [0u8; READ_CHUNK_SIZE];
            let chunk = need.min(READ_CHUNK_SIZE);
            let mut tmp = ReadBuf::new(&mut scratch[..chunk]);
            match Pin::new(&mut this.inner).poll_read(cx, &mut tmp) {
                Poll::Ready(Ok(())) => {}
                Poll::Ready(Err(e)) => return Poll::Ready(Err(e)),
                Poll::Pending => return Poll::Pending,
            }
            let got = tmp.filled().len();
            if got == 0 {
                // EOF. If we're mid-frame, that's a stream
                // truncation; otherwise a clean shutdown.
                //
                // The mid-frame branch's message includes the
                // state so failure mode is observable: in Header
                // it tells us how many header bytes were
                // received before EOF; in Body it tells us the
                // body byte count we were still waiting for.
                // The distinction matters because the actor's
                // ShutdownReason::StreamError(string) is the
                // only forensic signal the C side gets, and
                // "EOF mid-frame" by itself can't tell apart
                // "real server closed mid-send" from "cipher
                // desync produced a fake header announcing a
                // body that never came".
                match this.read_sm {
                    ReadState::Header { pos: 0, .. } => return Poll::Ready(Ok(())),
                    ReadState::Header { pos, .. } => {
                        return Poll::Ready(Err(io::Error::new(
                            io::ErrorKind::UnexpectedEof,
                            format!(
                                "hope_blowfish: EOF mid-frame (Header pos={pos}/{HL_HDR_SIZE})"
                            ),
                        )));
                    }
                    ReadState::Body { remaining } => {
                        return Poll::Ready(Err(io::Error::new(
                            io::ErrorKind::UnexpectedEof,
                            format!(
                                "hope_blowfish: EOF mid-frame (Body remaining={remaining})"
                            ),
                        )));
                    }
                }
            }

            // 4. Decrypt the bytes we just read with the
            //    current read state. OFB is symmetric; same
            //    `crypt_in_place` the BlowfishStream sibling
            //    uses.
            this.read_state.crypt_in_place(&mut scratch[..got]);

            // 5. Feed the decrypted bytes into the state
            //    machine.
            match this.read_sm {
                ReadState::Header {
                    ref mut pos,
                    ref mut header_buf,
                } => {
                    header_buf[*pos..*pos + got]
                        .copy_from_slice(&scratch[..got]);
                    *pos += got;
                    if *pos == HL_HDR_SIZE {
                        // Header complete. Inspect the marker
                        // byte BEFORE delivering the header
                        // bytes upstream (so we can strip it).
                        let mut hdr = *header_buf;
                        let marker = hdr[0];
                        if marker != 0 {
                            // Rotate read state by `marker`
                            // HMAC iterations. Strip the
                            // marker from byte 0 so the
                            // dispatcher sees a clean
                            // 24-bit opcode.
                            let count = marker as u32;
                            hdr[0] = 0;
                            if let Err(e) = this.rotate_read(count) {
                                return Poll::Ready(Err(e));
                            }
                        }
                        // Parse the wire `len` field at offset
                        // 12 (network byte order). The Hotline
                        // wire encodes `len = body_len +
                        // sizeof(hc) = body_len + 2`, so we
                        // subtract 2 to get the actual body
                        // byte count that comes off the wire
                        // after the 22-byte header. Treating
                        // `len` as body_len directly over-reads
                        // by 2 bytes per frame and quietly
                        // desyncs the frame boundary (and the
                        // rekey rotation boundary with it) —
                        // not what we want.
                        let wire_len = u32::from_be_bytes([
                            hdr[12], hdr[13], hdr[14], hdr[15],
                        ]) as usize;
                        if wire_len < HC_SIZE {
                            return Poll::Ready(Err(io::Error::new(
                                io::ErrorKind::InvalidData,
                                format!(
                                    "hope_blowfish: wire_len {wire_len} < \
                                     sizeof(hc) {HC_SIZE} (likely keystream \
                                     desync)"
                                ),
                            )));
                        }
                        let body_len = wire_len - HC_SIZE;
                        if body_len > MAX_BODY_LEN {
                            return Poll::Ready(Err(io::Error::new(
                                io::ErrorKind::InvalidData,
                                format!(
                                    "hope_blowfish: body_len {body_len} exceeds \
                                     MAX_BODY_LEN {MAX_BODY_LEN} (likely \
                                     keystream desync)"
                                ),
                            )));
                        }
                        this.read_pending.extend_from_slice(&hdr);
                        this.read_pending_pos = 0;
                        // A frame with `body_len == 0` (wire_len
                        // == 2 — only the `hc` field; common for
                        // server replies that just acknowledge
                        // the request, e.g. a TASK reply to a
                        // bodyless request) must transition
                        // straight back to Header, NOT into
                        // Body{remaining:0}. The loop's read
                        // step computes `need = remaining` and
                        // would ask the inner stream for 0
                        // bytes; the inner returns 0; the EOF
                        // check fires because we're in Body
                        // state (not Header pos=0) and the
                        // caller sees a spurious
                        // "EOF mid-frame (Body remaining=0)" —
                        // exactly the desync surface
                        // tests/integration/test_hope_blowfish_hxnet.c's
                        // post_login_burst test pinned against
                        // Janus.
                        this.read_sm = if body_len == 0 {
                            ReadState::Header {
                                pos: 0,
                                header_buf: [0u8; HL_HDR_SIZE],
                            }
                        } else {
                            ReadState::Body {
                                remaining: body_len,
                            }
                        };
                    }
                    // Loop back to deliver from read_pending
                    // (header may have been completed, or we
                    // may need to read more bytes for the
                    // header).
                }
                ReadState::Body { ref mut remaining } => {
                    // Fast path: if the caller's ReadBuf has
                    // room for everything we just decrypted,
                    // copy straight into it and skip the
                    // read_pending Vec entirely. For body
                    // chunks that arrive in caller-sized
                    // increments this elides both the
                    // `extend_from_slice` grow + memcpy and
                    // the loop's drain-into-buf memcpy on the
                    // next iteration. Falls back to the old
                    // pending-Vec path when the caller's buf
                    // is too small (the loop's drain branch
                    // handles the slow case correctly).
                    let buf_room = buf.remaining();
                    if buf_room >= got && this.read_pending_pos == this.read_pending.len() {
                        buf.put_slice(&scratch[..got]);
                        *remaining -= got;
                        if *remaining == 0 {
                            this.read_sm = ReadState::Header {
                                pos: 0,
                                header_buf: [0u8; HL_HDR_SIZE],
                            };
                        }
                        return Poll::Ready(Ok(()));
                    }
                    this.read_pending.extend_from_slice(&scratch[..got]);
                    this.read_pending_pos = 0;
                    *remaining -= got;
                    if *remaining == 0 {
                        this.read_sm = ReadState::Header {
                            pos: 0,
                            header_buf: [0u8; HL_HDR_SIZE],
                        };
                    }
                }
            }
            // Loop to drain read_pending into the caller's
            // ReadBuf.
        }
    }
}

// ------------------------------------------------------------------
// AsyncWrite
// ------------------------------------------------------------------

impl<S: AsyncWrite + Unpin, R: HopeRng + Unpin> AsyncWrite for HopeBlowfishStream<S, R> {
    fn poll_write(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<io::Result<usize>> {
        let this = self.get_mut();

        // 1. Drain any pending ciphertext first; we don't accept
        //    new plaintext until the previous frame has hit the
        //    wire (keeps the encrypt-step-once-per-frame
        //    contract simple, mirrors AeadStream's shape).
        if matches!(this.write_sm, WriteState::DrainCiphertext) {
            match drain_pending_ciphertext(
                &mut this.write_ciphertext,
                &mut this.write_ciphertext_pos,
                Pin::new(&mut this.inner),
                cx,
            ) {
                Poll::Ready(Ok(())) => {
                    // Drain done — accept fresh plaintext.
                    this.write_sm = WriteState::AccumulateFrame {
                        pos: 0,
                        frame_len: None,
                    };
                }
                Poll::Ready(Err(e)) => return Poll::Ready(Err(e)),
                Poll::Pending => return Poll::Pending,
            }
        }

        // 2. Accumulate plaintext into the frame buffer.
        if buf.is_empty() {
            return Poll::Ready(Ok(0));
        }

        let (pos, frame_len) = match this.write_sm {
            WriteState::AccumulateFrame { pos, frame_len } => (pos, frame_len),
            WriteState::DrainCiphertext => unreachable!("drained above"),
        };

        // How many bytes do we want from buf? If we haven't
        // determined the frame_len yet, soak up enough to fill
        // the header (HL_HDR_SIZE bytes). If we have, soak up
        // through frame_len.
        let target = match frame_len {
            Some(len) => len,
            None => HL_HDR_SIZE,
        };
        let want = target - pos;
        let take = want.min(buf.len());

        this.write_plaintext.extend_from_slice(&buf[..take]);
        let new_pos = pos + take;

        // Did we just complete the header (and so can parse the
        // body length)? The wire `len` field at offset 12 is
        // `body_len + sizeof(hc)`, so subtract HC_SIZE to get
        // the actual on-wire body byte count that follows the
        // 22-byte header. Same wire-format quirk hl_hdr_decode
        // / hxnet's frame::decode_header_full handle on the
        // receive side.
        let new_frame_len = match frame_len {
            Some(len) => Some(len),
            None if new_pos >= HL_HDR_SIZE => {
                let wire_len = u32::from_be_bytes([
                    this.write_plaintext[12],
                    this.write_plaintext[13],
                    this.write_plaintext[14],
                    this.write_plaintext[15],
                ]) as usize;
                if wire_len < HC_SIZE {
                    return Poll::Ready(Err(io::Error::new(
                        io::ErrorKind::InvalidInput,
                        format!(
                            "hope_blowfish: outgoing wire_len {wire_len} < \
                             sizeof(hc) {HC_SIZE}"
                        ),
                    )));
                }
                let body_len = wire_len - HC_SIZE;
                if body_len > MAX_BODY_LEN {
                    return Poll::Ready(Err(io::Error::new(
                        io::ErrorKind::InvalidInput,
                        format!(
                            "hope_blowfish: outgoing body_len {body_len} exceeds \
                             MAX_BODY_LEN {MAX_BODY_LEN}"
                        ),
                    )));
                }
                Some(HL_HDR_SIZE + body_len)
            }
            None => None,
        };

        // Is the frame now complete? If not, leave the state
        // machine in AccumulateFrame and report bytes-consumed.
        let complete = match new_frame_len {
            Some(len) if new_pos >= len => true,
            _ => false,
        };

        if !complete {
            this.write_sm = WriteState::AccumulateFrame {
                pos: new_pos,
                frame_len: new_frame_len,
            };
            return Poll::Ready(Ok(take));
        }

        // 3. Frame complete. Decide the marker, encrypt, queue.
        //    Sample 3 random bytes (matches the legacy 3
        //    sequential `random_bytes` calls in cipher.c) — but
        //    skip the sample + the marker insertion entirely
        //    when the write side is configured marker-off
        //    (compression-on case; see write_marker_enabled
        //    doc-comment on the struct field). Mirrors the
        //    legacy `compress_encode_type == COMPRESS_NONE`
        //    gate around `cipher_check_rekey_marker`.
        let fire = if this.write_marker_enabled {
            let mut rand_buf_check = [0u8; 3];
            this.rng.fill(&mut rand_buf_check);
            // First nibble: 3/16 odds — match the exact
            // constants the legacy code uses (`ran >> 4` then
            // == {2, 7, 13}).
            let ran0 = rand_buf_check[0] >> 4;
            let f = ran0 == 2 || ran0 == 7 || ran0 == 13;
            // Stash rand_buf for the iteration-count branch
            // below.
            (f, rand_buf_check)
        } else {
            (false, [0u8; 3])
        };
        let (fire, rand_buf) = fire;

        if fire {
            // Compute the iteration count exactly like the
            // legacy:
            //   ran = rand_buf[1] >> 2;
            //   if (!ran) ran = (rand_buf[2] >> 3) + 1;
            let mut ran = rand_buf[1] >> 2;
            if ran == 0 {
                ran = (rand_buf[2] >> 3) + 1;
            }
            // OR the count into the type field's high byte.
            this.write_plaintext[0] |= ran;
        }

        // Encrypt in place on the plaintext buffer. We move
        // ownership into `ciphertext` so the in-place XOR is
        // free — the previous `clone()` walked the whole frame
        // just to drop the original right after, which adds a
        // measurable cost for large frames (file-list,
        // chat-history-block, news posts). The plaintext slot
        // will be replaced with a fresh empty Vec for the next
        // frame's accumulation.
        let mut ciphertext = std::mem::take(&mut this.write_plaintext);
        this.write_state.crypt_in_place(&mut ciphertext[..HL_HDR_SIZE]);

        if fire {
            // ...rotate before encrypting the body.
            let count = (rand_buf[1] >> 2) as u32;
            // Recompute the same way the marker did so the
            // wire matches what we just stamped.
            let count = if count == 0 {
                ((rand_buf[2] >> 3) + 1) as u32
            } else {
                count
            };
            if let Err(e) = this.rotate_write(count) {
                return Poll::Ready(Err(e));
            }
        }

        // Encrypt the body.
        this.write_state.crypt_in_place(&mut ciphertext[HL_HDR_SIZE..]);

        // Stash ciphertext for draining and switch state. The
        // plaintext slot was emptied by `mem::take` above —
        // next frame's accumulation starts from a fresh Vec.
        this.write_ciphertext = ciphertext;
        this.write_ciphertext_pos = 0;
        this.write_sm = WriteState::DrainCiphertext;

        // Return how many plaintext bytes we consumed from the
        // caller in THIS poll_write call (== take). The caller
        // will retry with the remainder, and the next iteration
        // will land in the DrainCiphertext arm to flush this
        // frame to the wire before accepting fresh plaintext.
        Poll::Ready(Ok(take))
    }

    fn poll_flush(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
    ) -> Poll<io::Result<()>> {
        let this = self.get_mut();
        // Drain any in-flight ciphertext before reporting flush
        // success. If we reported flushed-while-buffered, the
        // caller would assume "bytes are on the wire" when
        // they're still in our local Vec.
        if matches!(this.write_sm, WriteState::DrainCiphertext) {
            match drain_pending_ciphertext(
                &mut this.write_ciphertext,
                &mut this.write_ciphertext_pos,
                Pin::new(&mut this.inner),
                cx,
            ) {
                Poll::Ready(Ok(())) => {
                    this.write_sm = WriteState::AccumulateFrame {
                        pos: 0,
                        frame_len: None,
                    };
                }
                Poll::Ready(Err(e)) => return Poll::Ready(Err(e)),
                Poll::Pending => return Poll::Pending,
            }
        }
        Pin::new(&mut this.inner).poll_flush(cx)
    }

    fn poll_shutdown(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
    ) -> Poll<io::Result<()>> {
        let this = self.get_mut();
        if matches!(this.write_sm, WriteState::DrainCiphertext) {
            match drain_pending_ciphertext(
                &mut this.write_ciphertext,
                &mut this.write_ciphertext_pos,
                Pin::new(&mut this.inner),
                cx,
            ) {
                Poll::Ready(Ok(())) => {
                    this.write_sm = WriteState::AccumulateFrame {
                        pos: 0,
                        frame_len: None,
                    };
                }
                Poll::Ready(Err(e)) => return Poll::Ready(Err(e)),
                Poll::Pending => return Poll::Pending,
            }
        }
        Pin::new(&mut this.inner).poll_shutdown(cx)
    }
}

/// Drain `buf[pos..]` into `inner`, advancing `pos`. Used by
/// poll_flush / poll_shutdown / the head of poll_write to flush
/// any in-flight ciphertext before processing a new frame.
fn drain_pending_ciphertext<I: AsyncWrite + Unpin>(
    buf: &mut Vec<u8>,
    pos: &mut usize,
    mut inner: Pin<&mut I>,
    cx: &mut Context<'_>,
) -> Poll<io::Result<()>> {
    while *pos < buf.len() {
        match inner.as_mut().poll_write(cx, &buf[*pos..]) {
            Poll::Ready(Ok(0)) => {
                return Poll::Ready(Err(io::Error::new(
                    io::ErrorKind::WriteZero,
                    "hope_blowfish: inner write returned 0 during drain",
                )));
            }
            Poll::Ready(Ok(n)) => {
                *pos += n;
            }
            Poll::Ready(Err(e)) => return Poll::Ready(Err(e)),
            Poll::Pending => return Poll::Pending,
        }
    }
    buf.clear();
    *pos = 0;
    Poll::Ready(Ok(()))
}

// ------------------------------------------------------------------
// Tests
// ------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use tokio::io::{AsyncReadExt, AsyncWriteExt, duplex};

    /// Test RNG: never fires the marker. Useful when we only
    /// want to exercise the framing-without-rekey path.
    #[derive(Default)]
    struct NoMarkerRng;
    impl HopeRng for NoMarkerRng {
        fn fill(&mut self, buf: &mut [u8]) {
            // ran0 = buf[0] >> 4 ∉ {2,7,13} → leave as 0.
            buf.fill(0);
        }
    }

    /// Test RNG: always fires the marker with a fixed
    /// iteration count baked in. `count` must be > 0 and < 64
    /// (the legacy bit-field range).
    struct AlwaysMarkerRng {
        count: u8,
    }
    impl HopeRng for AlwaysMarkerRng {
        fn fill(&mut self, buf: &mut [u8]) {
            // ran0 (buf[0] >> 4) == 2 → marker fires.
            buf[0] = 0x20;
            // ran (buf[1] >> 2) == count → matches the legacy
            // exact path. Tests that need count=N pre-shift
            // here to satisfy `>> 2 == N`.
            buf[1] = self.count << 2;
            buf[2] = 0;
        }
    }

    fn make_streams<RC, RS>(
        client_rng: RC,
        server_rng: RS,
    ) -> (
        HopeBlowfishStream<tokio::io::DuplexStream, RC>,
        HopeBlowfishStream<tokio::io::DuplexStream, RS>,
    )
    where
        RC: HopeRng + Unpin,
        RS: HopeRng + Unpin,
    {
        let (a, b) = duplex(64 * 1024);
        // Match the C-side legacy storage: client's read key =
        // server's write key, client's write key = server's
        // read key. Use distinct keys per direction (the bug
        // we just fixed in the bridge config).
        let read_key = b"client-read-key-32bytes--padding".to_vec();
        let write_key = b"client-write-key-32bytes-padding".to_vec();
        let session_key = b"session-key-bytes-128bit--------".to_vec();

        let client_read_state =
            BlowfishOfb64State::new(&read_key).expect("read key valid");
        let client_write_state =
            BlowfishOfb64State::new(&write_key).expect("write key valid");
        let server_read_state =
            BlowfishOfb64State::new(&write_key).expect("write key valid");
        let server_write_state =
            BlowfishOfb64State::new(&read_key).expect("read key valid");

        let client = HopeBlowfishStream::with_rng(
            a,
            client_read_state,
            read_key.clone(),
            client_write_state,
            write_key.clone(),
            session_key.clone(),
            HopeMacAlg::Sha256,
            /*write_marker_enabled=*/ true,
            client_rng,
        );
        let server = HopeBlowfishStream::with_rng(
            b,
            server_read_state,
            write_key,
            server_write_state,
            read_key,
            session_key,
            HopeMacAlg::Sha256,
            /*write_marker_enabled=*/ true,
            server_rng,
        );
        (client, server)
    }

    /// Build a Hotline frame with the given type, trans, and
    /// body. Returns the full 22-byte header + body buffer.
    /// The Hotline wire encodes `len = body_len + sizeof(hc)` so
    /// the wire `len` field is `body.len() + 2` — same
    /// off-by-hc encoding hl_hdr_decode reverses on the receive
    /// side. Tests need to match this exactly; encoding `len`
    /// as `body.len()` directly would produce wire-incompatible
    /// fixtures that mask wire_len/body_len bugs in the
    /// adapter under test.
    fn make_frame(type_: u32, trans: u32, body: &[u8]) -> Vec<u8> {
        let mut buf = Vec::with_capacity(HL_HDR_SIZE + body.len());
        let wire_len = (body.len() + HC_SIZE) as u32;
        buf.extend_from_slice(&type_.to_be_bytes());
        buf.extend_from_slice(&trans.to_be_bytes());
        buf.extend_from_slice(&0u32.to_be_bytes()); // flag
        buf.extend_from_slice(&wire_len.to_be_bytes()); // len
        buf.extend_from_slice(&wire_len.to_be_bytes()); // len2
        buf.extend_from_slice(&0u16.to_be_bytes()); // hc
        buf.extend_from_slice(body);
        buf
    }

    #[tokio::test]
    async fn round_trip_no_marker_single_frame() {
        let (mut client, mut server) =
            make_streams(NoMarkerRng, NoMarkerRng);
        let frame = make_frame(0x6b, 1, b"hello world");

        client.write_all(&frame).await.expect("write_all");
        client.flush().await.expect("flush");

        let mut received = vec![0u8; frame.len()];
        server
            .read_exact(&mut received)
            .await
            .expect("read_exact");
        assert_eq!(received, frame);
    }

    /// Regression test for the body_len=0 state-machine bug
    /// caught by the Tier 3 `post_login_burst_against_janus`
    /// reproducer. Frames with `body_len == 0` (wire_len == 2,
    /// i.e. only the hc field) used to transition the read
    /// state machine into `Body { remaining: 0 }`. The next
    /// loop iteration computed `need = 0`, asked the inner
    /// stream for 0 bytes, the inner returned 0 (because 0
    /// bytes were requested), the EOF check fired because we
    /// were in Body state (not Header pos=0), and the caller
    /// got a spurious `EOF mid-frame (Body remaining=0)`.
    /// Janus triggers this with empty TASK acknowledgement
    /// replies after USER_GETLIST / FILE_LIST / etc.; the
    /// reproducer's burst hit it on the 6th frame.
    ///
    /// Fix: when body_len == 0 at header-parse time, jump
    /// directly back to the Header state instead of entering
    /// Body{0}. This test sends a body-less frame, reads it,
    /// then sends a follow-up frame to prove the read state
    /// machine is back in Header pos=0 ready for the next
    /// frame's header.
    #[tokio::test]
    async fn round_trip_empty_body_frames_back_to_back() {
        let (mut client, mut server) =
            make_streams(NoMarkerRng, NoMarkerRng);

        // Frame 1: empty body. `body_len = 0`, wire `len = 2`
        // (just the hc). Same shape as a server TASK reply
        // that acknowledges a request with no payload.
        let frame_empty = make_frame(0x010000, 1, b"");
        client.write_all(&frame_empty).await.expect("write empty");
        client.flush().await.expect("flush empty");

        let mut received_empty = vec![0u8; frame_empty.len()];
        server
            .read_exact(&mut received_empty)
            .await
            .expect("read empty");
        assert_eq!(received_empty, frame_empty);

        // Frame 2: a frame with body. If the empty frame left
        // the read state machine stuck in Body{remaining:0},
        // the follow-up frame's header bytes would get read
        // and decrypted with the wrong expectation, causing
        // either a wrong body_len or an "EOF mid-frame" error.
        let frame_body = make_frame(0x6b, 2, b"follow-up-frame");
        client.write_all(&frame_body).await.expect("write follow-up");
        client.flush().await.expect("flush follow-up");

        let mut received_body = vec![0u8; frame_body.len()];
        server
            .read_exact(&mut received_body)
            .await
            .expect("read follow-up");
        assert_eq!(received_body, frame_body);
    }

    #[tokio::test]
    async fn round_trip_with_marker_rotates_both_sides() {
        // Client send-side fires the marker; server must
        // detect and rotate symmetrically.
        let (mut client, mut server) =
            make_streams(AlwaysMarkerRng { count: 5 }, NoMarkerRng);

        let frame = make_frame(0x6b, 1, b"frame-with-rekey-body");
        client.write_all(&frame).await.expect("write_all");
        client.flush().await.expect("flush");

        let mut received = vec![0u8; frame.len()];
        server
            .read_exact(&mut received)
            .await
            .expect("read_exact");
        // After the marker strip on the server side, what we
        // read should match the plaintext exactly.
        assert_eq!(received, frame);

        // And another frame should still round-trip (proves
        // the rotation is symmetric — both sides applied N
        // iterations to their direction-matching key).
        let frame2 = make_frame(0x6b, 2, b"post-rekey-frame");
        client.write_all(&frame2).await.expect("write_all 2");
        client.flush().await.expect("flush 2");

        let mut received2 = vec![0u8; frame2.len()];
        server
            .read_exact(&mut received2)
            .await
            .expect("read_exact 2");
        assert_eq!(received2, frame2);
    }

    #[tokio::test]
    async fn round_trip_multiple_markers_in_sequence() {
        // Five frames in a row, every one fires the marker.
        // Catches state-machine bugs that only show up after
        // multiple rotations.
        let (mut client, mut server) =
            make_streams(AlwaysMarkerRng { count: 3 }, NoMarkerRng);

        for i in 0..5u32 {
            let body = format!("frame-{i}-body");
            let frame = make_frame(0x6b, i + 1, body.as_bytes());
            client.write_all(&frame).await.expect("write_all");
            client.flush().await.expect("flush");

            let mut received = vec![0u8; frame.len()];
            server
                .read_exact(&mut received)
                .await
                .expect("read_exact");
            assert_eq!(received, frame, "frame {i} mismatch");
        }
    }

    #[tokio::test]
    async fn body_len_ceiling_rejected_on_read() {
        // Forge a frame with body_len > MAX_BODY_LEN and feed
        // it straight to the server side (no cipher applied
        // for the test, since we just want to verify the cap).
        let (a, b) = duplex(64 * 1024);
        let read_key = b"read-key-padded-to-32-bytes-----".to_vec();
        let write_key = b"write-key-padded-to-32-bytes----".to_vec();
        let session_key = b"session-key-128bit--------------".to_vec();
        // Server with both states keyed but unused (we write
        // directly to the duplex, bypassing the encryption
        // path so the read side sees plaintext bytes).
        let mut server = HopeBlowfishStream::with_rng(
            b,
            BlowfishOfb64State::new(&read_key).unwrap(),
            read_key.clone(),
            BlowfishOfb64State::new(&write_key).unwrap(),
            write_key,
            session_key,
            HopeMacAlg::Sha256,
            /*write_marker_enabled=*/ true,
            NoMarkerRng,
        );
        // Server's read_state will XOR with the keystream; we
        // need to encrypt our oversized header so it decodes
        // back to the oversized value the cap check should
        // reject. Use a side state with the same key.
        let mut side_state =
            BlowfishOfb64State::new(&read_key).unwrap();
        // Patch the wire `len` field with the value that
        // decodes to body_len = MAX_BODY_LEN + 1 after the
        // adapter subtracts HC_SIZE — i.e. `len =
        // (MAX_BODY_LEN + 1) + HC_SIZE`. With the framing fix
        // in place, just dropping MAX_BODY_LEN + 1 into the
        // wire would have parsed as body_len = MAX_BODY_LEN - 1,
        // which is in range and would NOT trip the ceiling
        // check we're trying to exercise.
        let bad_wire_len = (MAX_BODY_LEN + 1 + HC_SIZE) as u32;
        let mut hdr = make_frame(0x6b, 1, &[]);
        // Patch len field to the bad value.
        hdr[12..16].copy_from_slice(&bad_wire_len.to_be_bytes());
        side_state.crypt_in_place(&mut hdr);
        let mut writer = a;
        writer
            .write_all(&hdr)
            .await
            .expect("write encrypted bad header");
        drop(writer);
        let mut buf = [0u8; 64];
        let err = server
            .read(&mut buf)
            .await
            .expect_err("oversized body_len must be rejected");
        assert_eq!(err.kind(), io::ErrorKind::InvalidData);
    }

    /// Mirror the production Janus failure pattern: first
    /// frame has NO marker (matches the TASK reply for step 2
    /// LOGIN), second frame has the marker stamped (matches
    /// what Janus does on the SELFINFO frame ~3/16 of the
    /// time). The previous test runs always-on or always-off
    /// markers — this one specifically exercises the no-then-
    /// yes transition the wire produces in practice.
    ///
    /// We drive both sides by hand (no RNG) by writing
    /// pre-encrypted bytes directly into the duplex, so we
    /// control exactly which frame fires the marker and can
    /// assert the exact stripped opcode the reader sees.
    #[tokio::test]
    async fn read_pattern_clean_frame_then_marker_frame() {
        let (mut server_writer, client_inner) = duplex(64 * 1024);

        let read_key = b"client-read-key-32bytes--padding".to_vec();
        let write_key = b"client-write-key-32bytes-padding".to_vec();
        let session_key = b"session-key-bytes-128bit--------".to_vec();

        // Client: standard read direction. Two states because
        // we'll roll our own server-side encryption with a
        // parallel BlowfishOfb64State.
        let client_read_state =
            BlowfishOfb64State::new(&read_key).expect("read key");
        let client_write_state =
            BlowfishOfb64State::new(&write_key).expect("write key");
        let mut client = HopeBlowfishStream::with_rng(
            client_inner,
            client_read_state,
            read_key.clone(),
            client_write_state,
            write_key,
            session_key.clone(),
            HopeMacAlg::Sha256,
            /*write_marker_enabled=*/ true,
            NoMarkerRng,
        );

        // "Server" side: parallel BlowfishOfb64State that
        // encrypts the bytes we hand-craft into the duplex.
        // Same starting key/ivec/num as the client's read
        // state.
        let mut server_state =
            BlowfishOfb64State::new(&read_key).expect("server state");

        // ---- Frame 1: TASK reply, no marker ----
        // type=0x010000 (HTLS_HDR_TASK), trans=2, body 87 bytes.
        let mut frame1 = make_frame(0x010000, 2, &vec![0xAAu8; 87]);
        server_state.crypt_in_place(&mut frame1);
        server_writer
            .write_all(&frame1)
            .await
            .expect("write frame1 cipher");

        let mut received1 = vec![0u8; HL_HDR_SIZE + 87];
        client
            .read_exact(&mut received1)
            .await
            .expect("read frame1");
        // Plaintext header bytes — type=0x010000.
        let type1 = u32::from_be_bytes([
            received1[0], received1[1], received1[2], received1[3],
        ]);
        assert_eq!(type1, 0x010000, "frame 1 type should be HTLS_HDR_TASK");

        // ---- Frame 2: marker = 0x26 stamped on type ----
        // type = 0x26 << 24 | 0x000062 = 0x26000062 on the wire
        // BEFORE strip; the reader should see 0x000062 after
        // strip. body 14 bytes.
        let marker: u8 = 0x26;
        let opcode_low: u32 = 0x000062;
        let stamped_type = ((marker as u32) << 24) | opcode_low;
        let mut frame2 = make_frame(stamped_type, 1586751229, &vec![0xBBu8; 14]);
        // Server side: encrypt header with current state,
        // rotate key by `marker` HMAC iterations, encrypt body
        // with new state.
        server_state.crypt_in_place(&mut frame2[..HL_HDR_SIZE]);
        // Rotate the server-side key (mirrors what Janus does).
        let mut server_key = read_key.clone();
        let new_len = rotate_key_in_place(
            &mut server_key,
            &session_key,
            HopeMacAlg::Sha256,
            marker as u32,
        );
        assert_eq!(new_len, 32);
        assert!(server_state.set_key(&server_key));
        server_state.crypt_in_place(&mut frame2[HL_HDR_SIZE..]);
        server_writer
            .write_all(&frame2)
            .await
            .expect("write frame2 cipher");

        let mut received2 = vec![0u8; HL_HDR_SIZE + 14];
        client
            .read_exact(&mut received2)
            .await
            .expect("read frame2");
        let type2 = u32::from_be_bytes([
            received2[0], received2[1], received2[2], received2[3],
        ]);
        assert_eq!(
            type2, opcode_low,
            "frame 2 marker should have been stripped (got 0x{type2:08x})"
        );
        // Body bytes should round-trip too — proves the
        // body decryption used the rotated key.
        assert_eq!(&received2[HL_HDR_SIZE..], &vec![0xBBu8; 14]);
    }

    #[test]
    fn rotate_key_in_place_matches_iteration_loop() {
        let mut key = b"initial-key-32-bytes--padding---".to_vec();
        let session_key = b"session-key-128bits-------------".to_vec();
        let len = rotate_key_in_place(
            &mut key,
            &session_key,
            HopeMacAlg::Sha256,
            3,
        );
        assert_eq!(len, 32);
        assert_eq!(key.len(), 32);

        // Compare against a hand-rolled 3-iteration HMAC chain.
        let mut expected = b"initial-key-32-bytes--padding---".to_vec();
        for _ in 0..3 {
            let mut md = [0u8; 32];
            let len =
                hxcrypto_hash::hmac_xxx(&mut md, &expected, &session_key, "HMAC-SHA256");
            assert_eq!(len, 32);
            expected.clear();
            expected.extend_from_slice(&md[..len as usize]);
        }
        assert_eq!(key, expected);
    }
}
