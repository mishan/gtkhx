//! LOGIN reply receive for hxnet (Phase E of
//! `hxnet-owns-the-whole-lifecycle`).
//!
//! After the client sends `HTLC_HDR_LOGIN` (Phase D), the
//! server replies with an `HTLS_HDR_TASK` (opcode 0x010000)
//! frame whose body carries:
//!
//! - **Plaintext login** — no additional chunks. The TASK
//!   reply's `flag` byte signals success (0) or failure
//!   (non-zero, with an error string in a chunk).
//! - **HOPE login** — chunks carrying the server's MAC choice,
//!   cipher choice, compression choice, and a fresh session key
//!   the client uses to derive its HMAC challenge response.
//!
//! This module reads one 22-byte header + body off the
//! transport, parses the chunks via hotline-proto, and
//! produces a typed `LoginReply` for the Phase F HOPE state
//! machine to consume.
//!
//! # Failure modes
//!
//! - Stream IO error during read — surfaces as `io::Error`.
//! - Header signals task failure (`flag != 0`) — this is NOT an
//!   error here: `recv_login_reply` returns `Ok(LoginReply)` with
//!   `flag` set and `error_text` populated from the `DATA_ERROR_TEXT`
//!   chunk if present. The caller (e.g. the lifecycle) decides how to
//!   surface a rejection — `LoginReply::is_success()` is the gate.
//! - Header doesn't decode (oversized wire_len, etc.) —
//!   surfaces as `io::ErrorKind::InvalidData`.
//! - `wire_len` exceeds `MAX_BODY_LEN + 2` — surfaces the same way.

use std::io;

use tokio::io::AsyncReadExt;
use tokio::sync::mpsc;

use hotline_proto::parse::decode_header_full;
use hotline_proto::wire::ChunkIter;

use crate::{ConnectionState, Event, MAX_BODY_LEN};

/// `HTLS_HDR_TASK` opcode — the LOGIN reply wraps in this
/// response shape. Mirrors `HTLS_HDR_TASK` (0x00010000) in
/// `src/hotline.h`.
pub const HTLS_HDR_TASK: u32 = 0x0001_0000;

/// `HTLS_DATA_ERROR_TEXT` chunk tag — error message body when
/// a TASK reply signals failure. Mirrors
/// `HTLS_DATA_ERROR_TEXT` (0x0100) in `src/hotline.h`.
pub const TAG_ERROR_TEXT: u16 = 0x0100;

/// HOPE chunks the server might include. The C-side names
/// (with the `0x0e..` prefix) are from `src/hotline.h`.
pub const TAG_HOPE_APP_ID: u16 = 0x0e01;
pub const TAG_HOPE_APP_STRING: u16 = 0x0e02;
pub const TAG_SESSIONKEY: u16 = 0x0e03;
pub const TAG_MAC_ALG: u16 = 0x0e04;
pub const TAG_S_DATA_CIPHER_ALG: u16 = 0x0ec1;
pub const TAG_S_DATA_CIPHER_MODE: u16 = 0x0ec3;
/// Server-side COMPRESS_ALG tag. Mirrors `HTLS_DATA_COMPRESS_ALG`
/// (0x0ec9) in `src/hotline.h`. The step-2 LOGIN echoes the
/// server's negotiated compression choice back under this tag.
pub const TAG_S_DATA_COMPRESS_ALG: u16 = 0x0ec9;

/// Result of parsing a LOGIN reply. The HOPE-only fields are
/// `None` for a plaintext login reply.
#[derive(Debug, Clone, Default)]
pub struct LoginReply {
    /// The TASK reply's `flag` field. 0 = success, non-zero =
    /// failure (and `error_text` is populated).
    pub flag: u32,
    /// Server transaction id — the `trans` from the reply's
    /// header. C-side dispatcher uses this to correlate with
    /// the request the reply is for; pure-Rust callers
    /// typically ignore it.
    pub trans: u32,
    /// Populated when `flag != 0`. Server's error string.
    pub error_text: Option<Vec<u8>>,
    /// HOPE chunks. All `None` for a plaintext reply.
    pub sessionkey: Option<Vec<u8>>,
    pub mac_alg: Option<Vec<u8>>,
    pub cipher_alg: Option<Vec<u8>>,
    pub cipher_mode: Option<Vec<u8>>,
    pub hope_app_id: Option<Vec<u8>>,
    pub hope_app_string: Option<Vec<u8>>,
    /// The verbatim on-wire bytes of this reply: the 22-byte header
    /// followed by `body_len` chunk bytes. Retained so the Phase G
    /// orchestrator can replay the reply to the C side as a synthetic
    /// `Event::Frame` (`docs/phase-g-migration.md`, "Option B") after
    /// consuming it here to decide success/failure. Empty only for a
    /// default-constructed `LoginReply`; `recv_login_reply` always
    /// populates it.
    pub raw_frame: Vec<u8>,
}

impl LoginReply {
    /// `true` when the server replied with `flag == 0`.
    pub fn is_success(&self) -> bool {
        self.flag == 0
    }

