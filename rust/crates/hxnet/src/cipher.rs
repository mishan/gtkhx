//! `AsyncRead + AsyncWrite` adapters that transparently encrypt
//! and decrypt a Hotline stream (Phase R3.3.c).
//!
//! Two flavours sit side by side, matching the two ciphers the
//! HOPE negotiation can produce:
//!
//! - [`BlowfishStream`] — Blowfish in 64-bit OFB mode. Stream
//!   cipher: per-byte transform. Each direction holds an
//!   independent [`BlowfishOfb64State`], so reads and writes
//!   can advance the cipher state in parallel without colliding.
//! - [`AeadStream`] — ChaCha20-Poly1305 AEAD. Frame cipher: each
//!   wire frame is a length prefix (`u32` BE, 4 bytes) followed
//!   by ciphertext + 16-byte Poly1305 tag. Reads buffer one full
//!   AEAD frame at a time, then surface the opened plaintext.
//!   Writes batch the user's plaintext into a single AEAD frame
//!   per `poll_write` call.
//!
//! # Where these live in the stack
//!
//! The [`crate::Connection`] actor takes any `AsyncRead +
//! AsyncWrite + Unpin + Send + 'static`. R3.3.b used a
//! `tokio::net::TcpStream` directly (production was plaintext
//! Hotline frames, suitable for the smoke test). R3.3.c lets a
//! caller wrap that TcpStream in a `BlowfishStream` or
//! `AeadStream`, then hand the wrapped stream to
//! `Connection::spawn` — the actor's frame loop is unchanged; it
//! just sees post-decrypt bytes on the read side and writes
//! plaintext that gets encrypted on the way out.
//!
//! Future R3.3.d work stacks compression on top of the cipher
//! layer (same shape: wrap an inner stream with a compress /
//! decompress transform). The composition order matches the wire
//! protocol's HOPE specification: compress first, then encrypt;
//! reverse on the read side.
//!
//! # Compatibility
//!
//! Byte-for-byte compatible with the C `cipher.c` /
//! `cipher_aead.c` implementations. The wire bytes a server sees
//! must be indistinguishable from what a legacy GtkHx or mhxd
//! client would emit, and the legacy client's bytes must decrypt
//! correctly here. The hxcrypto::stream and hxcrypto::aead primitive
//! crates these adapters delegate to are already pinned against
//! the C implementation by Tier 1 KAT tests; this module's tests
//! verify the AsyncRead/AsyncWrite glue doesn't introduce drift.

use std::io;
use std::pin::Pin;
use std::task::{Context, Poll};

use hxcrypto::aead::AeadState;
use hxcrypto::stream::{BlowfishOfb64State, BLOWFISH_OFB64_BLOCK_SIZE};
use tokio::io::{AsyncRead, AsyncWrite, ReadBuf};

// ============================================================
// BlowfishStream — Blowfish-OFB-64 stream cipher
// ============================================================

/// `AsyncRead + AsyncWrite` adapter that wraps an inner stream
/// with a per-direction Blowfish-OFB-64 stream cipher. The user
/// of this adapter reads plaintext (decrypted on the fly) and
/// writes plaintext (encrypted on the fly); the underlying
/// stream sees only ciphertext.
///
/// # Cipher state per direction
///
/// OFB is a stream cipher whose state advances on every byte
/// processed. Reads and writes are independent streams — they
/// see different ciphertexts — so each direction holds its own
/// [`BlowfishOfb64State`]. The two states can be (and on the
/// HOPE wire often are) derived from the same key material but
/// with different roles; `BlowfishStream` doesn't care, it just
/// takes both states and applies the right one in the right
/// direction.
///
/// # Symmetry note
///
/// OFB is its own inverse: `crypt_in_place` on ciphertext
/// produces plaintext if (and only if) the cipher state advances
/// in the same byte sequence. That's why the read direction's
/// state must NEVER be touched by the write path, and vice
/// versa — desynchronisation manifests as garbage plaintext that
/// the higher layers (the Connection actor's framer) will
/// eventually surface as an invalid header.
pub struct BlowfishStream<S> {
    inner: S,
    read_state: BlowfishOfb64State,
    write_state: BlowfishOfb64State,
}

