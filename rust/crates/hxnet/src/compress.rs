//! `AsyncRead + AsyncWrite` adapters for the three HOPE
//! compression algorithms (Phase R3.3.d).
//!
//! - [`GzipStream`] — RFC 1950 zlib deflate / inflate. Despite
//!   the HOPE name, the wire bytes are zlib-wrapped deflate, NOT
//!   RFC 1952 gzip. Persistent codec state per direction.
//! - [`Lz4Stream`] — LZ4F (LZ4 frame format). One frame per
//!   `poll_write`.
//! - [`ZstdStream`] — Zstandard. One frame per `poll_write`.
//!
//! # Buffer-and-drain write semantics
//!
//! Every adapter here encodes into a `write_pending_frame` and
//! drains it across as many `poll_write` calls as the inner
//! stream takes. The compress step happens exactly once per
//! logical user `buf`; if the inner returns `Pending` or a
//! partial count, we hold the encoded bytes until the next
//! poll. This matches `AeadStream`'s shape (R3.3.c) and is the
//! load-bearing invariant for GZIP, whose persistent deflate
//! state can't be rolled back if we re-encoded on retry.
//!
//! # Where these live in the stack
//!
//! R3.3.c added cipher adapters; R3.3.d (this module) layers
//! compression on top. Production stacks are:
//!
//! ```text
//! TcpStream  ──►  BlowfishStream / AeadStream  ──►  GzipStream / Lz4Stream / ZstdStream  ──►  Connection actor
//! ```
//!
//! Each layer is a generic `S: AsyncRead + AsyncWrite + Unpin`
//! wrapper, so they compose freely. The Connection actor doesn't
//! need to know what's underneath — it just sees framed plaintext.

use std::io;
use std::pin::Pin;
use std::task::{Context, Poll};

use flate2::{Compress, Compression, Decompress, FlushCompress, FlushDecompress, Status};
use hxcrypto_aead::AEAD_MAX_FRAME_SIZE;
use tokio::io::{AsyncRead, AsyncWrite, ReadBuf};

/// Buffer size for pulling raw bytes from the inner stream on
/// the decode side. Generous enough that a typical Hotline
/// transaction (well under 1 KiB) usually arrives in one inner
/// read.
const DECODE_READ_CHUNK: usize = 64 * 1024;

/// Drive `pending_frame[*pending_pos..]` into `inner` until
/// fully drained, then reset the pending state. Returns
/// `Ready(Ok(()))` when the buffer is fully on the wire;
/// `Pending` / `Ready(Err)` propagate from the inner. Shared by
/// every compression adapter's poll_flush + poll_shutdown so
/// the caller's "flush" / "shutdown" observation is honest
/// about what's on the wire — otherwise a delegate-to-inner
/// flush could return Ok(()) while compressed bytes were still
/// buffered here in `pending_frame`, and shutdown could
/// silently truncate the stream.
///
/// # Why this helper does NOT clear `pending_frame`
///
/// The AsyncWrite contract says: if a previous `poll_write`
/// returned `Pending`, the caller may eventually retry with the
/// same plaintext. Every adapter's `poll_write` observes
/// `pending_frame.is_empty() == false` and returns
/// `Ok(pending_plaintext_len)` once the bytes drain — that's
/// both the "your plaintext is committed" report and where the
/// state actually clears.
///
/// If a caller does this:
///
/// 1. `poll_write(A)` → encodes A, drains partially, returns
///    `Pending`. `pending_*` set.
/// 2. `poll_flush(_)` — to drive the partial bytes out.
/// 3. `poll_write(A)` retry — same plaintext per AsyncWrite.
///
/// …and the flush helper cleared `pending_*` in step 2, then
/// step 3 sees empty state, re-encodes A as a brand-new frame
/// (e.g. burns a counter or duplicates a compressed block) and
/// the peer decodes garbage. Matches the same fix `AeadStream`'s
/// `ready_drain_aead_pending` got — see that helper's
/// doc-comment for the longer rationale.
fn drain_pending_write<S: AsyncWrite + Unpin + ?Sized>(
    pending_frame: &mut [u8],
    pending_pos: &mut usize,
    mut inner: Pin<&mut S>,
    cx: &mut Context<'_>,
) -> Poll<Result<(), io::Error>> {
    while *pending_pos < pending_frame.len() {
        let to_send = &pending_frame[*pending_pos..];
        match inner.as_mut().poll_write(cx, to_send) {
            Poll::Ready(Ok(0)) => {
                return Poll::Ready(Err(io::Error::new(
                    io::ErrorKind::WriteZero,
                    "compress inner write returned 0 bytes during drain",
                )));
            }
            Poll::Ready(Ok(n)) => {
                *pending_pos += n;
            }
            Poll::Ready(Err(e)) => return Poll::Ready(Err(e)),
            Poll::Pending => return Poll::Pending,
        }
    }
    // Drained. Intentionally do NOT touch pending_frame /
    // pending_plaintext_len — see the doc-comment above.
    // poll_write's pending-drain branch clears state and
    // returns Ok(plaintext_len) on the caller's retry.
    Poll::Ready(Ok(()))
}

/// Worst-case zlib output size for an input of `len` bytes
/// under deflate + Sync flush. Mirrors zlib's `deflateBound`:
/// `len + (len + 16383) / 16384 * 5 + 6`. The +5 per 16 KiB
/// block accounts for the worst-case stored-block overhead;
/// the +6 accounts for the zlib header. We add a small
/// constant safety margin at the call site for the Sync-flush
/// trailer.
fn gzip_compress_bound(len: usize) -> usize {
    len + len.div_ceil(16384) * 5 + 6
}

/// Ceiling on the per-stream compressed-input accumulation
/// buffer, applied to GZIP and ZSTD `read_raw_buf`s. (LZ4 has
/// its own per-frame cap at [`LZ4_MAX_FRAME_ACCUM`] — same
/// value, but the semantics differ: LZ4's cap is per-frame
/// and resets when a frame decodes; this one is the running
/// undecoded-bytes total at any point in the stream.)
///
/// The threat model is a malicious or buggy peer that keeps
/// pushing bytes the decoder can't make progress on (consumed
/// == 0 and produced == 0 over many polls). Without a ceiling
/// the buffer grows unbounded — a memory-exhaustion DoS.
/// We pin the ceiling to [`AEAD_MAX_FRAME_SIZE`] so all three
/// transport layers fail at the same point rather than one
/// becoming the soft underbelly — if the AEAD frame cap ever
/// changes, this constant tracks it mechanically rather than
/// requiring a coordinated update across crates.
const COMPRESS_READ_BUF_CEILING: usize = AEAD_MAX_FRAME_SIZE as usize;

