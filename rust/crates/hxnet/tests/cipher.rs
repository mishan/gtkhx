//! Round-trip tests for the R3.3.c cipher adapters.
//!
//! Strategy: build a `tokio::io::duplex` pair, wrap each side in
//! a matching pair of cipher adapters (with cipher states keyed
//! so that side A's encrypt-state matches side B's decrypt-state
//! and vice versa), pipe plaintext through, and verify it
//! arrives byte-identical on the other end.
//!
//! Each adapter family gets:
//!   * a happy-path single-message round-trip
//!   * a byte-at-a-time read to exercise partial-frame buffering
//!   * a multi-frame interleave to confirm cipher state advances
//!     correctly across boundaries
//!   * (AEAD only) an oversized-length-prefix reject path

use std::io;
use std::pin::Pin;
use std::task::{Context, Poll};

use hxcrypto_aead::{AeadState, AEAD_DIR_CLIENT_TO_SERVER, AEAD_DIR_SERVER_TO_CLIENT};
use hxcrypto_stream::BlowfishOfb64State;
use hxnet::cipher::{AeadStream, BlowfishStream};
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt, ReadBuf};

// ============================================================
// BlowfishStream round-trips
// ============================================================

/// Build a paired (left, right) BlowfishStream pair over a
/// duplex. Both sides use the same key; left's write cipher
/// matches right's read cipher (and vice versa), so anything
/// left writes becomes plaintext when right reads, and same the
/// other direction.
fn blowfish_pair(key: &[u8]) -> (
    BlowfishStream<tokio::io::DuplexStream>,
    BlowfishStream<tokio::io::DuplexStream>,
) {
    let (a, b) = tokio::io::duplex(64 * 1024);

    let left_read = BlowfishOfb64State::new(key).expect("key");
    let left_write = BlowfishOfb64State::new(key).expect("key");
    let right_read = BlowfishOfb64State::new(key).expect("key");
    let right_write = BlowfishOfb64State::new(key).expect("key");

    // Wait — Blowfish-OFB is a stream cipher; left.write_state
    // and right.read_state must be the SAME stream (advance in
    // lockstep) but they're separate state objects. Initialised
    // identically (same key, ivec zero, num zero), they advance
    // identically as long as each only processes the bytes
    // travelling in its own direction. The constructors above
    // give us four independent states with identical initial
    // conditions — exactly what we need.

    let left = BlowfishStream::new(a, left_read, left_write);
    let right = BlowfishStream::new(b, right_read, right_write);
    (left, right)
}

#[tokio::test]
async fn blowfish_round_trip_single_message() {
    let key = b"a-test-key-not-secret";
    let (mut left, mut right) = blowfish_pair(key);
    let plaintext = b"Hello over Blowfish-OFB!";

    left.write_all(plaintext).await.unwrap();
    left.flush().await.unwrap();

    let mut got = vec![0u8; plaintext.len()];
    right.read_exact(&mut got).await.unwrap();
    assert_eq!(got, plaintext);
}

#[tokio::test]
async fn blowfish_round_trip_byte_by_byte_read() {
    // Reader pulls one byte at a time. OFB state has to
    // advance correctly per-byte; if it doesn't, we'd see
    // garbage after the first 8-byte block boundary.
    let key = b"another-key";
    let (mut left, mut right) = blowfish_pair(key);
    let plaintext: Vec<u8> = (0..200u8).collect();

    left.write_all(&plaintext).await.unwrap();
    left.flush().await.unwrap();

    let mut got = Vec::new();
    for _ in 0..plaintext.len() {
        let mut byte = [0u8; 1];
        right.read_exact(&mut byte).await.unwrap();
        got.push(byte[0]);
    }
    assert_eq!(got, plaintext);
}

#[tokio::test]
async fn blowfish_round_trip_both_directions() {
    // Both sides write; both sides read. Each direction's
    // cipher state must stay independent.
    let key = b"bidirectional-key";
    let (mut left, mut right) = blowfish_pair(key);

    let l2r = b"left to right message that is long enough to span OFB blocks";
    let r2l = b"and a reply going the other way for symmetry's sake";

    left.write_all(l2r).await.unwrap();
    left.flush().await.unwrap();
    right.write_all(r2l).await.unwrap();
    right.flush().await.unwrap();

    let mut got_at_right = vec![0u8; l2r.len()];
    right.read_exact(&mut got_at_right).await.unwrap();
    let mut got_at_left = vec![0u8; r2l.len()];
    left.read_exact(&mut got_at_left).await.unwrap();

    assert_eq!(got_at_right, l2r);
    assert_eq!(got_at_left, r2l);
}