impl<S> BlowfishStream<S> {
    /// Wrap `inner` with the supplied per-direction cipher
    /// states. The caller is responsible for deriving the
    /// states from the HOPE handshake's session key material
    /// (R3.3.d will move that derivation into hxnet too).
    pub fn new(inner: S, read_state: BlowfishOfb64State, write_state: BlowfishOfb64State) -> Self {
        Self {
            inner,
            read_state,
            write_state,
        }
    }

    /// Borrow the inner stream. Useful for tests that need to
    /// poke the underlying socket directly.
    pub fn inner(&self) -> &S {
        &self.inner
    }

    /// Consume the adapter and return the inner stream. The
    /// cipher states are dropped along with the adapter.
    pub fn into_inner(self) -> S {
        self.inner
    }
}

impl<S: AsyncRead + Unpin> AsyncRead for BlowfishStream<S> {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut ReadBuf<'_>,
    ) -> Poll<io::Result<()>> {
        // Track the filled-len before delegating so we can
        // identify exactly the new bytes the inner produced and
        // decrypt only those. ReadBuf's contract is that
        // poll_read may leave previously-filled bytes alone,
        // append zero-or-more bytes, then return — we only want
        // to decrypt the appended span.
        let this = self.get_mut();
        let before = buf.filled().len();
        let result = Pin::new(&mut this.inner).poll_read(cx, buf);
        if matches!(result, Poll::Ready(Ok(()))) {
            let after = buf.filled().len();
            if after > before {
                let new_bytes = &mut buf.filled_mut()[before..after];
                this.read_state.crypt_in_place(new_bytes);
            }
        }
        result
    }
}