    /// `true` when at least one HOPE chunk was present —
    /// indicates the server is participating in HOPE
    /// handshake step 1.
    pub fn has_hope_handshake(&self) -> bool {
        self.sessionkey.is_some()
            || self.mac_alg.is_some()
            || self.cipher_alg.is_some()
    }
}

/// Receive a single LOGIN reply off the transport. Emits
/// `Event::State(LoginReplyWait)` before the first read so the
/// C side can flip the throbber. Returns the parsed reply on
/// success.
pub async fn recv_login_reply<S>(
    stream: &mut S,
    evt_tx: &mpsc::Sender<Event>,
) -> io::Result<LoginReply>
where
    S: tokio::io::AsyncRead + Unpin,
{
    if evt_tx
        .send(Event::State(ConnectionState::LoginReplyWait))
        .await
        .is_err()
    {
        return Err(io::Error::other(
            "consumer dropped before LoginReplyWait state delivered",
        ));
    }

    // Read the 22-byte header.
    let mut hdr_buf = [0u8; hotline_proto::HL_HDR_LEN];
    stream.read_exact(&mut hdr_buf).await?;

    // Decode with NO clamp (u32::MAX) so wire_len is the raw value,
    // then refuse oversized frames by the raw wire_len. Passing
    // MAX_BODY_LEN+2 would let decode_header_full clamp body_len and
    // we'd read a truncated frame, leaving the remainder on the stream
    // and desyncing the next header read. Same ceiling check as
    // connection.rs::read_one_frame (wire_len includes the 2-byte hc).
    let decoded = decode_header_full(&hdr_buf, u32::MAX).ok_or_else(|| {
        io::Error::new(
            io::ErrorKind::InvalidData,
            "LOGIN reply header didn't decode",
        )
    })?;

    if decoded.type_ != HTLS_HDR_TASK {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!(
                "LOGIN reply: expected HTLS_HDR_TASK (0x{:x}), got 0x{:x}",
                HTLS_HDR_TASK, decoded.type_
            ),
        ));
    }

    if decoded.wire_len > MAX_BODY_LEN + 2 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!(
                "LOGIN reply wire_len {} exceeds limit {}",
                decoded.wire_len,
                MAX_BODY_LEN + 2
            ),
        ));
    }

    // Read the body. body_len is the *wire* body length (after
    // header), which already excludes the hc-counted 2 bytes
    // that the chunk parser handles internally.
    let body_len = decoded.body_len as usize;
    let mut body_buf = vec![0u8; hotline_proto::HL_HDR_LEN + body_len];
    body_buf[..hotline_proto::HL_HDR_LEN].copy_from_slice(&hdr_buf);
    if body_len > 0 {
        stream
            .read_exact(&mut body_buf[hotline_proto::HL_HDR_LEN..])
            .await?;
    }

    let mut reply = LoginReply {
        flag: decoded.flag,
        trans: decoded.trans,
        ..Default::default()
    };

    // Walk the chunks. Empty body = no chunks; the loop is a no-op.
    for chunk in ChunkIter::over_message(&body_buf, body_buf.len()) {
        match chunk.tag {
            TAG_ERROR_TEXT => reply.error_text = Some(chunk.data.to_vec()),
            TAG_SESSIONKEY => reply.sessionkey = Some(chunk.data.to_vec()),
            TAG_MAC_ALG => reply.mac_alg = Some(chunk.data.to_vec()),
            TAG_S_DATA_CIPHER_ALG => reply.cipher_alg = Some(chunk.data.to_vec()),
            TAG_S_DATA_CIPHER_MODE => reply.cipher_mode = Some(chunk.data.to_vec()),
            TAG_HOPE_APP_ID => reply.hope_app_id = Some(chunk.data.to_vec()),
            TAG_HOPE_APP_STRING => reply.hope_app_string = Some(chunk.data.to_vec()),
            // TAG_S_DATA_COMPRESS_ALG is deliberately NOT captured: the
            // orchestrator advertises an empty compress_algs list in HOPE
            // step 1 (see run_hope_lifecycle), so a conformant server
            // never sends a compression choice here. If/when compression
            // is wired end-to-end (advertise + apply in compose()), add a
            // `compress_alg` field and capture it — see the note in
            // hope::select_algorithms.
            // Other chunks are ignored at this layer. The HOPE
            // state machine in Phase F consumes the typed
            // fields it cares about.
            _ => {}
        }
    }

    // Retain the full wire frame (header + body) for the Phase G
    // replay path. The chunk walk above borrowed body_buf
    // immutably; that borrow has ended, so we can move it in here.
    reply.raw_frame = body_buf;

    Ok(reply)
}

#[cfg(test)]
mod tests {
    use super::*;
    use hotline_proto::build::{pack_message, pack_message_size, PackChunk};
    use tokio::io::{duplex, AsyncWriteExt};

