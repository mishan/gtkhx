//! HOPE handshake state machine (Phase F of
//! `hxnet-owns-the-whole-lifecycle`).
//!
//! HOPE-Secure-Login is the two-step login flow that derives a
//! session-keyed HMAC over the password and negotiates a cipher
//! + compression layer for the post-login stream. The wire
//! shape:
//!
//! 1. **Step 1 — client sends LOGIN** with empty `DATA_LOGIN`
//!    + empty `DATA_PASSWORD` + `DATA_MAC_ALG` list (client's
//!    supported MACs) + `DATA_HOPE_APP_ID` ("GTKx") +
//!    `DATA_HOPE_APP_STRING` + `HTLC_DATA_CIPHER_ALG` list +
//!    empty `DATA_SESSIONKEY`.
//!
//! 2. **Step 1 reply — server sends TASK** with chosen
//!    `DATA_SESSIONKEY` (server-generated 64-byte nonce) +
//!    chosen `DATA_MAC_ALG` (single entry) + chosen
//!    `S_DATA_CIPHER_ALG` + `S_DATA_CIPHER_MODE` (STREAM or
//!    AEAD). Parsed in Phase E's `LoginReply`.
//!
//! 3. **Step 2 — client sends LOGIN** with real `DATA_LOGIN`
//!    (XOR-0xFF as plaintext) + `DATA_PASSWORD` (HMAC over the
//!    plaintext password using the server's sessionkey + the
//!    chosen MAC) + echoes of the chosen
//!    `S_DATA_CIPHER_ALG`/`S_DATA_COMPRESS_ALG`.
//!
//! 4. **Step 2 reply — server sends TASK** with `flag=0` for
//!    success. After this, both sides derive the cipher keys
//!    from the sessionkey + chosen algorithms, and the
//!    transport wraps in the corresponding adapter.
//!
//! # Phase F scope
//!
//! This module ships steps 1-3 (the wire shape + HMAC) and the
//! state-machine orchestrator. Step 4's cipher transition
//! (deriving the keys + swapping the transport) is the
//! follow-up Phase F-2 / Phase G work — it requires hooking
//! into the actor's transport-rewrap path, which is its own
//! design problem.

use std::io;

use hotline_proto::build::{pack_message, pack_message_size, PackChunk};
use hotline_proto::messages::tag;

use crate::login::HTLC_HDR_LOGIN;
use crate::login_reply::{
    TAG_HOPE_APP_ID, TAG_HOPE_APP_STRING, TAG_MAC_ALG, TAG_S_DATA_CIPHER_ALG,
    TAG_S_DATA_CIPHER_MODE, TAG_SESSIONKEY,
};

/// Tag for the client's CIPHER_ALG list in the step 1 LOGIN.
/// Mirrors `HTLC_DATA_CIPHER_ALG` (0x0ec2) in `src/hotline.h`.
pub const TAG_C_DATA_CIPHER_ALG: u16 = 0x0ec2;

/// Tag for the client's COMPRESS_ALG list. Mirrors
/// `HTLC_DATA_COMPRESS_ALG` (0x0eca) in `src/hotline.h`.
pub const TAG_C_DATA_COMPRESS_ALG: u16 = 0x0eca;

/// The four-byte ASCII tag identifying us to the server. C side
/// uses "GTKx" in `src/network.c`. Pinned here for wire-format
/// parity.
pub const HOPE_APP_ID: &[u8; 4] = b"GTKx";

/// Encode an algorithm list ("MAC list" / "cipher list") in
/// the HOPE wire shape:
///
/// ```text
///   2 bytes  BE u16 count
///   per entry:
///     1 byte    label length
///     N bytes   label (ASCII)
/// ```
///
/// Returns the encoded bytes. Caller drops the buffer into a
/// `PackChunk` for the matching tag.
pub fn encode_alg_list(names: &[&[u8]]) -> Vec<u8> {
    let count = names.len() as u16;
    let mut total = 2;
    for n in names {
        total += 1 + n.len();
    }
    let mut out = Vec::with_capacity(total);
    out.extend_from_slice(&count.to_be_bytes());
    for n in names {
        let len_byte = n.len().min(255) as u8;
        out.push(len_byte);
        out.extend_from_slice(&n[..len_byte as usize]);
    }
    out
}

/// Parse the BE u16 count + length-prefixed names back out of
/// an encoded algorithm list. Used to inspect what the server
/// chose in its step-1 reply.
pub fn parse_alg_list(buf: &[u8]) -> Option<Vec<Vec<u8>>> {
    if buf.len() < 2 {
        return None;
    }
    let count = u16::from_be_bytes([buf[0], buf[1]]) as usize;
    let mut names = Vec::with_capacity(count);
    let mut pos = 2;
    for _ in 0..count {
        if pos >= buf.len() {
            return None;
        }
        let label_len = buf[pos] as usize;
        pos += 1;
        if pos + label_len > buf.len() {
            return None;
        }
        names.push(buf[pos..pos + label_len].to_vec());
        pos += label_len;
    }
    Some(names)
}