impl<S: AsyncWrite + Unpin> AsyncWrite for BlowfishStream<S> {
    fn poll_write(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<Result<usize, io::Error>> {
        let this = self.get_mut();
        // Encrypt-then-write opens two desync windows: (1) inner
        // returns Pending / Err with no bytes sent yet — the
        // cipher must rewind to the pre-encrypt position so a
        // retry produces the same ciphertext; (2) inner returns
        // a partial count n < buf.len() — the cipher must end up
        // exactly n positions advanced so the next call to
        // poll_write (which will be called with the unsent
        // `buf[n..]` per AsyncWrite's contract) re-encrypts the
        // remaining plaintext from the correct keystream
        // position. Without these rollbacks the reader on the
        // other end gets garbage from the OFB byte after the
        // partial-write boundary.
        //
        // BlowfishOfb64State::save_ofb_state snapshots just the
        // 9-byte feedback state (8 ivec + 1 num) — the Blowfish
        // key schedule (~4 KiB) doesn't change during a single
        // poll_write, so we don't bother saving it. Same
        // discipline as src/network_decode.c's speculative-
        // decode rollback (see saved_bf_ivec / saved_bf_num).
        let mut scratch = buf.to_vec();
        let mut saved_ivec = [0u8; BLOWFISH_OFB64_BLOCK_SIZE];
        let mut saved_num = 0u32;
        this.write_state
            .save_ofb_state(&mut saved_ivec, &mut saved_num);
        this.write_state.crypt_in_place(&mut scratch);

        match Pin::new(&mut this.inner).poll_write(cx, &scratch) {
            Poll::Ready(Ok(n)) if n == scratch.len() => Poll::Ready(Ok(n)),
            Poll::Ready(Ok(0)) => {
                // Per AsyncWrite contract: returning Ok(0) on
                // a non-empty buf means the stream is closed.
                // Surface as WriteZero so write_all-style
                // callers don't spin. (If buf was empty, we
                // wouldn't have produced any ciphertext bytes
                // and the n == scratch.len() arm above would
                // have caught it.) Roll back the cipher state
                // since nothing was written.
                this.write_state.restore_ofb_state(&saved_ivec, saved_num);
                Poll::Ready(Err(io::Error::new(
                    io::ErrorKind::WriteZero,
                    "BlowfishStream inner write returned 0 bytes",
                )))
            }
            Poll::Ready(Ok(n)) => {
                // Partial write. Restore the pre-encrypt state
                // and re-advance by exactly n positions so the
                // adapter's cipher state matches the n bytes
                // that landed on the wire. The OFB keystream is
                // input-independent, so re-encrypting n bytes
                // of any input advances the state by n keystream
                // positions; we re-use the scratch buffer's
                // first `n` bytes (which we already own and are
                // about to drop) rather than allocating a fresh
                // throwaway Vec.
                this.write_state.restore_ofb_state(&saved_ivec, saved_num);
                this.write_state.crypt_in_place(&mut scratch[..n]);
                Poll::Ready(Ok(n))
            }
            Poll::Ready(Err(e)) => {
                this.write_state.restore_ofb_state(&saved_ivec, saved_num);
                Poll::Ready(Err(e))
            }
            Poll::Pending => {
                this.write_state.restore_ofb_state(&saved_ivec, saved_num);
                Poll::Pending
            }
        }
    }

    fn poll_flush(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Result<(), io::Error>> {
        Pin::new(&mut self.get_mut().inner).poll_flush(cx)
    }

    fn poll_shutdown(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Result<(), io::Error>> {
        Pin::new(&mut self.get_mut().inner).poll_shutdown(cx)
    }
}

// ============================================================
// AeadStream — ChaCha20-Poly1305 AEAD frame codec
// ============================================================

/// `AsyncRead + AsyncWrite` adapter that wraps an inner stream
/// with ChaCha20-Poly1305 AEAD framing. Per the HOPE AEAD wire
/// format: each frame is `u32` BE length prefix + `length` bytes
/// of ciphertext (which itself ends with a 16-byte Poly1305
/// tag).
///
/// # Read side
///
/// The reader buffers up to one complete AEAD frame at a time.
/// On `poll_read`:
///
/// 1. If a previously-opened plaintext is still being delivered,
///    copy more of it into the caller's buf.
/// 2. Otherwise, accumulate bytes from the inner stream until a
///    complete AEAD frame is buffered (length prefix + framed
///    body), then [`hxcrypto::aead::gtkhx_aead_open`] it and
///    start delivering the plaintext.
/// 3. If the inner stream returns `Pending` mid-frame, hold the
///    partial bytes for the next poll.
///
/// # Write side
///
/// Each `poll_write` call batches the supplied plaintext into a
/// single AEAD frame. The frame's full bytes are then handed to
/// the inner stream. The caller sees the number of plaintext
/// bytes accepted, which (under the same caveat as the
/// `BlowfishStream` write side) equals the count the inner
/// accepted.
pub struct AeadStream<S> {
    inner: S,
    read_state: AeadState,
    write_state: AeadState,

    /// Bytes accumulated for the in-progress inbound AEAD
    /// frame. Once enough have arrived for a full frame, the
    /// frame is opened and the plaintext moves into
    /// `read_plaintext` and `read_plaintext_pos`.
    read_buf: Vec<u8>,

    /// Opened plaintext from the last completed AEAD frame.
    /// Empty when no frame is pending delivery.
    read_plaintext: Vec<u8>,

    /// Number of bytes from `read_plaintext` already returned to
    /// the caller. Once equal to `read_plaintext.len()`, the
    /// vector is cleared and we go back to accumulating the
    /// next frame.
    read_plaintext_pos: usize,

    /// Sealed bytes pending write to the inner stream. Non-empty
    /// when a previous `poll_write` sealed a frame but the inner
    /// hadn't accepted all of it yet (Pending, partial, or Err
    /// on the inner). Subsequent `poll_write` calls drain this
    /// before sealing new plaintext.
    write_pending_frame: Vec<u8>,

    /// How many bytes of `write_pending_frame` the inner has
    /// already accepted. `write_pending_frame.len() -
    /// write_pending_pos` is what's left to drain.
    write_pending_pos: usize,

    /// The plaintext length that produced
    /// `write_pending_frame`. Returned to the caller once the
    /// full pending frame drains. This is the load-bearing
    /// invariant: the seal happens exactly once per
    /// `(plaintext, sealed)` pair; the inner write may take
    /// many polls, but the AEAD counter only advances once.
    write_pending_plaintext_len: usize,
}

impl<S> AeadStream<S> {
    /// Wrap `inner` with the supplied per-direction AEAD states.
    pub fn new(inner: S, read_state: AeadState, write_state: AeadState) -> Self {
        Self {
            inner,
            read_state,
            write_state,
            read_buf: Vec::new(),
            read_plaintext: Vec::new(),
            read_plaintext_pos: 0,
            write_pending_frame: Vec::new(),
            write_pending_pos: 0,
            write_pending_plaintext_len: 0,
        }
    }

    /// Borrow the inner stream.
    pub fn inner(&self) -> &S {
        &self.inner
    }

    /// Consume the adapter and return the inner stream.
    pub fn into_inner(self) -> S {
        self.inner
    }
}

/// Maximum AEAD ciphertext+tag size we'll accept on the read
/// side. Frames larger than this signal either a protocol error
/// or a malicious server; we surface them as an `InvalidData`
/// io::Error rather than allocate-and-fail. Matches
/// `hxcrypto::aead::AEAD_MAX_FRAME_SIZE` (16 MiB).
const AEAD_FRAME_LIMIT: u32 = hxcrypto::aead::AEAD_MAX_FRAME_SIZE;

impl<S: AsyncRead + Unpin> AsyncRead for AeadStream<S> {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut ReadBuf<'_>,
    ) -> Poll<io::Result<()>> {
        let this = self.get_mut();

        loop {
            // If a previously-opened plaintext is still being
            // delivered, copy more of it to the caller.
            if this.read_plaintext_pos < this.read_plaintext.len() {
                let remaining = &this.read_plaintext[this.read_plaintext_pos..];
                let n = remaining.len().min(buf.remaining());
                buf.put_slice(&remaining[..n]);
                this.read_plaintext_pos += n;
                if this.read_plaintext_pos == this.read_plaintext.len() {
                    this.read_plaintext.clear();
                    this.read_plaintext_pos = 0;
                }
                return Poll::Ready(Ok(()));
            }

            // Need to accumulate the next frame. Have we got
            // the 4-byte length prefix yet? Read directly into
            // `read_buf`'s spare capacity rather than into a
            // fresh per-poll Vec — same number of bytes copied
            // off the inner stream, but one fewer alloc + one
            // fewer memcpy per `poll_read` call. Large frames
            // up to the 16 MiB cap can otherwise produce
            // per-poll allocations equal to the missing-byte
            // count under backpressure.
            //
            // The temporary `len_before` / resize-write-truncate
            // dance is the canonical pattern: grow the Vec by
            // `needed` zero-bytes (which `tokio::io::ReadBuf`
            // treats as initialised storage we can fill), call
            // poll_read into the tail slice, then on
            // Pending/Err/Ok truncate back to `len_before + got`
            // so we never leave junk zeros in the buffer.
            if this.read_buf.len() < hxcrypto::aead::AEAD_LENGTH_PREFIX {
                let needed = hxcrypto::aead::AEAD_LENGTH_PREFIX - this.read_buf.len();
                let len_before = this.read_buf.len();
                this.read_buf.resize(len_before + needed, 0);
                let mut tmp = ReadBuf::new(&mut this.read_buf[len_before..]);
                let r = Pin::new(&mut this.inner).poll_read(cx, &mut tmp);
                let got = tmp.filled().len();
                this.read_buf.truncate(len_before + got);
                match r {
                    Poll::Ready(Ok(())) => {
                        if got == 0 {
                            // EOF.
                            return if this.read_buf.is_empty() {
                                Poll::Ready(Ok(()))
                            } else {
                                Poll::Ready(Err(io::Error::new(
                                    io::ErrorKind::UnexpectedEof,
                                    "AEAD frame truncated in length prefix",
                                )))
                            };
                        }
                    }
                    Poll::Ready(Err(e)) => return Poll::Ready(Err(e)),
                    Poll::Pending => return Poll::Pending,
                }
                continue;
            }

            // We have the prefix. Decode it.
            let length = u32::from_be_bytes([
                this.read_buf[0],
                this.read_buf[1],
                this.read_buf[2],
                this.read_buf[3],
            ]);
            if length > AEAD_FRAME_LIMIT {
                return Poll::Ready(Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    format!(
                        "AEAD frame length {} exceeds limit {}",
                        length, AEAD_FRAME_LIMIT
                    ),
                )));
            }
            // A valid frame body is `ciphertext || tag`. The tag
            // is fixed-size (Poly1305 = AEAD_TAG_SIZE bytes); a
            // body shorter than that can't possibly contain a
            // full tag and is malformed. Reject early with a
            // clear InvalidData error rather than reading the
            // truncated frame and letting `gtkhx_aead_open`
            // produce a generic "open failed" — the cause of the
            // failure is structurally obvious here and worth
            // surfacing distinctly.
            if (length as usize) < hxcrypto::aead::AEAD_TAG_SIZE {
                return Poll::Ready(Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    format!(
                        "AEAD frame length {} is smaller than tag size {}",
                        length,
                        hxcrypto::aead::AEAD_TAG_SIZE
                    ),
                )));
            }
            let total = hxcrypto::aead::AEAD_LENGTH_PREFIX + length as usize;

