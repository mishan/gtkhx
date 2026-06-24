//! Round-trip tests for the R3.3.e-2 transform composition.
//!
//! Each test builds a paired duplex, layers the matching transform
//! stack on each side (cipher and/or compression), and verifies a
//! plaintext byte string written into one box can be read out the
//! other unchanged. The Connection actor itself is not involved
//! here — the focus is the [`crate::transform::compose`] entry
//! and the [`AsyncDuplex`] trait-object wiring.

use std::io;

use hxcrypto_aead::{AeadState, AEAD_DIR_CLIENT_TO_SERVER, AEAD_DIR_SERVER_TO_CLIENT};
use hxcrypto_stream::BlowfishOfb64State;
use hxnet::transform::{
    compose, BoxedDuplex, CipherKind, CipherLayer, CompressionKind, TransformStack,
};
use tokio::io::{AsyncReadExt, AsyncWriteExt};

/// Random-but-deterministic AEAD key. The same key is used for
/// both directions because the AeadState already disambiguates by
/// dir tag.
fn aead_key() -> [u8; 32] {
    let mut k = [0u8; 32];
    for (i, b) in k.iter_mut().enumerate() {
        *b = (i as u8).wrapping_mul(13).wrapping_add(7);
    }
    k
}

/// Build matched left/right cipher layers for the requested kind.
/// `left.write` decrypts with `right.read`, and vice versa.
fn build_cipher_pair(kind: CipherKind) -> (CipherLayer, CipherLayer) {
    match kind {
        CipherKind::None => (CipherLayer::None, CipherLayer::None),
        CipherKind::Blowfish => {
            // Shared key; the two directions get independent
            // BlowfishOfb64State instances seeded identically.
            // Left's write feeds right's read and vice versa, so
            // we cross-wire: left { read=L2R-init, write=R2L-init }
            // mirrors right { read=R2L-init, write=L2R-init }.
            let key: Vec<u8> = (0..16u8).collect();
            let l2r_init = BlowfishOfb64State::new(&key).expect("blowfish key");
            let r2l_init = BlowfishOfb64State::new(&key).expect("blowfish key");
            // Clone the same starting state for the paired side so
            // its read OFB position tracks ours from byte zero.
            let left = CipherLayer::Blowfish {
                read_state: r2l_init.clone(),
                write_state: l2r_init.clone(),
            };
            let right = CipherLayer::Blowfish {
                read_state: l2r_init,
                write_state: r2l_init,
            };
            (left, right)
        }
        CipherKind::ChaCha20Poly1305 => {
            let key = aead_key();
            // Left writes with CLIENT_TO_SERVER so right reads
            // with CLIENT_TO_SERVER; left reads with
            // SERVER_TO_CLIENT to match right's write.
            let left = CipherLayer::ChaCha20Poly1305 {
                read: AeadState {
                    key,
                    counter: 0,
                    dir: AEAD_DIR_SERVER_TO_CLIENT,
                },
                write: AeadState {
                    key,
                    counter: 0,
                    dir: AEAD_DIR_CLIENT_TO_SERVER,
                },
            };
            let right = CipherLayer::ChaCha20Poly1305 {
                read: AeadState {
                    key,
                    counter: 0,
                    dir: AEAD_DIR_CLIENT_TO_SERVER,
                },
                write: AeadState {
                    key,
                    counter: 0,
                    dir: AEAD_DIR_SERVER_TO_CLIENT,
                },
            };
            (left, right)
        }
    }
}

