//! Round-trip tests for the R3.3.d compression adapters.
//!
//! Strategy mirrors the R3.3.c cipher tests: a `tokio::io::duplex`
//! pair, one side wrapped in the compressing adapter, the other
//! in the decompressing adapter. Plaintext written on one side
//! arrives byte-identical on the other.
//!
//! Each algorithm gets:
//!   * single-message round-trip
//!   * byte-at-a-time read (exercises the per-call decode loop)
//!   * multi-message persistence (decoder state survives across
//!     poll_read calls and frame boundaries)
//!   * compressible vs incompressible content (catches misuse
//!     of buffer sizing)

use hxnet::compress::{GzipStream, Lz4Stream, ZstdStream};
use tokio::io::{AsyncReadExt, AsyncWriteExt};

// ============================================================
// GzipStream
// ============================================================

fn gzip_pair() -> (
    GzipStream<tokio::io::DuplexStream>,
    GzipStream<tokio::io::DuplexStream>,
) {
    let (a, b) = tokio::io::duplex(256 * 1024);
    (GzipStream::new(a), GzipStream::new(b))
}

#[tokio::test]
async fn gzip_round_trip_single_message() {
    let (mut left, mut right) = gzip_pair();
    let plaintext = b"Hello over deflate / zlib!";

    left.write_all(plaintext).await.unwrap();
    left.flush().await.unwrap();

    let mut got = vec![0u8; plaintext.len()];
    right.read_exact(&mut got).await.unwrap();
    assert_eq!(got, plaintext);
}

