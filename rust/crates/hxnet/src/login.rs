//! LOGIN send for hxnet (Phase D of
//! `hxnet-owns-the-whole-lifecycle`).
//!
//! After the magic exchange completes (Phase C), the client
//! sends an `HTLC_HDR_LOGIN` (opcode 0x6b) frame. The body is
//! a chunked list with one chunk per credential / identity
//! field. This module assembles + sends that frame.
//!
//! # Wire shape (plaintext path)
//!
//! Required chunks:
//!
//! - `DATA_LOGIN` (tag 0x0069) — XOR-0xFF obfuscated login bytes
//! - `DATA_PASSWORD` (tag 0x006a) — XOR-0xFF obfuscated password
//!
//! Optional (production sends them; the server defaults them
//! gracefully if absent):
//!
//! - `DATA_NAME` (tag 0x0066) — display name (UTF-8, unobfuscated)
//! - `DATA_ICON` (tag 0x0068) — 2-byte BE icon ID
//! - `DATA_VERSION` (tag 0x00a0) — 2-byte BE client version
//!
//! # XOR-0xFF obfuscation
//!
//! Login + password bytes are obfuscated on the wire by XORing
//! each byte with 0xFF. This is from the 1990s Mac client and
//! has no security value (every server in the wild knows about
//! it), but it's part of the wire spec so we have to honour it.
//!
//! Empty password is a real case ("guest login") — encode the
//! empty buffer as a zero-length chunk (NOT a single 0xFF byte,
//! which would decode as a single-byte password of 0x00). The
//! server distinguishes these.
//!
//! # HOPE chunks
//!
//! Phase F adds the HOPE-handshake chunks
//! (`DATA_SESSIONKEY` / `DATA_MAC_ALG` / `DATA_HOPE_APP_ID` /
//! `DATA_HOPE_APP_STRING` / `DATA_CIPHER_ALG` /
//! `DATA_COMPRESS_ALG`). For the plaintext path this module
//! ships in Phase D, the login + password chunks are sufficient.

use std::io;

use tokio::io::AsyncWriteExt;
use tokio::sync::mpsc;

use hotline_proto::build::{pack_message, pack_message_size, PackChunk};
use hotline_proto::messages::tag;

use crate::{ConnectionState, Event};

/// `HTLC_HDR_LOGIN` opcode. Mirrors the C-side
/// `#define HTLC_HDR_LOGIN ((guint32)0x0000006b)` in
/// `src/hotline.h`. Pinned here as a `u32` to match the wire
/// header's 4-byte BE field.
pub const HTLC_HDR_LOGIN: u32 = 0x0000_006b;

/// `HTLC_DATA_CLIENTVERSION` chunk tag — 2-byte BE client
/// version. Mirrors `HTLC_DATA_CLIENTVERSION` (0x00a0) in
/// `src/hotline.h`. Servers (mhxd) read this to set the
/// `can_ping` access bit (>= 150 → PING keepalive accepted).
pub const TAG_VERSION: u16 = 0x00a0;

/// `HTLC_DATA_CAPABILITIES` chunk tag — 2-byte BE capability
/// bitmask. Mirrors `HTLC_DATA_CAPABILITIES` (0x01f0) in
/// `src/hotline.h`. Capability-aware servers (Janus) echo the
/// agreed bits back in the LOGIN reply; cap-unaware servers
/// (mhxd) ignore the chunk per spec. Omitting it entirely (as an
/// earlier draft of this module did) means extensions like
/// chat-history never negotiate.
pub const TAG_CAPABILITIES: u16 = 0x01f0;

/// XOR-0xFF obfuscate a credential buffer onto an output buffer.
/// `out` must be at least `src.len()` bytes; the caller pre-
/// sizes from `src.len()`. No allocation.
fn xor_credential(src: &[u8], out: &mut [u8]) {
    debug_assert!(out.len() >= src.len());
    for (i, b) in src.iter().enumerate() {
        out[i] = !b;
    }
}