/// Parameters for the HOPE step 1 LOGIN. Empty login + password
/// are mandatory — the server-chosen MAC is what authenticates
/// step 2. The cipher / mac / compress lists are the client's
/// supported algorithm preferences, in preference order.
#[derive(Debug, Clone)]
pub struct HopeStep1Request<'a> {
    pub trans: u32,
    pub mac_algs: &'a [&'a [u8]],
    pub cipher_algs: &'a [&'a [u8]],
    pub compress_algs: &'a [&'a [u8]],
    /// Optional 4-byte app id. Defaults to [`HOPE_APP_ID`]
    /// when None.
    pub app_id: Option<&'a [u8]>,
    /// Optional app description string. Empty `Some(&[])`
    /// sends an empty chunk; `None` omits the chunk entirely.
    pub app_string: Option<&'a [u8]>,
}

/// Build the HOPE step 1 LOGIN frame. Returns the wire bytes
/// ready for the transport write.
pub fn build_step1_login(req: &HopeStep1Request<'_>) -> io::Result<Vec<u8>> {
    let mac_list = encode_alg_list(req.mac_algs);
    let cipher_list = encode_alg_list(req.cipher_algs);
    let compress_list = encode_alg_list(req.compress_algs);
    let app_id = req.app_id.unwrap_or(HOPE_APP_ID);

    let mut chunks: Vec<PackChunk<'_>> = Vec::with_capacity(8);
    // Empty login + password chunks.
    chunks.push(PackChunk { tag: tag::LOGIN, data: &[] });
    chunks.push(PackChunk { tag: tag::PASSWORD, data: &[] });
    chunks.push(PackChunk { tag: TAG_MAC_ALG, data: &mac_list });
    chunks.push(PackChunk { tag: TAG_HOPE_APP_ID, data: app_id });
    if let Some(s) = req.app_string {
        chunks.push(PackChunk { tag: TAG_HOPE_APP_STRING, data: s });
    }
    chunks.push(PackChunk { tag: TAG_C_DATA_CIPHER_ALG, data: &cipher_list });
    if !req.compress_algs.is_empty() {
        chunks.push(PackChunk { tag: TAG_C_DATA_COMPRESS_ALG, data: &compress_list });
    }
    // Empty sessionkey — server fills it in.
    chunks.push(PackChunk { tag: TAG_SESSIONKEY, data: &[] });

    let needed = pack_message_size(&chunks);
    let mut out = vec![0u8; needed];
    pack_message(&mut out, HTLC_HDR_LOGIN, req.trans, 0, &chunks)
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "pack rejected"))?;
    Ok(out)
}

/// Algorithm choice the client made based on the server's
/// step-1 reply. Computed by [`select_algorithms`] from the
/// `LoginReply` Phase E parsed.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct HopeAlgorithmChoice {
    pub mac_alg: Vec<u8>,
    pub cipher_alg: Vec<u8>,
    pub cipher_mode: Vec<u8>,
    pub compress_alg: Option<Vec<u8>>,
    /// The 64-byte sessionkey the server generated.
    pub sessionkey: Vec<u8>,
}

/// Extract the server's algorithm choices from a step-1 reply.
/// Returns `None` if any required field is missing
/// (sessionkey, mac_alg, cipher_alg).
pub fn select_algorithms(reply: &crate::login_reply::LoginReply) -> Option<HopeAlgorithmChoice> {
    let sessionkey = reply.sessionkey.clone()?;
    let mac_list = reply.mac_alg.as_ref().and_then(|b| parse_alg_list(b))?;
    let cipher_list = reply.cipher_alg.as_ref().and_then(|b| parse_alg_list(b))?;
    let mac_alg = mac_list.into_iter().next()?;
    let cipher_alg = cipher_list.into_iter().next()?;
    let cipher_mode = reply.cipher_mode.clone().unwrap_or_else(|| b"STREAM".to_vec());
    Some(HopeAlgorithmChoice {
        mac_alg,
        cipher_alg,
        cipher_mode,
        compress_alg: None,
        sessionkey,
    })
}