#[tokio::test]
async fn gzip_round_trip_byte_by_byte_read() {
    let (mut left, mut right) = gzip_pair();
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
async fn gzip_multi_message_decoder_state_persists() {
    // Decoder must remember the deflate dictionary across
    // separate poll_read calls. Three writes, three reads in
    // order; if state got reset between sync-flush boundaries
    // the second message would decode as garbage.
    let (mut left, mut right) = gzip_pair();

    let m1 = b"first message";
    let m2 = b"second message after the first";
    let m3 = b"and a third one";
    left.write_all(m1).await.unwrap();
    left.write_all(m2).await.unwrap();
    left.write_all(m3).await.unwrap();
    left.flush().await.unwrap();

    let total = m1.len() + m2.len() + m3.len();
    let mut got = vec![0u8; total];
    right.read_exact(&mut got).await.unwrap();

    let mut expect = Vec::with_capacity(total);
    expect.extend_from_slice(m1);
    expect.extend_from_slice(m2);
    expect.extend_from_slice(m3);
    assert_eq!(got, expect);
}

#[tokio::test]
async fn gzip_round_trip_highly_compressible() {
    // Long run of identical bytes — deflate compresses this
    // aggressively; verifies the writer's scratch sizing
    // (+64) is enough for the sync-flushed compressed output.
    let (mut left, mut right) = gzip_pair();
    let plaintext = vec![0xAAu8; 4096];

    left.write_all(&plaintext).await.unwrap();
    left.flush().await.unwrap();

    let mut got = vec![0u8; plaintext.len()];
    right.read_exact(&mut got).await.unwrap();
    assert_eq!(got, plaintext);
}

// ============================================================
// Lz4Stream
// ============================================================

fn lz4_pair() -> (
    Lz4Stream<tokio::io::DuplexStream>,
    Lz4Stream<tokio::io::DuplexStream>,
) {
    let (a, b) = tokio::io::duplex(256 * 1024);
    (Lz4Stream::new(a), Lz4Stream::new(b))
}

#[tokio::test]
async fn lz4_round_trip_single_message() {
    let (mut left, mut right) = lz4_pair();
    let plaintext = b"Hello over LZ4F!";

    left.write_all(plaintext).await.unwrap();
    left.flush().await.unwrap();

    let mut got = vec![0u8; plaintext.len()];
    right.read_exact(&mut got).await.unwrap();
    assert_eq!(got, plaintext);
}

#[tokio::test]
async fn lz4_round_trip_byte_by_byte_read() {
    let (mut left, mut right) = lz4_pair();
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
async fn lz4_round_trip_multiple_frames_in_sequence() {
    // Each write becomes one LZ4F frame on the wire. Reader
    // must drain all three frames in order.
    let (mut left, mut right) = lz4_pair();

    let m1 = b"first lz4 frame";
    let m2 = b"second lz4 frame with extra content";
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
async fn lz4_round_trip_random_content() {
    // Pseudo-random bytes — LZ4 doesn't compress these well,
    // but must still round-trip exactly.
    let (mut left, mut right) = lz4_pair();
    let mut plaintext = vec![0u8; 1024];
    for (i, b) in plaintext.iter_mut().enumerate() {
        *b = ((i.wrapping_mul(0x9E37) ^ 0xDEAD) & 0xFF) as u8;
    }

    left.write_all(&plaintext).await.unwrap();
    left.flush().await.unwrap();

    let mut got = vec![0u8; plaintext.len()];
    right.read_exact(&mut got).await.unwrap();
    assert_eq!(got, plaintext);
}

// ============================================================
// ZstdStream
// ============================================================

fn zstd_pair() -> (
    ZstdStream<tokio::io::DuplexStream>,
    ZstdStream<tokio::io::DuplexStream>,
) {
    let (a, b) = tokio::io::duplex(256 * 1024);
    (
        ZstdStream::new(a).expect("zstd decoder init"),
        ZstdStream::new(b).expect("zstd decoder init"),
    )
}

#[tokio::test]
async fn zstd_round_trip_single_message() {
    let (mut left, mut right) = zstd_pair();
    let plaintext = b"Hello over ZSTD!";

    left.write_all(plaintext).await.unwrap();
    left.flush().await.unwrap();

    let mut got = vec![0u8; plaintext.len()];
    right.read_exact(&mut got).await.unwrap();
    assert_eq!(got, plaintext);
}

#[tokio::test]
async fn zstd_round_trip_byte_by_byte_read() {
    let (mut left, mut right) = zstd_pair();
    let plaintext: Vec<u8> = (0..150u8).collect();

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
async fn zstd_round_trip_multiple_frames_in_sequence() {
    let (mut left, mut right) = zstd_pair();

    let m1 = b"first zstd frame";
    let m2 = b"second one";
    let m3 = b"third and final";
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
async fn zstd_round_trip_highly_compressible() {
    let (mut left, mut right) = zstd_pair();
    let plaintext = vec![0x55u8; 4096];

    left.write_all(&plaintext).await.unwrap();
    left.flush().await.unwrap();

    let mut got = vec![0u8; plaintext.len()];
    right.read_exact(&mut got).await.unwrap();
    assert_eq!(got, plaintext);
}

// ============================================================
// Backpressure / chunking tests (R3.3.d feedback)
//
// Force the inner stream to accept only N bytes per
// poll_write and produce only N bytes per poll_read. A
// regression that re-compresses on partial write (or that
// drops partial frame bytes on read) would corrupt the
// stream — the round-trip would fail or hang.
// ============================================================

use std::io;
use std::pin::Pin;
use std::task::{Context, Poll};
use tokio::io::{AsyncRead, AsyncWrite, ReadBuf};

/// Inner-stream wrapper that caps per-poll byte counts. Mirror
/// of the helper in tests/cipher.rs.
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
    fn poll_flush(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Result<(), io::Error>> {
        Pin::new(&mut self.get_mut().inner).poll_flush(cx)
    }
    fn poll_shutdown(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Result<(), io::Error>> {
        Pin::new(&mut self.get_mut().inner).poll_shutdown(cx)
    }
}

#[tokio::test]
async fn gzip_round_trip_under_chunking_backpressure() {
    // Tiny duplex + chunk=5 on both directions. The writer's
    // compress() advances persistent deflate state on each
    // call; a regression that re-compresses on partial inner
    // write would garble the second half.
    let (a, b) = tokio::io::duplex(16);
    let mut left = GzipStream::new(ChunkingWrapper::new(a, 5));
    let mut right = GzipStream::new(ChunkingWrapper::new(b, 7));

    // Use read_to_end so the reader only completes when the
    // writer signals EOF (via drop). read_exact would let the
    // reader return early once it has 200 bytes; the writer
    // could still be draining its last chunks at that point,
    // and a duplex-side close would surface as BrokenPipe.
    let plaintext: Vec<u8> = (0..200u8).collect();
    let reader = tokio::spawn(async move {
        let mut got = Vec::new();
        right.read_to_end(&mut got).await.unwrap();
        got
    });

    left.write_all(&plaintext).await.unwrap();
    left.flush().await.unwrap();
    drop(left);

    let got = reader.await.unwrap();
    assert_eq!(got, plaintext);
}

#[tokio::test]
async fn lz4_round_trip_under_chunking_backpressure() {
    let (a, b) = tokio::io::duplex(16);
    let mut left = Lz4Stream::new(ChunkingWrapper::new(a, 5));
    let mut right = Lz4Stream::new(ChunkingWrapper::new(b, 7));

    // Use read_to_end so the reader only completes when the
    // writer signals EOF (via drop). read_exact would let the
    // reader return early once it has 200 bytes; the writer
    // could still be draining its last chunks at that point,
    // and a duplex-side close would surface as BrokenPipe.
    let plaintext: Vec<u8> = (0..200u8).collect();
    let reader = tokio::spawn(async move {
        let mut got = Vec::new();
        right.read_to_end(&mut got).await.unwrap();
        got
    });

    left.write_all(&plaintext).await.unwrap();
    left.flush().await.unwrap();
    drop(left);

    let got = reader.await.unwrap();
    assert_eq!(got, plaintext);
}

#[tokio::test]
async fn zstd_round_trip_under_chunking_backpressure() {
    let (a, b) = tokio::io::duplex(16);
    let mut left = ZstdStream::new(ChunkingWrapper::new(a, 5)).unwrap();
    let mut right = ZstdStream::new(ChunkingWrapper::new(b, 7)).unwrap();

    // Use read_to_end so the reader only completes when the
    // writer signals EOF (via drop). read_exact would let the
    // reader return early once it has 200 bytes; the writer
    // could still be draining its last chunks at that point,
    // and a duplex-side close would surface as BrokenPipe.
    let plaintext: Vec<u8> = (0..200u8).collect();
    let reader = tokio::spawn(async move {
        let mut got = Vec::new();
        right.read_to_end(&mut got).await.unwrap();
        got
    });

    left.write_all(&plaintext).await.unwrap();
    left.flush().await.unwrap();
    drop(left);

    let got = reader.await.unwrap();
    assert_eq!(got, plaintext);
}