/// Login parameters carried across the FFI for the LOGIN send.
/// Lifetimes and ownership are deliberately simple — every field
/// is a slice the caller owns; the module copies what it needs
/// into the wire buffer.
#[derive(Debug, Clone)]
pub struct LoginRequest<'a> {
    pub login: &'a [u8],
    pub password: &'a [u8],
    /// Display name. Empty `&[]` to omit the chunk.
    pub name: &'a [u8],
    /// Icon ID (BE u16 on the wire). 0 to omit.
    pub icon: u16,
    /// Client version (BE u16 on the wire). 0 to omit.
    pub version: u16,
    /// Capability bitmask (BE u16 on the wire, `HTLC_CAP_*`). 0
    /// omits the chunk — but production should advertise the same
    /// bits the legacy LOGIN does so extensions (chat-history,
    /// inline-media, voice) negotiate. The C side owns the policy
    /// and passes it through the FFI.
    pub caps: u16,
    /// Transaction id for the frame header. Caller picks; the
    /// C side typically uses a counter starting at 1.
    pub trans: u32,
}

/// Build the LOGIN frame into a freshly-allocated `Vec<u8>`.
/// Returns the wire bytes; the caller writes them through their
/// transport.
///
/// Returns `Err(io::Error{InvalidInput})` if `login.len()` or
/// `password.len()` exceeds the per-chunk u16 cap (extremely
/// unlikely in practice — even pathological credentials are
/// shorter than 64 KiB).
pub fn build_login_frame(req: &LoginRequest<'_>) -> io::Result<Vec<u8>> {
    if req.login.len() > u16::MAX as usize || req.password.len() > u16::MAX as usize {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "credential exceeds u16 chunk-length cap",
        ));
    }

    // XOR-0xFF the credentials into stack-allocated buffers.
    // Production credentials are bounded by the Hotline 1.x
    // wire (login: ≤ 32 chars, password: ≤ 32 chars by spec),
    // but accept arbitrary u16-sized buffers to keep the API
    // honest.
    let mut login_x = vec![0u8; req.login.len()];
    let mut pass_x = vec![0u8; req.password.len()];
    xor_credential(req.login, &mut login_x);
    xor_credential(req.password, &mut pass_x);

    // Icon + version chunks are u16 BE if present.
    let icon_be = req.icon.to_be_bytes();
    let version_be = req.version.to_be_bytes();

    let caps_be = req.caps.to_be_bytes();

    let mut chunks: Vec<PackChunk<'_>> = Vec::with_capacity(6);
    chunks.push(PackChunk { tag: tag::LOGIN, data: &login_x });
    chunks.push(PackChunk { tag: tag::PASSWORD, data: &pass_x });
    if !req.name.is_empty() {
        chunks.push(PackChunk { tag: tag::NAME, data: req.name });
    }
    if req.icon != 0 {
        chunks.push(PackChunk { tag: tag::ICON, data: &icon_be });
    }
    if req.version != 0 {
        chunks.push(PackChunk { tag: TAG_VERSION, data: &version_be });
    }
    if req.caps != 0 {
        chunks.push(PackChunk { tag: TAG_CAPABILITIES, data: &caps_be });
    }

    let needed = pack_message_size(&chunks);
    let mut out = vec![0u8; needed];
    let written = pack_message(
        &mut out,
        HTLC_HDR_LOGIN,
        req.trans,
        /*flag=*/ 0,
        &chunks,
    )
    .ok_or_else(|| {
        io::Error::new(
            io::ErrorKind::InvalidInput,
            "pack_message rejected the LOGIN chunks",
        )
    })?;
    debug_assert_eq!(written, needed);
    Ok(out)
}