/// Fixed headroom that [`COMPRESS_WRITE_INPUT_MAX`] sits below
/// [`COMPRESS_READ_BUF_CEILING`]. The headroom absorbs codec
/// framing overhead so a max-sized plaintext write can't expand
/// past the read-side ceiling. 64 KiB comfortably exceeds any
/// of zlib / Lz4F / ZSTD's per-frame fixed overhead at any
/// realistic input size.
const COMPRESS_WRITE_HEADROOM: usize = 64 * 1024;

/// Ceiling on the per-call input plaintext size in
/// `poll_write`. Without it, a single oversized `write_all`
/// from a buggy or malicious caller (e.g. via the FFI's
/// `Command::WriteFrame` path) could trigger a multi-GiB
/// scratch allocation inside the codec — Gzip's
/// `gzip_compress_bound` would compute a ~2 GiB upper bound for
/// a 2 GiB input, LZ4's `Vec::with_capacity` would grab the
/// same, and ZSTD's bulk compress would mirror it. Reject the
/// write up front instead and let the caller fail loudly.
///
/// The cap sits [`COMPRESS_WRITE_HEADROOM`] below the
/// transport ceiling. The headroom matters because the
/// compressed output of an incompressible input can exceed the
/// plaintext size (zlib's `Sync` flush adds ~5 bytes of
/// stored-block framing per 16 KiB plus a 6-byte zlib header,
/// and Lz4F's frame header / block headers add a similar fixed
/// slice). Without the headroom a max-sized write could pass
/// this input check and still hand the next layer (AeadStream
/// or the raw transport) a ciphertext / framed blob bigger
/// than its own limit, advancing the compressor state before
/// the inner write errors.
///
/// hxnet's actor caps `MAX_BODY_LEN` at 1 MiB, so any
/// legitimate write is comfortably below this limit regardless.
const COMPRESS_WRITE_INPUT_MAX: usize = COMPRESS_READ_BUF_CEILING - COMPRESS_WRITE_HEADROOM;

/// Build an [`io::Error`] for a write that exceeds the
/// per-call input ceiling. Factored out so each adapter's
/// poll_write does the same check with the same error message.
fn oversized_write_error(algo: &str, len: usize) -> io::Error {
    io::Error::new(
        io::ErrorKind::InvalidInput,
        format!(
            "{algo}: poll_write input {len} bytes exceeds per-call \
             ceiling of {COMPRESS_WRITE_INPUT_MAX}"
        ),
    )
}

// ============================================================
// GzipStream
// ============================================================

/// `AsyncRead + AsyncWrite` adapter that wraps an inner stream
/// with zlib-deflate compression on the write side and inflate
/// on the read side.
///
/// Naming note: HOPE calls this algorithm "GZIP" on the wire
/// but the bytes are RFC 1950 zlib (the 2-byte zlib header and
/// 4-byte Adler-32 trailer), not RFC 1952 gzip. `flate2::Compress`
/// / `Decompress` with `zlib_header=true` produce the right
/// shape. This matches `hxcompress::CompressEncoder::Gzip` and
/// every mhxd-family server's deflateInit/inflateInit.
pub struct GzipStream<S> {
    inner: S,
    read_decompress: Decompress,
    write_compress: Compress,
    /// Plaintext produced by the last `Decompress::decompress`
    /// call but not yet copied to the caller.
    read_plaintext: Vec<u8>,
    read_plaintext_pos: usize,
    /// Raw bytes pulled from inner but not yet (fully)
    /// consumed by the decompressor. The decoder may consume
    /// only a prefix on each call; we keep the rest here for
    /// the next pass. Critically, we NEVER discard
    /// already-buffered bytes when the decoder makes no
    /// progress — those are real wire bytes that the next
    /// inner read will extend, not garbage to skip.
    read_raw_buf: Vec<u8>,
    /// Compressed bytes that have been produced by `compress()`
    /// but not yet (fully) accepted by the inner stream. Per
    /// the module-level "buffer-and-drain" invariant: we
    /// compress once per user buf, then drain across as many
    /// polls as it takes.
    write_pending_frame: Vec<u8>,
    write_pending_pos: usize,
    write_pending_plaintext_len: usize,
}

impl<S> GzipStream<S> {
    pub fn new(inner: S) -> Self {
        Self {
            inner,
            read_decompress: Decompress::new(true),
            write_compress: Compress::new(Compression::default(), true),
            read_plaintext: Vec::new(),
            read_plaintext_pos: 0,
            read_raw_buf: Vec::new(),
            write_pending_frame: Vec::new(),
            write_pending_pos: 0,
            write_pending_plaintext_len: 0,
        }
    }

    pub fn inner(&self) -> &S {
        &self.inner
    }

    pub fn into_inner(self) -> S {
        self.inner
    }
}