/// Run a single send-and-read round trip across a paired transform
/// stack. Writes `plaintext` from one side; reads it back from the
/// other and asserts byte-equality. The reader uses `read_to_end`
/// driven by the writer's `shutdown` half-close: compression-with-
/// EOF semantics (Gzip/Lz4 finalising their frames, Zstd flushing
/// its block) only fire when the encoder side is shut down, so we
/// read until that signal arrives rather than nailing down an
/// exact byte count up front.
async fn round_trip(
    cipher: CipherKind,
    compression: CompressionKind,
    plaintext: &[u8],
) -> io::Result<()> {
    let (left_inner, right_inner) = tokio::io::duplex(64 * 1024);
    let (left_cipher, right_cipher) = build_cipher_pair(cipher);

    let mut left: BoxedDuplex = compose(left_inner, left_cipher, compression)?;
    let mut right: BoxedDuplex = compose(right_inner, right_cipher, compression)?;

    // Writer task: send the plaintext, then half-close. half-close
    // matters for compression-with-EOF semantics (Gzip/Lz4 finish
    // their frames on shutdown; Zstd flushes its block).
    let writer_plaintext = plaintext.to_vec();
    let writer = tokio::spawn(async move {
        left.write_all(&writer_plaintext).await?;
        left.shutdown().await?;
        Ok::<_, io::Error>(())
    });

    // Reader: read until the writer half-closes.
    let mut buf = Vec::with_capacity(plaintext.len());
    right.read_to_end(&mut buf).await?;
    writer.await.unwrap()?;

    assert_eq!(buf, plaintext, "round-trip byte mismatch");
    Ok(())
}

// ============================================================
// Passthrough — no cipher, no compression.
// ============================================================

#[tokio::test]
async fn passthrough_round_trip() {
    round_trip(CipherKind::None, CompressionKind::None, b"Hello, hxnet")
        .await
        .unwrap();
}

#[tokio::test]
async fn passthrough_is_passthrough_predicate() {
    let stack = TransformStack {
        cipher: CipherKind::None,
        compression: CompressionKind::None,
    };
    assert!(stack.is_passthrough());
}

// ============================================================
// Cipher-only stacks.
// ============================================================

#[tokio::test]
async fn blowfish_only_round_trip() {
    round_trip(
        CipherKind::Blowfish,
        CompressionKind::None,
        b"Blowfish over duplex",
    )
    .await
    .unwrap();
}

#[tokio::test]
async fn aead_only_round_trip() {
    round_trip(
        CipherKind::ChaCha20Poly1305,
        CompressionKind::None,
        b"AEAD over duplex",
    )
    .await
    .unwrap();
}

// ============================================================
// Compression-only stacks.
// ============================================================

#[tokio::test]
async fn gzip_only_round_trip() {
    let payload = vec![b'X'; 4096];
    round_trip(CipherKind::None, CompressionKind::Gzip, &payload)
        .await
        .unwrap();
}

#[tokio::test]
async fn lz4_only_round_trip() {
    let payload = vec![b'Y'; 4096];
    round_trip(CipherKind::None, CompressionKind::Lz4, &payload)
        .await
        .unwrap();
}

#[tokio::test]
async fn zstd_only_round_trip() {
    let payload = vec![b'Z'; 4096];
    round_trip(CipherKind::None, CompressionKind::Zstd, &payload)
        .await
        .unwrap();
}

// ============================================================
// Full stacks — cipher + compression in HOPE order
// (compress, then encrypt on send; reverse on receive).
// ============================================================

#[tokio::test]
async fn aead_plus_gzip_round_trip() {
    let payload: Vec<u8> = (0..8192u32).flat_map(u32::to_be_bytes).collect();
    round_trip(
        CipherKind::ChaCha20Poly1305,
        CompressionKind::Gzip,
        &payload,
    )
    .await
    .unwrap();
}

#[tokio::test]
async fn aead_plus_lz4_round_trip() {
    let payload: Vec<u8> = (0..8192u32).flat_map(u32::to_be_bytes).collect();
    round_trip(CipherKind::ChaCha20Poly1305, CompressionKind::Lz4, &payload)
        .await
        .unwrap();
}