            // Read more body bytes until we have the whole
            // frame. Same spare-capacity trick as the prefix
            // read above.
            if this.read_buf.len() < total {
                let needed = total - this.read_buf.len();
                let len_before = this.read_buf.len();
                this.read_buf.resize(len_before + needed, 0);
                let mut tmp = ReadBuf::new(&mut this.read_buf[len_before..]);
                let r = Pin::new(&mut this.inner).poll_read(cx, &mut tmp);
                let got = tmp.filled().len();
                this.read_buf.truncate(len_before + got);
                match r {
                    Poll::Ready(Ok(())) => {
                        if got == 0 {
                            return Poll::Ready(Err(io::Error::new(
                                io::ErrorKind::UnexpectedEof,
                                "AEAD frame truncated in body",
                            )));
                        }
                    }
                    Poll::Ready(Err(e)) => return Poll::Ready(Err(e)),
                    Poll::Pending => return Poll::Pending,
                }
                continue;
            }

            // Frame complete. Open it.
            //
            // SAFETY: the FFI-shaped function gtkhx_aead_open
            // takes raw byte pointers and writes the plaintext
            // into a caller-supplied output buffer. Plaintext
            // size is ciphertext - tag bytes (length prefix is
            // not part of the ciphertext). We size pt_out
            // accordingly and pass the framed input verbatim.
            let pt_capacity = (length as usize).saturating_sub(hxcrypto::aead::AEAD_TAG_SIZE);
            let mut pt_out = vec![0u8; pt_capacity];
            let opened_len = unsafe {
                hxcrypto::aead::gtkhx_aead_open(
                    &mut this.read_state,
                    this.read_buf.as_ptr(),
                    total,
                    pt_out.as_mut_ptr(),
                    pt_capacity,
                )
            };
            // `gtkhx_aead_open` returns 0 on failure (tag
            // mismatch / malformed frame); on success returns the
            // plaintext length. Return type is `usize`, so the
            // legacy `<= 0` shape was a misleading no-op for the
            // negative branch and clippy's "absurd extreme
            // comparison" would flag it.
            if opened_len == 0 {
                return Poll::Ready(Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    "AEAD open failed (tag mismatch or malformed frame)",
                )));
            }
            pt_out.truncate(opened_len);
            this.read_buf.clear();
            this.read_plaintext = pt_out;
            this.read_plaintext_pos = 0;
            // Loop back; next iteration will copy plaintext to
            // the caller.
        }
    }
}