impl<S: AsyncRead + Unpin> AsyncRead for GzipStream<S> {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut ReadBuf<'_>,
    ) -> Poll<io::Result<()>> {
        let this = self.get_mut();

        loop {
            // Drain previously-decompressed plaintext first.
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

            // Always attempt a decompress step, EVEN with an
            // empty input. flate2's `Decompress` can produce
            // additional plaintext from internally-buffered
            // state when called with an empty input slice
            // (status BufError on a previous call means
            // "output buffer was full, more plaintext
            // available" — we have to call again with
            // available output room to drain it). Gating on
            // !read_raw_buf.is_empty() would orphan that
            // plaintext on poll_read calls that happen to
            // arrive after the inner stream's bytes are all
            // consumed. The empty-input call returns
            // immediately with no progress if there's
            // genuinely nothing to drain; that's the signal
            // to fall through and poll the inner for more.
            // Use read_plaintext itself as the decode scratch.
            // The drain loop above guarantees it's empty here
            // (we returned early if any bytes were still
            // pending), so resize-then-decode-then-truncate
            // reuses the allocation across polls — flate2's
            // Decompress::decompress always overwrites the
            // first `produced` bytes, so the resize's zero-fill
            // is throwaway. First call allocates DECODE_READ_CHUNK;
            // subsequent calls find capacity already in place
            // and resize is a no-op.
            this.read_plaintext.resize(DECODE_READ_CHUNK, 0);
            let before_in = this.read_decompress.total_in();
            let before_out = this.read_decompress.total_out();
            let status = this.read_decompress.decompress(
                &this.read_raw_buf,
                &mut this.read_plaintext,
                FlushDecompress::Sync,
            );
            let consumed = (this.read_decompress.total_in() - before_in) as usize;
            let produced = (this.read_decompress.total_out() - before_out) as usize;
            match status {
                Ok(_) => {
                    if consumed > 0 {
                        this.read_raw_buf.drain(..consumed);
                    }
                    if produced > 0 {
                        this.read_plaintext.truncate(produced);
                        this.read_plaintext_pos = 0;
                        // Loop to deliver plaintext.
                        continue;
                    }
                    // No produce — shrink back so the next
                    // drain check sees an empty buffer and
                    // doesn't try to deliver zero-fill bytes.
                    this.read_plaintext.clear();
                    // No output AND no input consumed — the
                    // decoder is genuinely stuck waiting for
                    // more bytes. Fall through to read from
                    // inner. CRITICAL: keep read_raw_buf as
                    // is — the previous (buggy) version
                    // cleared it here, dropping real wire
                    // bytes spanning a deflate block
                    // boundary.
                }
                Err(e) => {
                    return Poll::Ready(Err(io::Error::new(
                        io::ErrorKind::InvalidData,
                        format!("gzip decompress failed: {e}"),
                    )));
                }
            }

            // Read directly into read_raw_buf's spare capacity
            // — resize / read / truncate avoids the per-poll
            // scratch Vec allocation + copy the previous design
            // paid every time. AeadStream uses the same shape.
            let len_before = this.read_raw_buf.len();
            this.read_raw_buf.resize(len_before + DECODE_READ_CHUNK, 0);
            let mut tmp = ReadBuf::new(&mut this.read_raw_buf[len_before..]);
            match Pin::new(&mut this.inner).poll_read(cx, &mut tmp) {
                Poll::Ready(Ok(())) => {
                    let got = tmp.filled().len();
                    this.read_raw_buf.truncate(len_before + got);
                    if got == 0 {
                        // EOF. If we still have un-decoded
                        // compressed bytes, the stream ended
                        // mid-frame — surface as
                        // UnexpectedEof rather than a clean
                        // shutdown, otherwise a truncated
                        // wire looks indistinguishable from a
                        // normal end-of-transmission and the
                        // caller silently loses data.
                        if this.read_raw_buf.is_empty() {
                            return Poll::Ready(Ok(()));
                        }
                        return Poll::Ready(Err(io::Error::new(
                            io::ErrorKind::UnexpectedEof,
                            "gzip stream EOF with undecoded bytes buffered",
                        )));
                    }
                    // Defensive ceiling: if the peer keeps
                    // pushing bytes that don't let the zlib
                    // decoder make progress, this buffer would
                    // grow unbounded — a memory-exhaustion DoS
                    // vector. 16 MiB is far past any legitimate
                    // pre-decode buffering for a Hotline frame
                    // (a sane plaintext frame caps at 1 MiB and
                    // compresses down from there), so a buffer
                    // past the cap means the stream is hostile.
                    if this.read_raw_buf.len() > COMPRESS_READ_BUF_CEILING {
                        return Poll::Ready(Err(io::Error::new(
                            io::ErrorKind::InvalidData,
                            format!(
                                "gzip: accumulated {} bytes without \
                                 decompressing; giving up to avoid OOM",
                                this.read_raw_buf.len()
                            ),
                        )));
                    }
                    // Loop back to attempt decompress.
                }
                Poll::Ready(Err(e)) => {
                    this.read_raw_buf.truncate(len_before);
                    return Poll::Ready(Err(e));
                }
                Poll::Pending => {
                    this.read_raw_buf.truncate(len_before);
                    return Poll::Pending;
                }
            }
        }
    }
}