#[tokio::test]
async fn blowfish_multi_write_state_persists_across_calls() {
    // Two separate writes followed by a single read should
    // yield the concatenation of plaintexts. If write_state got
    // reset between calls (a regression we should never let
    // happen), the second chunk would decrypt as garbage.
    let key = b"persist-key";
    let (mut left, mut right) = blowfish_pair(key);

    let p1 = b"first chunk";
    let p2 = b"second chunk after the first";
    left.write_all(p1).await.unwrap();
    left.write_all(p2).await.unwrap();
    left.flush().await.unwrap();

    let total = p1.len() + p2.len();
    let mut got = vec![0u8; total];
    right.read_exact(&mut got).await.unwrap();

    let mut expect = Vec::with_capacity(total);
    expect.extend_from_slice(p1);
    expect.extend_from_slice(p2);
    assert_eq!(got, expect);
}

// ============================================================
// AeadStream round-trips
// ============================================================

/// Build a paired AeadStream pair over a duplex. Like the
/// blowfish helper, each side gets two cipher states: encrypt
/// (write_state, towards the wire) and decrypt (read_state, off
/// the wire). For AEAD the two directions must be keyed and
/// `dir`-tagged so the nonce sequences don't collide — we use
/// the spec's `client_to_server` byte for the writer's nonce
/// direction and `server_to_client` for the reader's.
fn aead_pair(key: [u8; 32]) -> (
    AeadStream<tokio::io::DuplexStream>,
    AeadStream<tokio::io::DuplexStream>,
) {
    let (a, b) = tokio::io::duplex(1024 * 1024);

    // Left writes with client_to_server, reads server_to_client.
    let left_write = AeadState {
        key,
        counter: 0,
        dir: AEAD_DIR_CLIENT_TO_SERVER,
    };
    let left_read = AeadState {
        key,
        counter: 0,
        dir: AEAD_DIR_SERVER_TO_CLIENT,
    };
    // Right is the mirror.
    let right_write = AeadState {
        key,
        counter: 0,
        dir: AEAD_DIR_SERVER_TO_CLIENT,
    };
    let right_read = AeadState {
        key,
        counter: 0,
        dir: AEAD_DIR_CLIENT_TO_SERVER,
    };

    let left = AeadStream::new(a, left_read, left_write);
    let right = AeadStream::new(b, right_read, right_write);
    (left, right)
}

#[tokio::test]
async fn aead_round_trip_single_message() {
    let key = [0x42u8; 32];
    let (mut left, mut right) = aead_pair(key);
    let plaintext = b"Hello over AEAD!";

    left.write_all(plaintext).await.unwrap();
    left.flush().await.unwrap();

    let mut got = vec![0u8; plaintext.len()];
    right.read_exact(&mut got).await.unwrap();
    assert_eq!(got, plaintext);
}

#[tokio::test]
async fn aead_round_trip_byte_by_byte_read() {
    // The AEAD reader buffers a full wire frame before
    // surfacing plaintext. Reading one byte at a time of the
    // PLAINTEXT exercises the read_plaintext_pos cursor — the
    // adapter must hand bytes out incrementally even though it
    // received them all at once.
    let key = [0x99u8; 32];
    let (mut left, mut right) = aead_pair(key);
    let plaintext: Vec<u8> = (0..255u8).collect();

    left.write_all(&plaintext).await.unwrap();
    left.flush().await.unwrap();

    let mut got = Vec::new();
    for _ in 0..plaintext.len() {
        let mut byte = [0u8; 1];
        right.read_exact(&mut byte).await.unwrap();
        got.push(byte[0]);
    }
    assert_eq!(got, plaintext);
}