/// Maximum plaintext bytes per AEAD frame.
/// `hxcrypto::aead::AEAD_MAX_FRAME_SIZE` caps the body
/// (ciphertext + tag) — NOT the wire frame including the length
/// prefix. So plaintext_max = AEAD_MAX_FRAME_SIZE - AEAD_TAG_SIZE,
/// without also subtracting the prefix (an earlier version did
/// and was off by 4 bytes — it would have rejected plaintexts the
/// underlying `gtkhx_aead_seal` accepts). We reject oversized
/// plaintext at the entrance to `poll_write` so a buggy caller
/// can't force a multi-MiB allocation that the seal would refuse
/// anyway.
const AEAD_MAX_PLAINTEXT_PER_FRAME: usize =
    (hxcrypto::aead::AEAD_MAX_FRAME_SIZE as usize) - hxcrypto::aead::AEAD_TAG_SIZE;

impl<S: AsyncWrite + Unpin> AsyncWrite for AeadStream<S> {
    fn poll_write(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<Result<usize, io::Error>> {
        let this = self.get_mut();

        // Buffer-and-drain pattern. AEAD's counter advances
        // once per seal; we MUST NOT re-seal after a partial /
        // Pending inner write because that would burn a fresh
        // nonce on retry and the peer would see a jump that
        // tag-fails. So: seal once into `write_pending_frame`,
        // then drain across as many `poll_write` calls as the
        // inner takes. Only when the full frame has landed on
        // the wire do we return Ready(Ok(plaintext_len)) to
        // signal "this many of YOUR bytes were accepted."
        loop {
            // Phase 1: drain any pending sealed bytes.
            if this.write_pending_pos < this.write_pending_frame.len() {
                let to_send = &this.write_pending_frame[this.write_pending_pos..];
                match Pin::new(&mut this.inner).poll_write(cx, to_send) {
                    Poll::Ready(Ok(0)) => {
                        // Per AsyncWrite contract: 0 from a
                        // non-empty buf means the stream is
                        // closed. Surface as WriteZero so the
                        // actor tears the connection down.
                        return Poll::Ready(Err(io::Error::new(
                            io::ErrorKind::WriteZero,
                            "AEAD inner write returned 0 bytes",
                        )));
                    }
                    Poll::Ready(Ok(n)) => {
                        this.write_pending_pos += n;
                        // Loop back to try draining more —
                        // inner accepted some, maybe more is
                        // available before we'd see Pending.
                        continue;
                    }
                    Poll::Ready(Err(e)) => return Poll::Ready(Err(e)),
                    Poll::Pending => return Poll::Pending,
                }
            }

            // Phase 1 complete OR pending frame was empty to
            // begin with. If we had a pending frame, it's now
            // fully drained — report its plaintext length to
            // the caller.
            if !this.write_pending_frame.is_empty() {
                let plt_len = this.write_pending_plaintext_len;
                this.write_pending_frame.clear();
                this.write_pending_pos = 0;
                this.write_pending_plaintext_len = 0;
                return Poll::Ready(Ok(plt_len));
            }

            // Phase 2: no pending frame — seal `buf` into one.
            // Defensive: if buf is empty, return Ok(0) without
            // sealing (an empty AEAD frame would still consume
            // a counter, and the caller's write_all wouldn't
            // hit this anyway).
            if buf.is_empty() {
                return Poll::Ready(Ok(0));
            }
            if buf.len() > AEAD_MAX_PLAINTEXT_PER_FRAME {
                return Poll::Ready(Err(io::Error::new(
                    io::ErrorKind::InvalidInput,
                    format!(
                        "AEAD plaintext {} exceeds maximum {}",
                        buf.len(),
                        AEAD_MAX_PLAINTEXT_PER_FRAME
                    ),
                )));
            }

            let frame_cap =
                hxcrypto::aead::AEAD_LENGTH_PREFIX + buf.len() + hxcrypto::aead::AEAD_TAG_SIZE;
            let mut framed = vec![0u8; frame_cap];
            // SAFETY: pointers come from owned Vecs of the
            // right length; gtkhx_aead_seal honours the
            // documented size contract. The counter advance
            // inside seal is the load-bearing side effect we
            // want — it happens exactly once per logical
            // frame, regardless of how many polls the drain
            // takes.
            let written = unsafe {
                hxcrypto::aead::gtkhx_aead_seal(
                    &mut this.write_state,
                    buf.as_ptr(),
                    buf.len(),
                    framed.as_mut_ptr(),
                    frame_cap,
                )
            };
            // `gtkhx_aead_seal` returns 0 on failure; on success
            // returns the sealed frame length. Return type is
            // `usize`, so `<= 0` was a misleading shape (clippy's
            // "absurd extreme comparison" lint trigger).
            if written == 0 {
                return Poll::Ready(Err(io::Error::other("AEAD seal failed")));
            }
            framed.truncate(written);
            this.write_pending_frame = framed;
            this.write_pending_pos = 0;
            this.write_pending_plaintext_len = buf.len();
            // Loop back to drain phase.
        }
    }

    fn poll_flush(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Result<(), io::Error>> {
        let this = self.get_mut();
        // Drain any sealed bytes pending in write_pending_frame
        // before delegating to the inner flush. Otherwise the
        // caller observes Ok(()) from flush while ciphertext we
        // sealed in a previous poll_write is still in our own
        // buffer — those bytes haven't reached the inner stream,
        // let alone the peer.
        match ready_drain_aead_pending(this, cx) {
            Poll::Ready(Ok(())) => {}
            other => return other,
        }
        Pin::new(&mut this.inner).poll_flush(cx)
    }

    fn poll_shutdown(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Result<(), io::Error>> {
        let this = self.get_mut();
        // Same as flush: drain pending sealed bytes first.
        // Otherwise shutdown silently truncates the stream by
        // closing the inner before the last AEAD frame's bytes
        // were committed.
        match ready_drain_aead_pending(this, cx) {
            Poll::Ready(Ok(())) => {}
            other => return other,
        }
        Pin::new(&mut this.inner).poll_shutdown(cx)
    }
}

/// Drive `write_pending_frame[write_pending_pos..]` into the
/// inner stream until fully drained. Returns `Ready(Ok(()))`
/// when the pending buffer is fully on the wire; `Pending` /
/// `Ready(Err)` propagate from the inner. Used by AeadStream's
/// flush / shutdown so the caller's "flush" observation is
/// honest about what's on the wire.
///
/// # Why this helper does NOT clear `write_pending_*`
///
/// The AsyncWrite contract says: if a previous `poll_write`
/// returned `Pending`, the caller may eventually retry that
/// same write. Our `poll_write` implements that retry by
/// observing `write_pending_frame.is_empty() == false` and
/// returning `Ok(write_pending_plaintext_len)` once the bytes
/// drain (which both reports "your plaintext is committed" to
/// the caller and clears the pending state).
///
/// If a caller does this sequence:
///
/// 1. `poll_write(A)` → seals A, drains partially, returns
///    `Pending`. `write_pending_*` set.
/// 2. `poll_flush(_)` — to drive the partial bytes out.
/// 3. `poll_write(A)` retry — caller, per the AsyncWrite
///    contract, hands the same plaintext back.
///
/// …and the flush helper cleared `write_pending_*` in step 2,
/// then step 3 sees empty pending state, re-seals plaintext A
/// **as a brand-new frame** (burning a second AEAD counter)
/// and the peer sees a duplicate frame. To preserve the
/// contract, step 2 must drain the bytes but leave the pending
/// state set; step 3 then enters poll_write, sees pending
/// state, drains nothing (already on the wire), and returns
/// `Ok(plaintext_len)` while clearing the state.
fn ready_drain_aead_pending<S: AsyncWrite + Unpin>(
    this: &mut AeadStream<S>,
    cx: &mut Context<'_>,
) -> Poll<Result<(), io::Error>> {
    while this.write_pending_pos < this.write_pending_frame.len() {
        let to_send = &this.write_pending_frame[this.write_pending_pos..];
        match Pin::new(&mut this.inner).poll_write(cx, to_send) {
            Poll::Ready(Ok(0)) => {
                return Poll::Ready(Err(io::Error::new(
                    io::ErrorKind::WriteZero,
                    "AEAD inner write returned 0 bytes during drain",
                )));
            }
            Poll::Ready(Ok(n)) => {
                this.write_pending_pos += n;
            }
            Poll::Ready(Err(e)) => return Poll::Ready(Err(e)),
            Poll::Pending => return Poll::Pending,
        }
    }
    // Drained. Intentionally do NOT clear write_pending_* —
    // see the doc-comment above. poll_write's pending-drain
    // branch is the one that clears state and returns
    // Ok(plaintext_len) to the caller's retry.
    Poll::Ready(Ok(()))
}