impl<S: AsyncWrite + Unpin> AsyncWrite for GzipStream<S> {
    fn poll_write(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<Result<usize, io::Error>> {
        let this = self.get_mut();

        // Buffer-and-drain. The deflate state in
        // `write_compress` advances irreversibly on every
        // `.compress()` call, so we MUST NOT re-compress after
        // a partial / Pending inner write — that would
        // produce a different byte stream on retry and
        // desynchronize the peer's decoder.
        loop {
            // Phase 1: drain pending compressed bytes.
            if this.write_pending_pos < this.write_pending_frame.len() {
                let to_send = &this.write_pending_frame[this.write_pending_pos..];
                match Pin::new(&mut this.inner).poll_write(cx, to_send) {
                    Poll::Ready(Ok(0)) => {
                        return Poll::Ready(Err(io::Error::new(
                            io::ErrorKind::WriteZero,
                            "gzip inner write returned 0 bytes",
                        )));
                    }
                    Poll::Ready(Ok(n)) => {
                        this.write_pending_pos += n;
                        continue;
                    }
                    Poll::Ready(Err(e)) => return Poll::Ready(Err(e)),
                    Poll::Pending => return Poll::Pending,
                }
            }

            // Drain complete or never-needed. Did we have a
            // frame in flight? Report its plaintext length.
            if !this.write_pending_frame.is_empty() {
                let plt_len = this.write_pending_plaintext_len;
                this.write_pending_frame.clear();
                this.write_pending_pos = 0;
                this.write_pending_plaintext_len = 0;
                return Poll::Ready(Ok(plt_len));
            }

            // Phase 2: compress `buf` into pending frame.
            if buf.is_empty() {
                return Poll::Ready(Ok(0));
            }
            // DoS-safety ceiling: reject oversized writes
            // before computing gzip_compress_bound (which would
            // scale linearly with buf.len()) and allocating
            // `out`. See COMPRESS_WRITE_INPUT_MAX.
            if buf.len() > COMPRESS_WRITE_INPUT_MAX {
                return Poll::Ready(Err(oversized_write_error("gzip", buf.len())));
            }
            // zlib's deflateBound formula: input + ceil(input
            // / 16384) * 5 + 6. flate2 doesn't expose
            // deflateBound directly, so we compute it here.
            // The earlier `buf.len() + 64` was a safe bound
            // for typical Hotline transactions (well under
            // 16 KiB so deflateBound resolved to input + ~11)
            // but it would fail on writes near MAX_BODY_LEN
            // (1 MiB) where the per-16K-block overhead can
            // total ~330 bytes. Use the full formula plus a
            // small safety margin to also account for the
            // sync-flush trailer.
            let cap = gzip_compress_bound(buf.len()) + 64;
            let mut out = vec![0u8; cap];
            let before_in = this.write_compress.total_in();
            let before_out = this.write_compress.total_out();
            match this
                .write_compress
                .compress(buf, &mut out, FlushCompress::Sync)
            {
                Ok(Status::Ok) | Ok(Status::StreamEnd) => {
                    let consumed = (this.write_compress.total_in() - before_in) as usize;
                    let produced = (this.write_compress.total_out() - before_out) as usize;
                    if consumed != buf.len() {
                        return Poll::Ready(Err(io::Error::other(
                            "gzip compress: scratch buffer too small for sync flush",
                        )));
                    }
                    out.truncate(produced);
                    this.write_pending_frame = out;
                    this.write_pending_pos = 0;
                    this.write_pending_plaintext_len = buf.len();
                    // Loop to drain phase.
                }
                Ok(Status::BufError) => {
                    // flate2 returns BufError when it can't make
                    // progress without more output space. We sized
                    // `out` via `gzip_compress_bound(buf.len()) +
                    // SAFETY_MARGIN`, which by the deflateBound
                    // formula is always enough for a Sync flush of
                    // `buf.len()` input bytes. Hitting BufError
                    // here means the bound calculation drifted
                    // away from the underlying flate2 / zlib
                    // semantics — surface that specifically so a
                    // future maintainer knows where to look.
                    return Poll::Ready(Err(io::Error::other(
                        "gzip compress reported BufError despite \
                         pre-sized scratch (deflateBound mismatch?)",
                    )));
                }
                Err(e) => {
                    // Genuine codec failure inside flate2
                    // (corrupted internal state, etc.). Preserve
                    // the source error in the wrapping io::Error
                    // so the cause shows up in logs / debuggers.
                    return Poll::Ready(Err(io::Error::other(format!(
                        "gzip compress failed: {e}"
                    ))));
                }
            }
        }
    }

    fn poll_flush(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Result<(), io::Error>> {
        let this = self.get_mut();
        // Drain any compressed bytes pending in
        // write_pending_frame before delegating to the inner
        // flush. Otherwise the caller observes Ok(()) from
        // flush while compressed output we produced in a
        // previous poll_write is still in our own buffer — the
        // peer hasn't seen those bytes.
        match drain_pending_write(
            &mut this.write_pending_frame,
            &mut this.write_pending_pos,
            Pin::new(&mut this.inner),
            cx,
        ) {
            Poll::Ready(Ok(())) => {}
            other => return other,
        }
        Pin::new(&mut this.inner).poll_flush(cx)
    }

    fn poll_shutdown(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Result<(), io::Error>> {
        let this = self.get_mut();
        // Same as flush: drain pending compressed bytes first.
        // Otherwise shutdown silently truncates the stream by
        // closing the inner before the last frame's bytes were
        // committed.
        match drain_pending_write(
            &mut this.write_pending_frame,
            &mut this.write_pending_pos,
            Pin::new(&mut this.inner),
            cx,
        ) {
            Poll::Ready(Ok(())) => {}
            other => return other,
        }
        Pin::new(&mut this.inner).poll_shutdown(cx)
    }
}

// ============================================================
// Lz4Stream
// ============================================================

pub struct Lz4Stream<S> {
    inner: S,
    read_raw_buf: Vec<u8>,
    /// The buffer size at the last time `try_decode_lz4_frame`
    /// was called with the current incomplete frame. Used to
    /// gate the next redecode behind meaningful new bytes —
    /// see [`LZ4_REDECODE_MIN_GROWTH`]. Reset to zero
    /// every time the buffer goes empty (frame fully consumed)
    /// or shrinks below the watermark.
    read_last_decode_attempt_size: usize,
    read_plaintext: Vec<u8>,
    read_plaintext_pos: usize,
    write_pending_frame: Vec<u8>,
    write_pending_pos: usize,
    write_pending_plaintext_len: usize,
}

impl<S> Lz4Stream<S> {
    pub fn new(inner: S) -> Self {
        Self {
            inner,
            read_raw_buf: Vec::new(),
            read_last_decode_attempt_size: 0,
            read_plaintext: Vec::new(),
            read_plaintext_pos: 0,
            write_pending_frame: Vec::new(),
            write_pending_pos: 0,
            write_pending_plaintext_len: 0,
        }
    }

    pub fn inner(&self) -> &S {
        &self.inner
    }

    pub fn into_inner(self) -> S {
        self.inner
    }
}

/// LZ4F wire-format magic (little-endian 0x184D2204).
const LZ4F_MAGIC: [u8; 4] = [0x04, 0x22, 0x4D, 0x18];

/// Max bytes we'll accumulate trying to decode one LZ4F frame
/// before giving up. Hotline transactions are typically <1 KiB
/// and the spec ceiling is the same as the AEAD limit (16
/// MiB); past that, an undecodable buffer almost certainly
/// means a truly corrupted frame, and we surface
/// `InvalidData` to tear the connection down rather than wedge
/// or OOM.
const LZ4_MAX_FRAME_ACCUM: usize = 16 * 1024 * 1024;

/// Minimum extra bytes the inner stream must deliver before we
/// re-attempt the (allocating, full-buffer) LZ4F decode.
///
/// `try_decode_lz4_frame` is single-shot — it builds a fresh
/// `FrameDecoder` and pumps it via a bounded `decoder.read(...)`
/// loop over the whole accumulated buffer (the read loop
/// supersedes the earlier `read_to_end` shape — the bound is
/// the zip-bomb guard added per R3.3.d feedback). `lz4_flex`
/// doesn't expose mid-frame state we could resume from, so each
/// retry re-parses from byte zero. Under chunked inner reads /
/// backpressure the previous design called the decoder on every
/// `poll_read` and that's O(n²) per frame.
///
/// This watermark caps the number of redecodes per frame to
/// `O(log frame_size)`: each retry requires at least 50%
/// growth (or 1 KiB, whichever is larger) over the previous
/// attempt's buffer size. The trade-off is one Pending bounce
/// per growth step in the worst case — bounded latency for
/// bounded CPU. Bytes already arrived stay in the buffer; the
/// next inner read just adds to them and eventually crosses
/// the threshold.
///
/// 1 KiB matches typical Hotline payload sizes; frames smaller
/// than that decode in one shot regardless of the watermark
/// (a 1 KiB buffer easily contains the whole frame).
const LZ4_REDECODE_MIN_GROWTH: usize = 1024;

/// `Read` adapter over a `&[u8]` slice that returns
/// `WouldBlock` (rather than `Ok(0)`) when the buffer is
/// exhausted. Mirrors hxcompress's `Lz4InputBuf` pattern.
///
/// Why this matters: `lz4_flex::frame::FrameDecoder` calls
/// `read` on its inner reader until it has enough bytes to
/// complete a frame; when the reader returns `Ok(0)` (the
/// default for an exhausted `std::io::Cursor`), the decoder's
/// `read_to_end` interprets it as a clean EOF and returns
/// successfully with whatever plaintext it managed to
/// produce. That mis-classifies a *truncated* frame as a
/// successful decode — if the truncation happened to fall on
/// a block boundary the decoder will have emitted some bytes,
/// and the caller drains those plus the consumed prefix,
/// leaving the partial tail orphaned in `read_raw_buf`. The
/// next iteration starts past the magic and either errors
/// (`WrongMagicNumber`) or, worse, silently mis-decodes.
///
/// `Err(WouldBlock)` instead tells lz4_flex "no more bytes
/// right now, please ask again." `read_to_end` propagates the
/// error verbatim, and we map any error from the decoder back
/// to `Ok(None)` ("frame incomplete, need more bytes"). Real
/// malformed frames trip the magic check at the entry point;
/// the `LZ4_MAX_FRAME_ACCUM` ceiling guards against truly
/// garbled input we can't classify.
struct Lz4PartialReader<'a> {
    bytes: &'a [u8],
    pos: usize,
}

impl<'a> std::io::Read for Lz4PartialReader<'a> {
    fn read(&mut self, dst: &mut [u8]) -> std::io::Result<usize> {
        let avail = self.bytes.len() - self.pos;
        if avail == 0 {
            return Err(std::io::Error::new(
                std::io::ErrorKind::WouldBlock,
                "lz4 partial reader exhausted — feed more bytes",
            ));
        }
        let n = avail.min(dst.len());
        dst[..n].copy_from_slice(&self.bytes[self.pos..self.pos + n]);
        self.pos += n;
        Ok(n)
    }
}

/// Try to decode one complete LZ4F frame from `bytes`. Returns
/// `Ok(Some((plaintext, frame_size)))` on success, where
/// `frame_size` is how many bytes the frame consumed from the
/// front of `bytes`. Returns `Ok(None)` if more bytes are
/// needed (frame incomplete). `Err` on a malformed frame.
fn try_decode_lz4_frame(bytes: &[u8]) -> io::Result<Option<(Vec<u8>, usize)>> {
    use std::io::Read;

    if bytes.len() < LZ4F_MAGIC.len() {
        return Ok(None);
    }
    if bytes[..LZ4F_MAGIC.len()] != LZ4F_MAGIC {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "lz4 frame magic mismatch",
        ));
    }