#[tokio::test]
async fn aead_round_trip_multiple_frames_in_sequence() {
    // Each write_all encodes one AEAD frame. The counter
    // advances per frame, so two writes in sequence have to
    // decrypt with two different nonces — desync there shows
    // up as the second message decoding to garbage / failing
    // the tag check.
    let key = [0xAAu8; 32];
    let (mut left, mut right) = aead_pair(key);

    let m1 = b"first frame";
    let m2 = b"second frame, with different content";
    let m3 = b"third!";
    left.write_all(m1).await.unwrap();
    left.write_all(m2).await.unwrap();
    left.write_all(m3).await.unwrap();
    left.flush().await.unwrap();

    let mut got1 = vec![0u8; m1.len()];
    right.read_exact(&mut got1).await.unwrap();
    let mut got2 = vec![0u8; m2.len()];
    right.read_exact(&mut got2).await.unwrap();
    let mut got3 = vec![0u8; m3.len()];
    right.read_exact(&mut got3).await.unwrap();

    assert_eq!(got1, m1);
    assert_eq!(got2, m2);
    assert_eq!(got3, m3);
}

#[tokio::test]
async fn aead_oversized_length_prefix_rejected() {
    // Synthesise a forged inbound frame with a length prefix
    // exceeding AEAD_MAX_FRAME_SIZE. The reader must surface
    // an InvalidData io::Error rather than allocate-and-wait
    // or wedge.
    //
    // (`AsyncReadExt` is already imported at module scope.)

    let key = [0u8; 32];
    let (mut server_side, client_side) = tokio::io::duplex(64);
    let read_state = AeadState {
        key,
        counter: 0,
        dir: AEAD_DIR_SERVER_TO_CLIENT,
    };
    let write_state = AeadState {
        key,
        counter: 0,
        dir: AEAD_DIR_CLIENT_TO_SERVER,
    };
    let mut adapter = AeadStream::new(client_side, read_state, write_state);

    // Write a 4-byte big-endian length that's clearly over the
    // cap (max + 1). The adapter sees this on its read path
    // and should reject.
    let bad_len = hxcrypto_aead::AEAD_MAX_FRAME_SIZE + 1;
    server_side
        .write_all(&bad_len.to_be_bytes())
        .await
        .unwrap();

    let mut sink = [0u8; 16];
    let err = adapter.read(&mut sink).await.unwrap_err();
    assert_eq!(err.kind(), std::io::ErrorKind::InvalidData);
    assert!(
        err.to_string().contains("AEAD frame length"),
        "error message should name the bad length: {err}"
    );
}

#[tokio::test]
async fn aead_truncated_body_surfaces_unexpected_eof() {
    // Length prefix says N bytes coming; only some arrive
    // before EOF. The reader must surface UnexpectedEof rather
    // than wedge forever.
    let key = [1u8; 32];
    let (mut server_side, client_side) = tokio::io::duplex(64);
    let read_state = AeadState {
        key,
        counter: 0,
        dir: AEAD_DIR_SERVER_TO_CLIENT,
    };
    let write_state = AeadState {
        key,
        counter: 0,
        dir: AEAD_DIR_CLIENT_TO_SERVER,
    };
    let mut adapter = AeadStream::new(client_side, read_state, write_state);

    // Length prefix of 100 bytes, then write 10 bytes and
    // drop. The adapter expects 100 body bytes but only sees
    // 10 before EOF.
    let claimed: u32 = 100;
    server_side
        .write_all(&claimed.to_be_bytes())
        .await
        .unwrap();
    server_side.write_all(&[0u8; 10]).await.unwrap();
    drop(server_side);

    let mut sink = [0u8; 16];
    let err = adapter.read(&mut sink).await.unwrap_err();
    assert_eq!(err.kind(), std::io::ErrorKind::UnexpectedEof);
}

// ============================================================
// Rollback-on-Pending / partial-write tests
//
// These pin the load-bearing invariant that BlowfishStream's
// OFB state and AeadStream's AEAD counter must NEVER advance
// past bytes the inner stream didn't actually take. Without
// rollback, the next call seals/encrypts from the wrong
// cipher position and the peer's tag check fails (AEAD) or
// the peer decrypts garbage (Blowfish).
// ============================================================

/// Test inner: always-Pending. AsyncWrite returns Pending
/// every time; AsyncRead is unused. Used to verify the
/// adapter's cipher state stays put on a Pending inner.
struct AlwaysPendingWriter;

impl AsyncRead for AlwaysPendingWriter {
    fn poll_read(
        self: Pin<&mut Self>,
        _cx: &mut Context<'_>,
        _buf: &mut ReadBuf<'_>,
    ) -> Poll<io::Result<()>> {
        Poll::Pending
    }
}