#[tokio::test]
async fn aead_plus_zstd_round_trip() {
    let payload: Vec<u8> = (0..8192u32).flat_map(u32::to_be_bytes).collect();
    round_trip(
        CipherKind::ChaCha20Poly1305,
        CompressionKind::Zstd,
        &payload,
    )
    .await
    .unwrap();
}

#[tokio::test]
async fn blowfish_plus_gzip_round_trip() {
    let payload: Vec<u8> = (0..2048u32).flat_map(u32::to_be_bytes).collect();
    round_trip(CipherKind::Blowfish, CompressionKind::Gzip, &payload)
        .await
        .unwrap();
}

// ============================================================
// Connection-actor integration: spawn the actor behind a
// BoxedDuplex and exchange a Hotline-shaped frame end-to-end
// through the trait-object boundary.
// ============================================================

#[tokio::test]
async fn connection_spawn_boxed_over_aead_plus_gzip() {
    use hxnet::{Connection, Event};

    let (left_inner, right_inner) = tokio::io::duplex(64 * 1024);
    let (left_cipher, right_cipher) = build_cipher_pair(CipherKind::ChaCha20Poly1305);

    // Left side: hand the Connection actor a BoxedDuplex composed
    // of the full transform stack. The actor will see plaintext
    // Hotline frames.
    let left_box: BoxedDuplex = compose(left_inner, left_cipher, CompressionKind::Gzip).unwrap();
    let (cmd, mut events, join) = Connection::spawn_boxed(left_box).expect("spawn");

    // Right side: a BoxedDuplex with the inverted stack. We push
    // a single Hotline-framed message in (22-byte header + body)
    // and expect the Connection to emit an Event::Frame.
    let mut right_box: BoxedDuplex =
        compose(right_inner, right_cipher, CompressionKind::Gzip).unwrap();

    // Hand-build a minimal Hotline header. Layout per
    // hotline_proto::parse::Header is:
    //   type(4) + trans(4) + flag(4) + len(4) + len2(4) + hc(2)
    // = 22 bytes. `len` counts body_bytes + sizeof(hc)=2 (the
    // protocol's quirky body-size encoding); we set it to
    // body.len() + 2 so decode_header_full reports the right
    // body_len after the hc subtraction.
    let body = b"transform-stack actor round trip".to_vec();
    let mut frame = vec![0u8; hotline_proto::HL_HDR_LEN + body.len()];
    let len_wire = (body.len() as u32) + 2;
    let len_off = 4 + 4 + 4; // after type, trans, flag
    frame[len_off..len_off + 4].copy_from_slice(&len_wire.to_be_bytes());
    frame[hotline_proto::HL_HDR_LEN..].copy_from_slice(&body);

    // Push it in via the right side; shut down so the writer
    // half closes its compressed frame deterministically.
    right_box.write_all(&frame).await.unwrap();
    right_box.shutdown().await.unwrap();

    // Receive the Frame event.
    let evt = tokio::time::timeout(std::time::Duration::from_secs(5), events.recv())
        .await
        .expect("event timely")
        .expect("event present");
    match evt {
        Event::Frame(f) => {
            assert_eq!(f.body, body, "frame body mismatch through transform stack");
        }
        Event::Shutdown(r) => panic!("expected Frame, got Shutdown: {r:?}"),
        Event::State(s) => panic!("expected Frame, got State: {s:?}"),
    }

    // Drop the command sender so the actor exits cleanly.
    drop(cmd);
    // Drain any trailing Shutdown event.
    let _ = tokio::time::timeout(std::time::Duration::from_secs(1), events.recv()).await;
    // Await the actor task itself with a timeout. Two things this
    // catches that the previous `let _join` didn't: the actor
    // loop actually exits (rather than getting silently aborted
    // when the tokio test runtime is dropped) and any panic
    // inside the loop surfaces here instead of being lost.
    let join_result = tokio::time::timeout(std::time::Duration::from_secs(2), join).await;
    let task_result = join_result.expect("Connection actor task did not exit within timeout");
    task_result.expect("Connection actor task panicked");
}