    // Bounded read loop instead of `read_to_end` — LZ4 has a
    // very high theoretical compression ratio (one byte of input
    // can expand to ~255 bytes of output via RLE-style runs), so
    // a small frame can decompress into many GiB if we let the
    // decoder write into an unbounded Vec. Reject the frame as
    // soon as the decompressed total crosses
    // [`COMPRESS_WRITE_INPUT_MAX`] — the per-call write cap,
    // which is [`COMPRESS_WRITE_HEADROOM`] below the AEAD frame
    // limit (and therefore below the read-side ceiling). Anything
    // legitimate (Hotline transactions cap at MAX_BODY_LEN = 1
    // MiB) is comfortably below.
    let reader = Lz4PartialReader { bytes, pos: 0 };
    let mut decoder = lz4_flex::frame::FrameDecoder::new(reader);
    let mut plain = Vec::new();
    let mut chunk = [0u8; DECODE_READ_CHUNK];
    loop {
        match decoder.read(&mut chunk) {
            Ok(0) => {
                // End of frame.
                let consumed = decoder.into_inner().pos;
                return Ok(Some((plain, consumed)));
            }
            Ok(n) => {
                // Cap the decompressed total before we extend.
                if plain.len() + n > COMPRESS_WRITE_INPUT_MAX {
                    return Err(io::Error::new(
                        io::ErrorKind::InvalidData,
                        format!(
                            "lz4 frame decompressed output exceeds \
                             {COMPRESS_WRITE_INPUT_MAX} bytes (zip-bomb \
                             guard)"
                        ),
                    ));
                }
                plain.extend_from_slice(&chunk[..n]);
            }
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                // Our Lz4PartialReader's signal that the input
                // is incomplete. The caller will append more
                // wire bytes and retry.
                return Ok(None);
            }
            Err(e) => {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    format!("lz4 frame decode failed: {e}"),
                ));
            }
        }
    }
}