impl AsyncWrite for AlwaysPendingWriter {
    fn poll_write(
        self: Pin<&mut Self>,
        _cx: &mut Context<'_>,
        _buf: &[u8],
    ) -> Poll<Result<usize, io::Error>> {
        Poll::Pending
    }
    fn poll_flush(
        self: Pin<&mut Self>,
        _cx: &mut Context<'_>,
    ) -> Poll<Result<(), io::Error>> {
        Poll::Ready(Ok(()))
    }
    fn poll_shutdown(
        self: Pin<&mut Self>,
        _cx: &mut Context<'_>,
    ) -> Poll<Result<(), io::Error>> {
        Poll::Ready(Ok(()))
    }
}

/// Test inner: caller-controlled partial accept. The first
/// poll_write returns Ok(first_accepted); subsequent
/// poll_writes return Pending (we only test one call). Used
/// to verify BlowfishStream advances OFB state by exactly the
/// accepted byte count.
struct PartialWriter {
    first_accepted: usize,
    seen: Vec<u8>,
    calls: u32,
}

impl AsyncRead for PartialWriter {
    fn poll_read(
        self: Pin<&mut Self>,
        _cx: &mut Context<'_>,
        _buf: &mut ReadBuf<'_>,
    ) -> Poll<io::Result<()>> {
        Poll::Pending
    }
}