    /// Build a TASK reply with the given chunks for the test.
    fn build_task_reply(flag: u32, chunks: &[PackChunk<'_>]) -> Vec<u8> {
        let needed = pack_message_size(chunks);
        let mut out = vec![0u8; needed];
        pack_message(&mut out, HTLS_HDR_TASK, /*trans=*/ 1, flag, chunks).expect("pack");
        out
    }

    /// Plaintext success: server sends an empty TASK reply.
    #[tokio::test]
    async fn recv_login_reply_plaintext_success() {
        let (mut client, mut server) = duplex(256);
        let (evt_tx, mut evt_rx) = mpsc::channel(8);

        let reply_bytes = build_task_reply(0, &[]);

        tokio::spawn(async move {
            server.write_all(&reply_bytes).await.expect("write");
        });

        let reply = recv_login_reply(&mut client, &evt_tx).await.expect("recv");
        assert!(reply.is_success());
        assert!(!reply.has_hope_handshake());
        assert!(reply.error_text.is_none());

        // Phase G: the raw wire frame is retained for replay and
        // round-trips through Frame::from_raw to the same opcode /
        // flag the C side will dispatch on.
        assert!(!reply.raw_frame.is_empty(), "raw_frame must be populated");
        let frame = crate::Frame::from_raw(&reply.raw_frame)
            .expect("raw_frame should decode");
        assert_eq!(frame.header.type_, HTLS_HDR_TASK);
        assert_eq!(frame.header.flag, 0);

        let evt = evt_rx.recv().await.expect("state event");
        assert!(matches!(evt, Event::State(ConnectionState::LoginReplyWait)));
    }

    /// Plaintext failure: TASK reply with flag != 0 and an
    /// ERROR_TEXT chunk.
    #[tokio::test]
    async fn recv_login_reply_plaintext_failure_extracts_error_text() {
        let (mut client, mut server) = duplex(256);
        let (evt_tx, _evt_rx) = mpsc::channel(8);

        let reply_bytes = build_task_reply(
            1,
            &[PackChunk {
                tag: TAG_ERROR_TEXT,
                data: b"login incorrect",
            }],
        );

        tokio::spawn(async move {
            server.write_all(&reply_bytes).await.expect("write");
        });

        let reply = recv_login_reply(&mut client, &evt_tx).await.expect("recv");
        assert!(!reply.is_success());
        assert_eq!(reply.flag, 1);
        assert_eq!(reply.error_text.as_deref(), Some(b"login incorrect" as &[u8]));
    }

    /// HOPE step-1 reply: extracts the cipher / MAC choice
    /// chunks. Pinned against a hand-built fixture.
    #[tokio::test]
    async fn recv_login_reply_hope_step1_extracts_chunks() {
        let (mut client, mut server) = duplex(512);
        let (evt_tx, _evt_rx) = mpsc::channel(8);

        let reply_bytes = build_task_reply(
            0,
            &[
                PackChunk {
                    tag: TAG_SESSIONKEY,
                    data: b"\x12\x34\x56\x78" as &[u8],
                },
                PackChunk {
                    tag: TAG_MAC_ALG,
                    data: b"\x00\x01\x0bHMAC-SHA256" as &[u8],
                },
                PackChunk {
                    tag: TAG_S_DATA_CIPHER_ALG,
                    data: b"\x00\x01\x08BLOWFISH" as &[u8],
                },
                PackChunk {
                    tag: TAG_S_DATA_CIPHER_MODE,
                    data: b"STREAM" as &[u8],
                },
            ],
        );

        tokio::spawn(async move {
            server.write_all(&reply_bytes).await.expect("write");
        });

        let reply = recv_login_reply(&mut client, &evt_tx).await.expect("recv");
        assert!(reply.is_success());
        assert!(reply.has_hope_handshake());
        assert_eq!(reply.sessionkey.as_deref(), Some(b"\x12\x34\x56\x78" as &[u8]));
        assert_eq!(
            reply.mac_alg.as_deref(),
            Some(b"\x00\x01\x0bHMAC-SHA256" as &[u8])
        );
        assert_eq!(
            reply.cipher_alg.as_deref(),
            Some(b"\x00\x01\x08BLOWFISH" as &[u8])
        );
        assert_eq!(reply.cipher_mode.as_deref(), Some(b"STREAM" as &[u8]));
    }

    /// Wrong opcode (server sent something other than TASK)
    /// surfaces as InvalidData.
    #[tokio::test]
    async fn recv_login_reply_wrong_opcode_errors() {
        let (mut client, mut server) = duplex(256);
        let (evt_tx, _evt_rx) = mpsc::channel(8);

        // Build a frame with HTLC_HDR_LOGIN opcode (0x6b) where
        // we expected HTLS_HDR_TASK (0x010000).
        let needed = pack_message_size(&[]);
        let mut wrong = vec![0u8; needed];
        pack_message(&mut wrong, 0x6b, 1, 0, &[]).expect("pack");

        tokio::spawn(async move {
            server.write_all(&wrong).await.expect("write");
        });

        let err = recv_login_reply(&mut client, &evt_tx).await.unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::InvalidData);
        let msg = err.to_string();
        assert!(msg.contains("expected HTLS_HDR_TASK"), "got: {msg}");
    }
}