impl<S: AsyncRead + Unpin> AsyncRead for Lz4Stream<S> {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut ReadBuf<'_>,
    ) -> Poll<io::Result<()>> {
        let this = self.get_mut();

        loop {
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

            if !this.read_raw_buf.is_empty() {
                // Throttle the per-poll redecode. The previous
                // design called try_decode_lz4_frame on EVERY
                // poll_read while a frame was incomplete, which
                // is O(n²) per frame under chunked inner reads /
                // backpressure (each retry re-parses the whole
                // accumulated buffer). lz4_flex doesn't expose
                // mid-decode state we could resume from, so the
                // cleanest cap is: require the buffer to have
                // grown by at least LZ4_REDECODE_MIN_GROWTH (or
                // 50%, whichever is larger) over the previous
                // attempt before paying the redecode cost again.
                // Bytes already arrived stay buffered; the next
                // inner read appends to them and eventually
                // crosses the threshold. Worst case is
                // O(log frame_size) decodes per frame instead of
                // O(frame_size).
                let prev = this.read_last_decode_attempt_size;
                let grew_enough = prev == 0
                    || this.read_raw_buf.len() >= prev + LZ4_REDECODE_MIN_GROWTH
                    || this.read_raw_buf.len() >= prev * 3 / 2;
                if grew_enough {
                    this.read_last_decode_attempt_size = this.read_raw_buf.len();
                    match try_decode_lz4_frame(&this.read_raw_buf)? {
                        Some((plain, consumed)) => {
                            this.read_raw_buf.drain(..consumed);
                            this.read_last_decode_attempt_size = 0;
                            this.read_plaintext = plain;
                            this.read_plaintext_pos = 0;
                            continue;
                        }
                        None => {
                            if this.read_raw_buf.len() > LZ4_MAX_FRAME_ACCUM {
                                return Poll::Ready(Err(io::Error::new(
                                    io::ErrorKind::InvalidData,
                                    format!(
                                        "lz4: accumulated {} bytes without \
                                         decoding a frame; giving up",
                                        this.read_raw_buf.len()
                                    ),
                                )));
                            }
                            // Fall through to pull from inner.
                        }
                    }
                }
                // Either we just attempted decode and need more
                // bytes, or growth didn't justify a fresh attempt
                // yet. Either way: fall through to pull from inner.
            }

            // Read directly into read_raw_buf's spare capacity
            // (resize / read / truncate) — same shape as
            // AeadStream + GzipStream. Avoids the per-poll
            // scratch Vec + copy in the previous design.
            let len_before = this.read_raw_buf.len();
            this.read_raw_buf.resize(len_before + DECODE_READ_CHUNK, 0);
            let mut tmp = ReadBuf::new(&mut this.read_raw_buf[len_before..]);
            match Pin::new(&mut this.inner).poll_read(cx, &mut tmp) {
                Poll::Ready(Ok(())) => {
                    let got = tmp.filled().len();
                    this.read_raw_buf.truncate(len_before + got);
                    if got == 0 {
                        if this.read_raw_buf.is_empty() {
                            return Poll::Ready(Ok(()));
                        }
                        // EOF with buffered bytes. The watermark
                        // throttle might have skipped a decode
                        // attempt that would have succeeded at
                        // the current buffer size — try once
                        // more here before reporting truncation.
                        // If it still won't decode, the frame is
                        // genuinely incomplete and we surface
                        // UnexpectedEof so callers see the
                        // truncation instead of a silent
                        // shutdown.
                        let prev = this.read_last_decode_attempt_size;
                        if this.read_raw_buf.len() != prev {
                            this.read_last_decode_attempt_size = this.read_raw_buf.len();
                            if let Some((plain, consumed)) =
                                try_decode_lz4_frame(&this.read_raw_buf)?
                            {
                                this.read_raw_buf.drain(..consumed);
                                this.read_last_decode_attempt_size = 0;
                                this.read_plaintext = plain;
                                this.read_plaintext_pos = 0;
                                continue;
                            }
                        }
                        return Poll::Ready(Err(io::Error::new(
                            io::ErrorKind::UnexpectedEof,
                            "lz4 stream EOF mid-frame",
                        )));
                    }
                    // Hard-cap check immediately after extend.
                    // The post-decode-no-progress check at the
                    // top of the loop would let the buffer
                    // overshoot the 16 MiB ceiling because the
                    // redecode-watermark throttles decode
                    // attempts (e.g. prev=12 MiB requires
                    // growing to 18 MiB before the next try).
                    // Apply the cap here so the per-stream
                    // accumulation limit is enforced regardless
                    // of when the decoder next runs.
                    if this.read_raw_buf.len() > LZ4_MAX_FRAME_ACCUM {
                        return Poll::Ready(Err(io::Error::new(
                            io::ErrorKind::InvalidData,
                            format!(
                                "lz4: accumulated {} bytes without \
                                 decoding a frame; giving up",
                                this.read_raw_buf.len()
                            ),
                        )));
                    }
                }
                Poll::Pending => {
                    // We pre-grew read_raw_buf by DECODE_READ_CHUNK
                    // for the read; on Pending those bytes are
                    // garbage padding and we must roll back the
                    // resize before doing anything else.
                    this.read_raw_buf.truncate(len_before);
                    // Inner is parked. If we have buffered bytes
                    // we haven't tried decoding at this size yet
                    // (the watermark throttle skipped them on
                    // entry), force one decode attempt before
                    // returning Pending. Otherwise a frame that
                    // becomes complete on a small below-watermark
                    // chunk would stall: inner.poll_read parks
                    // waiting for more bytes that may never come,
                    // and we never get the chance to notice the
                    // frame is already decodable.
                    //
                    // The O(n²) protection still holds: we only
                    // pay one decode attempt per Pending bounce,
                    // and only when there's strictly new content
                    // since the last attempt. The amortised cost
                    // is identical to the watermark-only case
                    // for the common "lots of bytes per poll"
                    // path; this branch only kicks in when the
                    // peer is feeding us in dribs.
                    let prev = this.read_last_decode_attempt_size;
                    if !this.read_raw_buf.is_empty() && this.read_raw_buf.len() != prev {
                        this.read_last_decode_attempt_size = this.read_raw_buf.len();
                        match try_decode_lz4_frame(&this.read_raw_buf)? {
                            Some((plain, consumed)) => {
                                this.read_raw_buf.drain(..consumed);
                                this.read_last_decode_attempt_size = 0;
                                this.read_plaintext = plain;
                                this.read_plaintext_pos = 0;
                                continue;
                            }
                            None => { /* still incomplete; fall through */ }
                        }
                    }
                    return Poll::Pending;
                }
                Poll::Ready(Err(e)) => {
                    this.read_raw_buf.truncate(len_before);
                    return Poll::Ready(Err(e));
                }
            }
        }
    }
}