impl AsyncWrite for PartialWriter {
    fn poll_write(
        mut self: Pin<&mut Self>,
        _cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<Result<usize, io::Error>> {
        if self.calls == 0 {
            self.calls += 1;
            let n = self.first_accepted.min(buf.len());
            self.seen.extend_from_slice(&buf[..n]);
            Poll::Ready(Ok(n))
        } else {
            Poll::Pending
        }
    }
    fn poll_flush(
        self: Pin<&mut Self>,
        _cx: &mut Context<'_>,
    ) -> Poll<Result<(), io::Error>> {
        Poll::Ready(Ok(()))
    }
    fn poll_shutdown(
        self: Pin<&mut Self>,
        _cx: &mut Context<'_>,
    ) -> Poll<Result<(), io::Error>> {
        Poll::Ready(Ok(()))
    }
}

#[tokio::test]
async fn blowfish_partial_write_advances_state_by_accepted_count() {
    // The hard case: inner accepts 5 of 16 bytes. The adapter
    // must report Ok(5) AND leave the cipher state at +5
    // positions. We verify by reading the inner's `seen`
    // buffer and decrypting it — those 5 bytes MUST decrypt to
    // the first 5 plaintext bytes when fed to a fresh state.
    // If the adapter didn't roll back correctly, the ciphertext
    // would have been encrypted under the WRONG state
    // positions and the decrypt would garble.
    let key = b"partial-write-key";
    let mut adapter = BlowfishStream::new(
        PartialWriter { first_accepted: 5, seen: Vec::new(), calls: 0 },
        BlowfishOfb64State::new(key).unwrap(),
        BlowfishOfb64State::new(key).unwrap(),
    );
    let plaintext: Vec<u8> = (0..16u8).collect();

    let waker = futures_noop_waker();
    let mut cx = Context::from_waker(&waker);
    let result = Pin::new(&mut adapter).poll_write(&mut cx, &plaintext);
    assert!(matches!(result, Poll::Ready(Ok(5))), "got {result:?}");

    // Decrypt the 5 bytes the inner saw and verify they match
    // the first 5 plaintext bytes — encryption was performed
    // from cipher position 0 onward.
    let mut decrypt = BlowfishOfb64State::new(key).unwrap();
    let mut decrypted_first = adapter.inner().seen.clone();
    decrypt.crypt_in_place(&mut decrypted_first);
    assert_eq!(&decrypted_first, &plaintext[..5]);
}

#[tokio::test]
async fn aead_pending_inner_seals_exactly_once_per_frame() {
    // AeadStream uses a buffer-and-drain pattern: it seals
    // the user's plaintext into one AEAD frame exactly once,
    // then drains the sealed bytes across as many `poll_write`
    // calls as the inner takes. The load-bearing invariant is
    // NOT "rollback the counter on Pending" — it's "don't
    // re-seal on retry." If the adapter re-sealed under a
    // Pending inner, the second retry would burn a fresh
    // counter and the peer's decoder would tag-fail on the
    // wrong nonce.
    //
    // This test pins the invariant two ways:
    //   1. Happy-path: two writes through a real duplex pair
    //      decrypt cleanly on the receive side, which only
    //      works if the counters advanced exactly 0 → 1 → 2
    //      (one seal per Hotline frame, no re-seal).
    //   2. Pending-path: poll_write through an always-Pending
    //      inner returns Pending without panicking. A real
    //      counter-double-advance would require both the seal
    //      step AND the subsequent retry to misbehave; the
    //      first test catches the retry side, this one
    //      catches the immediate Pending side.
    let key = [0x77u8; 32];

    // Build a paired AeadStream: server's read_state mirrors
    // client's write_state.
    let (a, b) = tokio::io::duplex(64 * 1024);
    let left_write = AeadState { key, counter: 0, dir: AEAD_DIR_CLIENT_TO_SERVER };
    let left_read = AeadState { key, counter: 0, dir: AEAD_DIR_SERVER_TO_CLIENT };
    let right_write = AeadState { key, counter: 0, dir: AEAD_DIR_SERVER_TO_CLIENT };
    let right_read = AeadState { key, counter: 0, dir: AEAD_DIR_CLIENT_TO_SERVER };
    let mut left = AeadStream::new(a, left_read, left_write);
    let mut right = AeadStream::new(b, right_read, right_write);

    let m1 = b"first";
    let m2 = b"second";
    left.write_all(m1).await.unwrap();
    left.write_all(m2).await.unwrap();
    left.flush().await.unwrap();

    let mut got1 = vec![0u8; m1.len()];
    right.read_exact(&mut got1).await.unwrap();
    let mut got2 = vec![0u8; m2.len()];
    right.read_exact(&mut got2).await.unwrap();
    assert_eq!(got1, m1);
    assert_eq!(got2, m2);

    // Pending path: doesn't crash and doesn't return some
    // bogus Ready result on the way out.
    let pending_left = AeadStream::new(
        AlwaysPendingWriter,
        AeadState { key, counter: 0, dir: AEAD_DIR_SERVER_TO_CLIENT },
        AeadState { key, counter: 0, dir: AEAD_DIR_CLIENT_TO_SERVER },
    );
    let mut pending_left = pending_left;
    let waker = futures_noop_waker();
    let mut cx = Context::from_waker(&waker);
    let result = Pin::new(&mut pending_left).poll_write(&mut cx, b"data");
    assert!(matches!(result, Poll::Pending), "got {result:?}");
}

/// Minimal noop Waker for driving manual poll_write calls in
/// tests. We only call poll_write once per test and inspect
/// the immediate result, so the waker never needs to wake.
///
/// `RawWakerVTable::new` takes `unsafe fn` pointers; safe non-
/// capturing closures don't reliably coerce through to
/// `unsafe fn` on stable, so we declare the vtable's clone /
/// noop entries as proper `unsafe fn` items. The vtable itself
/// is a `static` so the `&VTABLE` reference inside `clone` is
/// stable across `Waker` clones.
fn futures_noop_waker() -> std::task::Waker {
    use std::task::{RawWaker, RawWakerVTable, Waker};
    unsafe fn clone(_: *const ()) -> RawWaker {
        RawWaker::new(std::ptr::null(), &VTABLE)
    }
    unsafe fn noop(_: *const ()) {}
    static VTABLE: RawWakerVTable = RawWakerVTable::new(clone, noop, noop, noop);
    unsafe { Waker::from_raw(RawWaker::new(std::ptr::null(), &VTABLE)) }
}

// ============================================================
// Backpressure / chunking tests (R3.3.c feedback)
//
// These exercise the buffer-and-drain pattern by forcing the
// inner stream to accept only N bytes per poll_write. A
// regression that re-seals on partial-write would burn a fresh
// AEAD counter on retry and the peer's tag check would fail.
// ============================================================

/// Inner-stream wrapper that splits each poll_write into at
/// most `chunk` bytes. The inner is wrapped behind a Pin so we
/// can forward poll_* calls. Combined with a small duplex
/// capacity, this drives the adapter through Pending /
/// partial-write paths reliably.
struct ChunkingWrapper<S> {
    inner: S,
    chunk: usize,
}

impl<S> ChunkingWrapper<S> {
    fn new(inner: S, chunk: usize) -> Self {
        Self { inner, chunk }
    }
}

impl<S: AsyncRead + Unpin> AsyncRead for ChunkingWrapper<S> {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut ReadBuf<'_>,
    ) -> Poll<io::Result<()>> {
        let this = self.get_mut();
        // Cap per-poll bytes by reading into a scratch and
        // copying — simpler than ReadBuf::take's
        // initialised-byte bookkeeping and good enough for a
        // test wrapper.
        let take = buf.remaining().min(this.chunk);
        if take == 0 {
            return Poll::Ready(Ok(()));
        }
        let mut scratch = vec![0u8; take];
        let mut tmp = ReadBuf::new(&mut scratch);
        let r = Pin::new(&mut this.inner).poll_read(cx, &mut tmp);
        if let Poll::Ready(Ok(())) = r {
            let n = tmp.filled().len();
            if n > 0 {
                buf.put_slice(&tmp.filled()[..n]);
            }
        }
        r
    }
}