/// Compute the HMAC over the password using the sessionkey +
/// chosen MAC algorithm. The output goes in the step-2 LOGIN
/// `DATA_PASSWORD` chunk.
///
/// Returns the HMAC output bytes (length matches the MAC's
/// digest size — 32 for HMAC-SHA256, 20 for HMAC-SHA1, 16 for
/// HMAC-MD5).
pub fn hmac_password(
    password: &[u8],
    sessionkey: &[u8],
    mac_alg_label: &[u8],
) -> io::Result<Vec<u8>> {
    let alg_str = std::str::from_utf8(mac_alg_label).map_err(|e| {
        io::Error::new(io::ErrorKind::InvalidInput, format!("MAC alg label not UTF-8: {e}"))
    })?;
    // hxcrypto-hash's hmac_xxx writes into a fixed 32-byte
    // buffer and returns the digest length.
    let mut md = [0u8; 32];
    let len = hxcrypto_hash::hmac_xxx(&mut md, password, sessionkey, alg_str);
    if len == 0 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            format!("hmac_xxx returned 0 for alg '{alg_str}' — unsupported algorithm?"),
        ));
    }
    Ok(md[..len as usize].to_vec())
}

/// Parameters for the HOPE step 2 LOGIN.
#[derive(Debug, Clone)]
pub struct HopeStep2Request<'a> {
    pub trans: u32,
    /// Real login bytes (XOR-0xFF on the wire — same as
    /// plaintext login).
    pub login: &'a [u8],
    /// HMAC output from `hmac_password` — already digested.
    pub password_mac: &'a [u8],
    /// Algorithm choice from the step 1 reply.
    pub choice: &'a HopeAlgorithmChoice,
    /// Display name (empty `&[]` to omit).
    pub name: &'a [u8],
    /// Icon (0 to omit).
    pub icon: u16,
    /// Client version (0 to omit).
    pub version: u16,
}