impl<S: AsyncWrite + Unpin> AsyncWrite for Lz4Stream<S> {
    fn poll_write(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<Result<usize, io::Error>> {
        use std::io::Write;
        let this = self.get_mut();

        loop {
            // Phase 1: drain.
            if this.write_pending_pos < this.write_pending_frame.len() {
                let to_send = &this.write_pending_frame[this.write_pending_pos..];
                match Pin::new(&mut this.inner).poll_write(cx, to_send) {
                    Poll::Ready(Ok(0)) => {
                        return Poll::Ready(Err(io::Error::new(
                            io::ErrorKind::WriteZero,
                            "lz4 inner write returned 0 bytes",
                        )));
                    }
                    Poll::Ready(Ok(n)) => {
                        this.write_pending_pos += n;
                        continue;
                    }
                    Poll::Ready(Err(e)) => return Poll::Ready(Err(e)),
                    Poll::Pending => return Poll::Pending,
                }
            }

            if !this.write_pending_frame.is_empty() {
                let plt_len = this.write_pending_plaintext_len;
                this.write_pending_frame.clear();
                this.write_pending_pos = 0;
                this.write_pending_plaintext_len = 0;
                return Poll::Ready(Ok(plt_len));
            }

            // Phase 2: encode `buf` into a new LZ4F frame.
            if buf.is_empty() {
                return Poll::Ready(Ok(0));
            }
            // DoS-safety ceiling — see COMPRESS_WRITE_INPUT_MAX.
            if buf.len() > COMPRESS_WRITE_INPUT_MAX {
                return Poll::Ready(Err(oversized_write_error("lz4", buf.len())));
            }
            let mut framed = Vec::with_capacity(buf.len() + 32);
            {
                let mut enc = lz4_flex::frame::FrameEncoder::new(&mut framed);
                if let Err(e) = enc.write_all(buf) {
                    return Poll::Ready(Err(io::Error::other(format!(
                        "lz4 frame encode failed: {e}"
                    ))));
                }
                if let Err(e) = enc.finish() {
                    return Poll::Ready(Err(io::Error::other(format!(
                        "lz4 frame finalize failed: {e}"
                    ))));
                }
            }
            this.write_pending_frame = framed;
            this.write_pending_pos = 0;
            this.write_pending_plaintext_len = buf.len();
            // Loop to drain.
        }
    }

    fn poll_flush(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Result<(), io::Error>> {
        let this = self.get_mut();
        // Drain any compressed bytes pending in
        // write_pending_frame before delegating to the inner
        // flush. Otherwise the caller observes Ok(()) from
        // flush while compressed output we produced in a
        // previous poll_write is still in our own buffer — the
        // peer hasn't seen those bytes.
        match drain_pending_write(
            &mut this.write_pending_frame,
            &mut this.write_pending_pos,
            Pin::new(&mut this.inner),
            cx,
        ) {
            Poll::Ready(Ok(())) => {}
            other => return other,
        }
        Pin::new(&mut this.inner).poll_flush(cx)
    }

    fn poll_shutdown(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Result<(), io::Error>> {
        let this = self.get_mut();
        // Same as flush: drain pending compressed bytes first.
        // Otherwise shutdown silently truncates the stream by
        // closing the inner before the last frame's bytes were
        // committed.
        match drain_pending_write(
            &mut this.write_pending_frame,
            &mut this.write_pending_pos,
            Pin::new(&mut this.inner),
            cx,
        ) {
            Poll::Ready(Ok(())) => {}
            other => return other,
        }
        Pin::new(&mut this.inner).poll_shutdown(cx)
    }
}

// ============================================================
// ZstdStream
// ============================================================

pub struct ZstdStream<S> {
    inner: S,
    read_decoder: zstd::stream::raw::Decoder<'static>,
    read_raw_buf: Vec<u8>,
    read_plaintext: Vec<u8>,
    read_plaintext_pos: usize,
    write_pending_frame: Vec<u8>,
    write_pending_pos: usize,
    write_pending_plaintext_len: usize,
}

impl<S> ZstdStream<S> {
    pub fn new(inner: S) -> io::Result<Self> {
        let read_decoder = zstd::stream::raw::Decoder::new().map_err(io::Error::other)?;
        Ok(Self {
            inner,
            read_decoder,
            read_raw_buf: Vec::new(),
            read_plaintext: Vec::new(),
            read_plaintext_pos: 0,
            write_pending_frame: Vec::new(),
            write_pending_pos: 0,
            write_pending_plaintext_len: 0,
        })
    }

    pub fn inner(&self) -> &S {
        &self.inner
    }

    pub fn into_inner(self) -> S {
        self.inner
    }
}

impl<S: AsyncRead + Unpin> AsyncRead for ZstdStream<S> {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut ReadBuf<'_>,
    ) -> Poll<io::Result<()>> {
        // `Operation` is the trait that defines `Decoder::run`.
        // The use looks unused at a glance (the method call is
        // `this.read_decoder.run(...)`) but without the trait
        // in scope rustc errors with E0599 "no method named
        // `run` found for `Decoder<'_>`". Same load-bearing-
        // trait-import pattern as hxcompress's lib.rs.
        use zstd::stream::raw::Operation;
        let this = self.get_mut();

        loop {
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

            // Always attempt a decode step, even with empty
            // input. The zstd raw decoder can produce buffered
            // plaintext from internal state when called with
            // an empty InBuffer — the same "more output
            // available, call again with output space" model
            // GzipStream needs. Gating on
            // !read_raw_buf.is_empty() would orphan that
            // plaintext after the inner stream's bytes are
            // all consumed and yield a misleading clean-EOF.
            // Use read_plaintext itself as the decode scratch
            // — the drain loop above guarantees it's empty
            // here, so resize-then-decode-then-truncate reuses
            // the allocation across polls (first call
            // allocates DECODE_READ_CHUNK; subsequent calls
            // find the capacity already in place and resize is
            // a no-op). Same pattern as GzipStream.
            this.read_plaintext.resize(DECODE_READ_CHUNK, 0);
            let mut in_buf = zstd::stream::raw::InBuffer::around(&this.read_raw_buf);
            let mut out_buf = zstd::stream::raw::OutBuffer::around(&mut this.read_plaintext);
            match this.read_decoder.run(&mut in_buf, &mut out_buf) {
                Ok(_) => {
                    let consumed = in_buf.pos();
                    let produced = out_buf.pos();
                    if consumed > 0 {
                        this.read_raw_buf.drain(..consumed);
                    }
                    if produced > 0 {
                        this.read_plaintext.truncate(produced);
                        this.read_plaintext_pos = 0;
                        continue;
                    }
                    // No progress — shrink back so the next
                    // drain check sees an empty buffer.
                    this.read_plaintext.clear();
                    // No progress (no input consumed, no
                    // plaintext produced). Fall through to
                    // read more input. CRITICALLY: do NOT
                    // clear read_raw_buf; those bytes are
                    // real wire bytes that the next inner
                    // read will extend (a ZSTD frame header /
                    // body split across multiple inner reads
                    // is normal).
                }
                Err(e) => {
                    return Poll::Ready(Err(io::Error::new(
                        io::ErrorKind::InvalidData,
                        format!("zstd decode failed: {e}"),
                    )));
                }
            }

            // Read directly into read_raw_buf's spare capacity
            // — resize / read / truncate, same as GzipStream
            // and Lz4Stream. Avoids the per-poll scratch Vec +
            // copy in the previous design.
            let len_before = this.read_raw_buf.len();
            this.read_raw_buf.resize(len_before + DECODE_READ_CHUNK, 0);
            let mut tmp = ReadBuf::new(&mut this.read_raw_buf[len_before..]);
            match Pin::new(&mut this.inner).poll_read(cx, &mut tmp) {
                Poll::Ready(Ok(())) => {
                    let got = tmp.filled().len();
                    this.read_raw_buf.truncate(len_before + got);
                    if got == 0 {
                        // EOF. If there are still un-decoded
                        // compressed bytes buffered, the
                        // stream truncated mid-frame — surface
                        // as UnexpectedEof rather than masking
                        // it as a clean shutdown.
                        if this.read_raw_buf.is_empty() {
                            return Poll::Ready(Ok(()));
                        }
                        return Poll::Ready(Err(io::Error::new(
                            io::ErrorKind::UnexpectedEof,
                            "zstd stream EOF with undecoded bytes buffered",
                        )));
                    }
                    // Defensive ceiling: same threat model as
                    // GzipStream's check — a peer pushing bytes
                    // the zstd raw decoder can't make progress
                    // on would grow this buffer unbounded. 16
                    // MiB matches the AEAD / LZ4 caps so all
                    // transport layers fail at the same point.
                    if this.read_raw_buf.len() > COMPRESS_READ_BUF_CEILING {
                        return Poll::Ready(Err(io::Error::new(
                            io::ErrorKind::InvalidData,
                            format!(
                                "zstd: accumulated {} bytes without \
                                 decompressing; giving up to avoid OOM",
                                this.read_raw_buf.len()
                            ),
                        )));
                    }
                }
                Poll::Ready(Err(e)) => {
                    this.read_raw_buf.truncate(len_before);
                    return Poll::Ready(Err(e));
                }
                Poll::Pending => {
                    this.read_raw_buf.truncate(len_before);
                    return Poll::Pending;
                }
            }
        }
    }
}

impl<S: AsyncWrite + Unpin> AsyncWrite for ZstdStream<S> {
    fn poll_write(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<Result<usize, io::Error>> {
        let this = self.get_mut();

        loop {
            // Phase 1: drain.
            if this.write_pending_pos < this.write_pending_frame.len() {
                let to_send = &this.write_pending_frame[this.write_pending_pos..];
                match Pin::new(&mut this.inner).poll_write(cx, to_send) {
                    Poll::Ready(Ok(0)) => {
                        return Poll::Ready(Err(io::Error::new(
                            io::ErrorKind::WriteZero,
                            "zstd inner write returned 0 bytes",
                        )));
                    }
                    Poll::Ready(Ok(n)) => {
                        this.write_pending_pos += n;
                        continue;
                    }
                    Poll::Ready(Err(e)) => return Poll::Ready(Err(e)),
                    Poll::Pending => return Poll::Pending,
                }
            }

            if !this.write_pending_frame.is_empty() {
                let plt_len = this.write_pending_plaintext_len;
                this.write_pending_frame.clear();
                this.write_pending_pos = 0;
                this.write_pending_plaintext_len = 0;
                return Poll::Ready(Ok(plt_len));
            }

            // Phase 2: encode `buf` into a self-contained
            // ZSTD frame.
            if buf.is_empty() {
                return Poll::Ready(Ok(0));
            }
            // DoS-safety ceiling — see COMPRESS_WRITE_INPUT_MAX.
            if buf.len() > COMPRESS_WRITE_INPUT_MAX {
                return Poll::Ready(Err(oversized_write_error("zstd", buf.len())));
            }
            let framed = zstd::bulk::compress(buf, 0)
                .map_err(|e| io::Error::other(format!("zstd compress failed: {e}")))?;
            this.write_pending_frame = framed;
            this.write_pending_pos = 0;
            this.write_pending_plaintext_len = buf.len();
        }
    }

    fn poll_flush(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Result<(), io::Error>> {
        let this = self.get_mut();
        // Drain any compressed bytes pending in
        // write_pending_frame before delegating to the inner
        // flush. Otherwise the caller observes Ok(()) from
        // flush while compressed output we produced in a
        // previous poll_write is still in our own buffer — the
        // peer hasn't seen those bytes.
        match drain_pending_write(
            &mut this.write_pending_frame,
            &mut this.write_pending_pos,
            Pin::new(&mut this.inner),
            cx,
        ) {
            Poll::Ready(Ok(())) => {}
            other => return other,
        }
        Pin::new(&mut this.inner).poll_flush(cx)
    }

    fn poll_shutdown(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Result<(), io::Error>> {
        let this = self.get_mut();
        // Same as flush: drain pending compressed bytes first.
        // Otherwise shutdown silently truncates the stream by
        // closing the inner before the last frame's bytes were
        // committed.
        match drain_pending_write(
            &mut this.write_pending_frame,
            &mut this.write_pending_pos,
            Pin::new(&mut this.inner),
            cx,
        ) {
            Poll::Ready(Ok(())) => {}
            other => return other,
        }
        Pin::new(&mut this.inner).poll_shutdown(cx)
    }
}
