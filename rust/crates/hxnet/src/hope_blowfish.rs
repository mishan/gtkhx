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
    ) -> Self {
        Self::with_rng(
            inner,
            read_state,
            read_key,
            write_state,
            write_key,
            session_key,
            macalg,
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
            //    sized exactly to what we need next (so we never
            //    over-read past a frame boundary and decrypt
            //    body bytes with the wrong key after a rotation).
            //    The scratch is bounded by HL_HDR_SIZE +
            //    MAX_BODY_LEN, which is the largest legitimate
            //    single read for one frame; in practice header
            //    reads dominate.
            let mut scratch = vec![0u8; need];
            let mut tmp = ReadBuf::new(&mut scratch);
            match Pin::new(&mut this.inner).poll_read(cx, &mut tmp) {
                Poll::Ready(Ok(())) => {}
                Poll::Ready(Err(e)) => return Poll::Ready(Err(e)),
                Poll::Pending => return Poll::Pending,
            }
            let got = tmp.filled().len();
            if got == 0 {
                // EOF. If we're mid-frame, that's a stream
                // truncation; otherwise a clean shutdown.
                match this.read_sm {
                    ReadState::Header { pos: 0, .. } => return Poll::Ready(Ok(())),
                    _ => {
                        return Poll::Ready(Err(io::Error::new(
                            io::ErrorKind::UnexpectedEof,
                            "hope_blowfish: EOF mid-frame",
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
                        // Parse `len` (body length) from offset
                        // 12 (network byte order). Defend the
                        // dispatcher against bogus values up
                        // front.
                        let body_len = u32::from_be_bytes([
                            hdr[12], hdr[13], hdr[14], hdr[15],
                        ]) as usize;
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
                        this.read_sm = ReadState::Body {
                            remaining: body_len,
                        };
                    }
                    // Loop back to deliver from read_pending
                    // (header may have been completed, or we
                    // may need to read more bytes for the
                    // header).
                }
                ReadState::Body { ref mut remaining } => {
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
        // body length)?
        let new_frame_len = match frame_len {
            Some(len) => Some(len),
            None if new_pos >= HL_HDR_SIZE => {
                let body_len = u32::from_be_bytes([
                    this.write_plaintext[12],
                    this.write_plaintext[13],
                    this.write_plaintext[14],
                    this.write_plaintext[15],
                ]) as usize;
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
        //    sequential `random_bytes` calls in cipher.c).
        let mut rand_buf = [0u8; 3];
        this.rng.fill(&mut rand_buf);
        // First nibble: 3/16 odds — match the exact constants
        // the legacy code uses (`ran >> 4` then == {2, 7, 13}).
        let ran0 = rand_buf[0] >> 4;
        let fire = ran0 == 2 || ran0 == 7 || ran0 == 13;

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

        // Encrypt the header...
        let mut ciphertext = this.write_plaintext.clone();
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

        // Stash ciphertext for draining, reset plaintext
        // buffer, switch state.
        this.write_ciphertext = ciphertext;
        this.write_ciphertext_pos = 0;
        this.write_plaintext.clear();
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
            server_rng,
        );
        (client, server)
    }

    /// Build a Hotline frame with the given type, trans, and
    /// body. Returns the full 22-byte header + body buffer.
    fn make_frame(type_: u32, trans: u32, body: &[u8]) -> Vec<u8> {
        let mut buf = Vec::with_capacity(HL_HDR_SIZE + body.len());
        buf.extend_from_slice(&type_.to_be_bytes());
        buf.extend_from_slice(&trans.to_be_bytes());
        buf.extend_from_slice(&0u32.to_be_bytes()); // flag
        buf.extend_from_slice(&(body.len() as u32).to_be_bytes()); // len
        buf.extend_from_slice(&(body.len() as u32).to_be_bytes()); // len2
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
            NoMarkerRng,
        );
        // Server's read_state will XOR with the keystream; we
        // need to encrypt our oversized header so it decodes
        // back to the oversized value the cap check should
        // reject. Use a side state with the same key.
        let mut side_state =
            BlowfishOfb64State::new(&read_key).unwrap();
        let bad_body_len = (MAX_BODY_LEN + 1) as u32;
        let mut hdr = make_frame(0x6b, 1, &[]);
        // Patch len field to the bad value.
        hdr[12..16].copy_from_slice(&bad_body_len.to_be_bytes());
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