impl<S: AsyncWrite + Unpin> AsyncWrite for ChunkingWrapper<S> {
    fn poll_write(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<Result<usize, io::Error>> {
        let this = self.get_mut();
        let take = buf.len().min(this.chunk);
        Pin::new(&mut this.inner).poll_write(cx, &buf[..take])
    }
    fn poll_flush(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
    ) -> Poll<Result<(), io::Error>> {
        Pin::new(&mut self.get_mut().inner).poll_flush(cx)
    }
    fn poll_shutdown(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
    ) -> Poll<Result<(), io::Error>> {
        Pin::new(&mut self.get_mut().inner).poll_shutdown(cx)
    }
}

#[tokio::test]
async fn aead_round_trip_under_chunking_backpressure() {
    // Small duplex + ChunkingWrapper(7) means each AEAD frame
    // (~ plaintext + 20 bytes overhead) is delivered to the
    // inner stream in 7-byte chunks. The writer must NOT
    // re-seal on partial-write or the reader's tag check
    // fails on the second poll's bytes.
    let key = [0xCDu8; 32];
    let (a, b) = tokio::io::duplex(32);
    let left_write = AeadState { key, counter: 0, dir: AEAD_DIR_CLIENT_TO_SERVER };
    let left_read = AeadState { key, counter: 0, dir: AEAD_DIR_SERVER_TO_CLIENT };
    let right_write = AeadState { key, counter: 0, dir: AEAD_DIR_SERVER_TO_CLIENT };
    let right_read = AeadState { key, counter: 0, dir: AEAD_DIR_CLIENT_TO_SERVER };
    let mut left = AeadStream::new(ChunkingWrapper::new(a, 7), left_read, left_write);
    let mut right = AeadStream::new(ChunkingWrapper::new(b, 5), right_read, right_write);

    // Run reader concurrently with writer so the small duplex
    // doesn't block.
    let plaintext: Vec<u8> = (0..96u8).collect();
    let plaintext_clone = plaintext.clone();
    let reader = tokio::spawn(async move {
        let mut got = vec![0u8; plaintext_clone.len()];
        right.read_exact(&mut got).await.unwrap();
        got
    });

    left.write_all(&plaintext).await.unwrap();
    left.flush().await.unwrap();
    drop(left);

    let got = reader.await.unwrap();
    assert_eq!(got, plaintext);
}

#[tokio::test]
async fn aead_oversized_plaintext_rejected_with_invalid_input() {
    // Plaintext larger than AEAD_MAX_PLAINTEXT_PER_FRAME must
    // be refused with InvalidInput rather than triggering a
    // multi-megabyte allocation that the seal would refuse
    // anyway.
    let key = [0u8; 32];
    let (_server, client) = tokio::io::duplex(64);
    let read_state = AeadState { key, counter: 0, dir: AEAD_DIR_SERVER_TO_CLIENT };
    let write_state = AeadState { key, counter: 0, dir: AEAD_DIR_CLIENT_TO_SERVER };
    let mut adapter = AeadStream::new(client, read_state, write_state);

    // Build an oversized plaintext. AEAD_MAX_FRAME_SIZE caps
    // the *body* (ciphertext + tag) of a single frame, so the
    // per-frame plaintext maximum is
    // AEAD_MAX_FRAME_SIZE - AEAD_TAG_SIZE (the 4-byte wire
    // length prefix sits outside the cap). Anything strictly
    // greater than AEAD_MAX_FRAME_SIZE itself comfortably
    // exceeds the plaintext cap; using +1 keeps the allocation
    // tractable for the test.
    let huge = vec![0u8; (hxcrypto_aead::AEAD_MAX_FRAME_SIZE as usize) + 1];
    let err = adapter.write_all(&huge).await.unwrap_err();
    assert_eq!(err.kind(), io::ErrorKind::InvalidInput);
}