/// Build the HOPE step 2 LOGIN frame.
pub fn build_step2_login(req: &HopeStep2Request<'_>) -> io::Result<Vec<u8>> {
    // XOR-0xFF the login (same as plaintext).
    let login_x: Vec<u8> = req.login.iter().map(|b| !b).collect();

    // Re-encode the chosen cipher/compress as a single-entry
    // list, which is what step 2 echoes back to the server.
    let cipher_list_back = encode_alg_list(&[&req.choice.cipher_alg]);
    let compress_list_back = req
        .choice
        .compress_alg
        .as_ref()
        .map(|c| encode_alg_list(&[c]));

    let icon_be = req.icon.to_be_bytes();
    let version_be = req.version.to_be_bytes();

    let mut chunks: Vec<PackChunk<'_>> = Vec::with_capacity(8);
    chunks.push(PackChunk { tag: tag::LOGIN, data: &login_x });
    chunks.push(PackChunk { tag: tag::PASSWORD, data: req.password_mac });
    chunks.push(PackChunk { tag: TAG_S_DATA_CIPHER_ALG, data: &cipher_list_back });
    if let Some(c) = &compress_list_back {
        chunks.push(PackChunk { tag: 0x0ec9, data: c });
    }
    if !req.name.is_empty() {
        chunks.push(PackChunk { tag: tag::NAME, data: req.name });
    }
    if req.icon != 0 {
        chunks.push(PackChunk { tag: tag::ICON, data: &icon_be });
    }
    if req.version != 0 {
        chunks.push(PackChunk { tag: crate::login::TAG_VERSION, data: &version_be });
    }
    // Echo cipher_mode if non-default — preserves the STREAM/
    // AEAD distinction the server expects.
    if req.choice.cipher_mode != b"STREAM" {
        chunks.push(PackChunk {
            tag: TAG_S_DATA_CIPHER_MODE,
            data: &req.choice.cipher_mode,
        });
    }

    let needed = pack_message_size(&chunks);
    let mut out = vec![0u8; needed];
    pack_message(&mut out, HTLC_HDR_LOGIN, req.trans, 0, &chunks)
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "pack rejected"))?;
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::login_reply::LoginReply;

    #[test]
    fn encode_alg_list_three_macs() {
        let list = encode_alg_list(&[b"HMAC-SHA256", b"HMAC-SHA1", b"HMAC-MD5"]);
        // Byte-exact against the trace from production:
        // 00 03 0b HMAC-SHA256 09 HMAC-SHA1 08 HMAC-MD5
        let expected = b"\x00\x03\x0bHMAC-SHA256\x09HMAC-SHA1\x08HMAC-MD5";
        assert_eq!(&list[..], &expected[..]);
    }

    #[test]
    fn parse_alg_list_round_trips() {
        let encoded = encode_alg_list(&[b"HMAC-SHA256", b"HMAC-SHA1"]);
        let parsed = parse_alg_list(&encoded).expect("parse");
        assert_eq!(parsed.len(), 2);
        assert_eq!(&parsed[0], b"HMAC-SHA256");
        assert_eq!(&parsed[1], b"HMAC-SHA1");
    }

    #[test]
    fn parse_alg_list_rejects_truncated() {
        // count=3 but only one entry's worth of bytes.
        let truncated = b"\x00\x03\x05hello";
        assert!(parse_alg_list(truncated).is_none());
    }

    #[test]
    fn build_step1_login_contains_required_chunks() {
        let req = HopeStep1Request {
            trans: 1,
            mac_algs: &[b"HMAC-SHA256", b"HMAC-SHA1", b"HMAC-MD5"],
            cipher_algs: &[b"BLOWFISH"],
            compress_algs: &[],
            app_id: None,
            app_string: Some(b""),
        };
        let frame = build_step1_login(&req).expect("build");
        // hc should be: LOGIN + PASSWORD + MAC_ALG + APP_ID +
        // APP_STRING + CIPHER_ALG + SESSIONKEY = 7 chunks.
        let hc = u16::from_be_bytes([frame[20], frame[21]]);
        assert_eq!(hc, 7);
        // Header opcode is HTLC_HDR_LOGIN.
        assert_eq!(&frame[0..4], &HTLC_HDR_LOGIN.to_be_bytes());
    }

    #[test]
    fn select_algorithms_from_step1_reply() {
        let mut reply = LoginReply::default();
        reply.flag = 0;
        reply.sessionkey = Some(vec![0u8; 64]);
        reply.mac_alg = Some(encode_alg_list(&[b"HMAC-SHA256"]));
        reply.cipher_alg = Some(encode_alg_list(&[b"BLOWFISH"]));
        reply.cipher_mode = Some(b"STREAM".to_vec());

        let choice = select_algorithms(&reply).expect("choice");
        assert_eq!(&choice.mac_alg, b"HMAC-SHA256");
        assert_eq!(&choice.cipher_alg, b"BLOWFISH");
        assert_eq!(&choice.cipher_mode, b"STREAM");
        assert_eq!(choice.sessionkey.len(), 64);
    }

    #[test]
    fn select_algorithms_missing_sessionkey_returns_none() {
        let reply = LoginReply::default();
        assert!(select_algorithms(&reply).is_none());
    }

    #[test]
    fn hmac_password_sha256_produces_32_bytes() {
        let mac = hmac_password(b"secret", b"sessionkey1234", b"HMAC-SHA256").expect("hmac");
        assert_eq!(mac.len(), 32);
        // Same inputs → same output.
        let mac2 = hmac_password(b"secret", b"sessionkey1234", b"HMAC-SHA256").expect("hmac2");
        assert_eq!(mac, mac2);
        // Different password → different output.
        let mac3 = hmac_password(b"other", b"sessionkey1234", b"HMAC-SHA256").expect("hmac3");
        assert_ne!(mac, mac3);
    }

    #[test]
    fn hmac_password_unsupported_alg_errors() {
        let err = hmac_password(b"x", b"y", b"NOTREALMAC").unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::InvalidInput);
    }

    #[test]
    fn build_step2_login_contains_real_credentials() {
        let choice = HopeAlgorithmChoice {
            mac_alg: b"HMAC-SHA256".to_vec(),
            cipher_alg: b"BLOWFISH".to_vec(),
            cipher_mode: b"STREAM".to_vec(),
            compress_alg: None,
            sessionkey: vec![0u8; 64],
        };
        let mac = hmac_password(b"hunter2", &choice.sessionkey, &choice.mac_alg).expect("hmac");
        let req = HopeStep2Request {
            trans: 2,
            login: b"misha",
            password_mac: &mac,
            choice: &choice,
            name: b"Misha",
            icon: 0x7ffd,
            version: 0x00b9,
        };
        let frame = build_step2_login(&req).expect("build");
        assert_eq!(&frame[0..4], &HTLC_HDR_LOGIN.to_be_bytes());

        // Body should contain XOR'd login somewhere. Walk
        // chunks via hotline-proto.
        use hotline_proto::wire::ChunkIter;
        let mut found_login = false;
        let mut found_password_mac = false;
        for chunk in ChunkIter::over_message(&frame, frame.len()) {
            if chunk.tag == tag::LOGIN {
                let xored: Vec<u8> = b"misha".iter().map(|b| !b).collect();
                assert_eq!(chunk.data, &xored[..]);
                found_login = true;
            }
            if chunk.tag == tag::PASSWORD {
                assert_eq!(chunk.data, &mac[..], "password chunk should be HMAC output");
                found_password_mac = true;
            }
        }
        assert!(found_login, "LOGIN chunk missing");
        assert!(found_password_mac, "PASSWORD (HMAC) chunk missing");
    }
}