/// Send a LOGIN frame on the provided transport. Emits
/// `Event::State(LoginSending)` before the write. Returns
/// `Ok(())` once the bytes are flushed.
pub async fn send_login<S>(
    stream: &mut S,
    req: &LoginRequest<'_>,
    evt_tx: &mpsc::Sender<Event>,
) -> io::Result<()>
where
    S: tokio::io::AsyncWrite + Unpin,
{
    let frame = build_login_frame(req)?;
    if evt_tx
        .send(Event::State(ConnectionState::LoginSending))
        .await
        .is_err()
    {
        return Err(io::Error::other(
            "consumer dropped before LoginSending state delivered",
        ));
    }
    stream.write_all(&frame).await?;
    stream.flush().await?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use tokio::io::{duplex, AsyncReadExt};

    /// Hand-craft the expected wire bytes for a known fixture
    /// and verify build_login_frame produces them byte-exact.
    /// Pin the XOR-0xFF + chunk-layout + header-encoding all in
    /// one place; future refactors can't drift without tripping
    /// this assertion.
    #[test]
    fn build_login_frame_wire_layout() {
        let req = LoginRequest {
            login: b"misha",
            password: b"pw",
            name: b"",
            icon: 0,
            version: 0,
            caps: 0,
            trans: 1,
        };
        let frame = build_login_frame(&req).expect("build");

        // 22-byte header + 2 chunks. Chunks: LOGIN (5 bytes XOR
        // + 4-byte chunk header) + PASSWORD (2 bytes XOR + 4-byte
        // chunk header) = 15 byte body.
        // wire_len encodes body+2 = 17.
        let expected_total = 22 + 4 + 5 + 4 + 2;
        assert_eq!(frame.len(), expected_total);

        // Header field-by-field.
        assert_eq!(&frame[0..4], &HTLC_HDR_LOGIN.to_be_bytes());
        assert_eq!(&frame[4..8], &1u32.to_be_bytes()); // trans
        assert_eq!(&frame[8..12], &0u32.to_be_bytes()); // flag
        let wire_len = (expected_total as u32) - 20; // body + 2
        assert_eq!(&frame[12..16], &wire_len.to_be_bytes());
        assert_eq!(&frame[16..20], &wire_len.to_be_bytes()); // len2
        assert_eq!(&frame[20..22], &2u16.to_be_bytes()); // hc

        // Chunk 1: tag=0x0069 len=5 data=XOR("misha")
        assert_eq!(&frame[22..24], &tag::LOGIN.to_be_bytes());
        assert_eq!(&frame[24..26], &5u16.to_be_bytes());
        let xor_misha: Vec<u8> = b"misha".iter().map(|b| !b).collect();
        assert_eq!(&frame[26..31], &xor_misha[..]);

        // Chunk 2: tag=0x006a len=2 data=XOR("pw")
        assert_eq!(&frame[31..33], &tag::PASSWORD.to_be_bytes());
        assert_eq!(&frame[33..35], &2u16.to_be_bytes());
        let xor_pw: Vec<u8> = b"pw".iter().map(|b| !b).collect();
        assert_eq!(&frame[35..37], &xor_pw[..]);
    }

    /// Empty password (guest login) — encode as zero-length
    /// chunk, NOT a single 0xFF byte.
    #[test]
    fn build_login_frame_empty_password() {
        let req = LoginRequest {
            login: b"guest",
            password: b"",
            name: b"",
            icon: 0,
            version: 0,
            caps: 0,
            trans: 2,
        };
        let frame = build_login_frame(&req).expect("build");

        // PASSWORD chunk: tag at offset 31 (22+4+5 = 31), len=0,
        // no data bytes.
        let pass_tag_offset = 22 + 4 + 5;
        assert_eq!(
            &frame[pass_tag_offset..pass_tag_offset + 2],
            &tag::PASSWORD.to_be_bytes()
        );
        assert_eq!(
            &frame[pass_tag_offset + 2..pass_tag_offset + 4],
            &0u16.to_be_bytes(),
            "empty password should encode as zero-length chunk"
        );
        // Frame ends right after the chunk header (no data
        // bytes for the password).
        assert_eq!(frame.len(), pass_tag_offset + 4);
    }

    /// Optional chunks (name, icon, version) all appear when
    /// non-empty / non-zero.
    #[test]
    fn build_login_frame_with_optional_chunks() {
        let req = LoginRequest {
            login: b"x",
            password: b"y",
            name: b"Misha",
            icon: 0x7ffd,
            version: 0x00b9,
            caps: 0,
            trans: 3,
        };
        let frame = build_login_frame(&req).expect("build");

        // 5 chunks total. hc at offset 20.
        assert_eq!(&frame[20..22], &5u16.to_be_bytes());

        // Find each tag in the frame. Order-independent —
        // production servers parse by tag.
        let tags = [tag::LOGIN, tag::PASSWORD, tag::NAME, tag::ICON, TAG_VERSION];
        let mut pos = 22;
        for expected_tag in tags {
            let got_tag = u16::from_be_bytes([frame[pos], frame[pos + 1]]);
            assert_eq!(got_tag, expected_tag);
            let chunk_len = u16::from_be_bytes([frame[pos + 2], frame[pos + 3]]) as usize;
            pos += 4 + chunk_len;
        }
        assert_eq!(pos, frame.len());
    }

    /// Capabilities regression guard. The orchestrator LOGIN must
    /// carry an HTLC_DATA_CAPABILITIES chunk (tag 0x01f0) whenever
    /// caps != 0, with the bitmask big-endian — otherwise
    /// extensions (chat-history / inline-media / voice) never
    /// negotiate, which is exactly the bug that shipped when this
    /// chunk was missing. Mirrors the legacy login_packet.c
    /// LEGACY-mode advertisement (0x001F).
    #[test]
    fn build_login_frame_advertises_capabilities() {
        const CAPS: u16 = 0x001F; // large-files|text-encoding|voice|inline|chat-history
        let req = LoginRequest {
            login: b"guest",
            password: b"",
            name: b"",
            icon: 0,
            version: 185,
            caps: CAPS,
            trans: 1,
        };
        let frame = build_login_frame(&req).expect("build");

        // Walk the chunks and find the CAPABILITIES one.
        let hc = u16::from_be_bytes([frame[20], frame[21]]);
        let mut pos = 22usize;
        let mut found = None;
        for _ in 0..hc {
            let tag = u16::from_be_bytes([frame[pos], frame[pos + 1]]);
            let len = u16::from_be_bytes([frame[pos + 2], frame[pos + 3]]) as usize;
            let data = &frame[pos + 4..pos + 4 + len];
            if tag == TAG_CAPABILITIES {
                found = Some(data.to_vec());
            }
            pos += 4 + len;
        }
        let caps_data = found.expect("LOGIN must include an HTLC_DATA_CAPABILITIES chunk");
        assert_eq!(caps_data, CAPS.to_be_bytes(), "caps bitmask must round-trip big-endian");

        // caps == 0 omits the chunk (matches legacy send_caps gate).
        let req0 = LoginRequest { caps: 0, ..req };
        let frame0 = build_login_frame(&req0).expect("build");
        let hc0 = u16::from_be_bytes([frame0[20], frame0[21]]);
        let mut pos0 = 22usize;
        for _ in 0..hc0 {
            let tag = u16::from_be_bytes([frame0[pos0], frame0[pos0 + 1]]);
            let len = u16::from_be_bytes([frame0[pos0 + 2], frame0[pos0 + 3]]) as usize;
            assert_ne!(tag, TAG_CAPABILITIES, "caps=0 should omit the chunk");
            pos0 += 4 + len;
        }
    }

    /// send_login emits the LoginSending state event and
    /// writes the right bytes.
    #[tokio::test]
    async fn send_login_emits_state_and_writes_bytes() {
        let (mut client, mut server) = duplex(256);
        let (evt_tx, mut evt_rx) = mpsc::channel(8);

        let req = LoginRequest {
            login: b"guest",
            password: b"",
            name: b"",
            icon: 0,
            version: 0,
            caps: 0,
            trans: 7,
        };
        let expected = build_login_frame(&req).expect("build");
        let expected_len = expected.len();

        let server_task = tokio::spawn(async move {
            let mut buf = vec![0u8; expected_len];
            server.read_exact(&mut buf).await.expect("server read");
            buf
        });

        send_login(&mut client, &req, &evt_tx).await.expect("send");
        let got_bytes = server_task.await.unwrap();

        assert_eq!(got_bytes, expected);

        let evt = evt_rx.recv().await.expect("state event");
        assert!(
            matches!(evt, Event::State(ConnectionState::LoginSending)),
            "expected LoginSending state event, got {evt:?}"
        );
    }
}
