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
    TAG_HOPE_APP_ID, TAG_HOPE_APP_STRING, TAG_MAC_ALG, TAG_SESSIONKEY, TAG_S_DATA_CIPHER_ALG,
    TAG_S_DATA_CIPHER_MODE, TAG_S_DATA_COMPRESS_ALG,
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
/// Returns the encoded bytes, or `Err(InvalidInput)` if any label is
/// longer than the 1-byte length prefix can represent (> 255 bytes).
/// Caller drops the buffer into a `PackChunk` for the matching tag.
///
/// Returns a `Result` rather than asserting: this is a `pub fn` whose
/// inputs can reach it across the FFI boundary, where a panic would
/// abort the process. Real alg labels are fixed wire constants well
/// under 255, so the error path is unreachable in practice — but a
/// clean handshake failure beats a process kill.
pub fn encode_alg_list(names: &[&[u8]]) -> io::Result<Vec<u8>> {
    if names.len() > u16::MAX as usize {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            format!(
                "alg list has {} entries, exceeds the u16 count field",
                names.len()
            ),
        ));
    }
    let count = names.len() as u16;
    // Validate each label (≤ 255) AND size the buffer with checked_add in
    // the same pass, before allocating. Doing the length check up front
    // means a huge or garbage label slice arriving via the FFI can't
    // drive a giant `Vec::with_capacity` (OOM) before we'd have rejected
    // it.
    let mut total: usize = 2;
    for n in names {
        if n.len() > 255 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                format!(
                    "alg label exceeds the 1-byte length prefix: {} bytes",
                    n.len()
                ),
            ));
        }
        total = total.checked_add(1 + n.len()).ok_or_else(|| {
            io::Error::new(
                io::ErrorKind::InvalidInput,
                "alg list total length overflows usize",
            )
        })?;
    }
    let mut out = Vec::with_capacity(total);
    out.extend_from_slice(&count.to_be_bytes());
    for n in names {
        out.push(n.len() as u8);
        out.extend_from_slice(n);
    }
    Ok(out)
}

/// Parse the BE u16 count + length-prefixed names back out of
/// an encoded algorithm list. Used to inspect what the server
/// chose in its step-1 reply.
pub fn parse_alg_list(buf: &[u8]) -> Option<Vec<Vec<u8>>> {
    if buf.len() < 2 {
        return None;
    }
    let count = u16::from_be_bytes([buf[0], buf[1]]) as usize;
    // Hard cap on the accepted entry count. A HOPE MAC / cipher /
    // compression list has a handful of entries in practice; without a
    // cap an attacker-controlled reply could claim a count of tens of
    // thousands (0-length labels in a ~1 MiB frame) and force a large
    // `Vec::with_capacity` plus many per-entry allocations — a cheap
    // DoS. 64 is far above any realistic preference list, and the
    // callers only take the first entry anyway.
    const MAX_ALG_ENTRIES: usize = 64;
    if count > MAX_ALG_ENTRIES {
        return None;
    }
    // Each entry is also at least a 1-byte length prefix, so a valid
    // `count` can't exceed the bytes remaining after the 2-byte count
    // field — reject a truncated buffer claiming more.
    if count > buf.len() - 2 {
        return None;
    }
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
    let mac_list = encode_alg_list(req.mac_algs)?;
    let cipher_list = encode_alg_list(req.cipher_algs)?;
    let compress_list = encode_alg_list(req.compress_algs)?;
    let app_id = req.app_id.unwrap_or(HOPE_APP_ID);
    // HOPE_APP_ID is a fixed 4-byte OSType on the wire. A different
    // length would emit an invalid frame; reject it with a clean
    // handshake error rather than ship malformed bytes. (Err, not
    // assert!, because app_id can reach here from the FFI caller and a
    // panic across that boundary aborts the process.)
    if app_id.len() != 4 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "HOPE app_id must be a 4-byte OSType",
        ));
    }

    // LOGIN + PASSWORD carry a single 0 byte, NOT a zero-length
    // chunk. The spec says zero-length, but every server we've
    // tested (mhxd, Janus) expects the 1-byte placeholder — an
    // empty LOGIN/PASSWORD reads as a *plaintext* guest login, so
    // the server accepts it and replies with an empty TASK instead
    // of a HOPE step-1 reply (no sessionkey). Mirrors
    // the legacy HOPE step-1 client. Caught against
    // live Janus, which returned a 22-byte (hc=0) reply until this
    // was fixed.
    let zero_placeholder = [0u8; 1];
    let mut chunks: Vec<PackChunk<'_>> = Vec::with_capacity(8);
    chunks.push(PackChunk {
        tag: tag::LOGIN,
        data: &zero_placeholder,
    });
    chunks.push(PackChunk {
        tag: tag::PASSWORD,
        data: &zero_placeholder,
    });
    chunks.push(PackChunk {
        tag: TAG_MAC_ALG,
        data: &mac_list,
    });
    chunks.push(PackChunk {
        tag: TAG_HOPE_APP_ID,
        data: app_id,
    });
    if let Some(s) = req.app_string {
        chunks.push(PackChunk {
            tag: TAG_HOPE_APP_STRING,
            data: s,
        });
    }
    chunks.push(PackChunk {
        tag: TAG_C_DATA_CIPHER_ALG,
        data: &cipher_list,
    });
    if !req.compress_algs.is_empty() {
        chunks.push(PackChunk {
            tag: TAG_C_DATA_COMPRESS_ALG,
            data: &compress_list,
        });
    }
    // Empty sessionkey — server fills it in.
    chunks.push(PackChunk {
        tag: TAG_SESSIONKEY,
        data: &[],
    });

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
/// Returns `None` if a *required* field is missing (sessionkey or
/// mac_alg). The cipher is OPTIONAL: an absent or empty cipher list
/// means the server negotiated no cipher (HMAC-authenticated login over
/// a plaintext transport — mhxd's non-`cipher_only` secure-login mode),
/// and `cipher_alg` comes back empty. A cipher list that is present and
/// non-empty but unparseable is still treated as an error (`None`), so
/// a malformed cipher on a genuinely-ciphered login fails loudly rather
/// than silently downgrading to plaintext.
pub fn select_algorithms(reply: &crate::login_reply::LoginReply) -> Option<HopeAlgorithmChoice> {
    let sessionkey = reply.sessionkey.clone()?;
    let mac_list = reply.mac_alg.as_ref().and_then(|b| parse_alg_list(b))?;
    let mac_alg = mac_list.into_iter().next()?;
    let cipher_alg = match reply.cipher_alg.as_ref() {
        None => Vec::new(),
        Some(b) if b.is_empty() => Vec::new(),
        Some(b) => parse_alg_list(b).and_then(|l| l.into_iter().next())?,
    };
    let cipher_mode = reply
        .cipher_mode
        .clone()
        .unwrap_or_else(|| b"STREAM".to_vec());
    Some(HopeAlgorithmChoice {
        mac_alg,
        cipher_alg,
        cipher_mode,
        // Compression is intentionally NOT negotiated by the
        // orchestrator's first cut: `run_hope_lifecycle` advertises an
        // empty `compress_algs` list in step 1, so the server never
        // picks one and never sends `TAG_S_DATA_COMPRESS_ALG` in its
        // reply. Leaving this `None` keeps `build_step2_login` from
        // echoing a COMPRESS_ALG the transport (composed with
        // `CompressionKind::None`) wouldn't actually apply — echoing
        // without applying would commit us to compressing while sending
        // plaintext, desyncing the server. Wiring compression means all
        // three together: advertise algs in step 1, populate this from
        // the parsed reply, AND pass the matching `CompressionKind` to
        // `compose()` in the lifecycle.
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
        io::Error::new(
            io::ErrorKind::InvalidInput,
            format!("MAC alg label not UTF-8: {e}"),
        )
    })?;
    // hxcrypto-hash's hmac_xxx writes into a fixed 32-byte
    // buffer and returns the digest length.
    let mut md = [0u8; 32];
    let len = hxcrypto::hash::hmac_xxx(&mut md, password, sessionkey, alg_str);
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
    /// Real login bytes. Encoded on the wire as `HMAC(login,
    /// sessionkey)` when `secure_login` is set, else XOR-0xFF (same
    /// as plaintext login). See [`HopeStep2Request::secure_login`].
    pub login: &'a [u8],
    /// Whether the server runs the secure_login variant (its step-1
    /// reply echoed the login). When true, the step-2 LOGIN field is
    /// the HMAC of the login under the sessionkey + chosen MAC,
    /// matching `src/hope.c::hope_build_login_field`'s
    /// `secure_login` branch. mhxd guest needs this; Janus guest
    /// does not.
    pub secure_login: bool,
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
    /// Capability bitmask (`HTLC_CAP_*`). Always emitted in step 2
    /// (matching the legacy HOPE step-2 client, which sends it
    /// unconditionally — the server needs the caps echo to finalise
    /// the session, and some servers reject a step-2 that omits it).
    pub caps: u16,
}

/// Build the HOPE step 2 LOGIN frame.
pub fn build_step2_login(req: &HopeStep2Request<'_>) -> io::Result<Vec<u8>> {
    // Encode the login field. secure_login servers (mhxd) expect
    // HMAC(login, sessionkey) under the chosen MAC; everyone else
    // (Janus guest) expects the plaintext XOR-0xFF form. Mirrors
    // src/hope.c::hope_build_login_field.
    let login_x: Vec<u8> = if req.secure_login {
        hmac_password(req.login, &req.choice.sessionkey, &req.choice.mac_alg)?
    } else {
        req.login.iter().map(|b| !b).collect()
    };

    // Re-encode the chosen cipher/compress as a single-entry list,
    // which is what step 2 echoes back to the server. When no cipher
    // was negotiated (empty cipher_alg), omit the CIPHER_ALG chunk
    // entirely rather than echo a one-entry list with an empty string —
    // the server treats a present-but-invalid cipher list as a hard
    // error and closes the connection (mhxd rcv.c step-2 handler), while
    // an absent chunk correctly means "no cipher".
    let cipher_list_back = if req.choice.cipher_alg.is_empty() {
        None
    } else {
        Some(encode_alg_list(&[&req.choice.cipher_alg])?)
    };
    let compress_list_back = req
        .choice
        .compress_alg
        .as_ref()
        .map(|c| encode_alg_list(&[c]))
        .transpose()?;

    let icon_be = req.icon.to_be_bytes();
    let version_be = req.version.to_be_bytes();
    let caps_be = req.caps.to_be_bytes();

    let mut chunks: Vec<PackChunk<'_>> = Vec::with_capacity(9);
    chunks.push(PackChunk {
        tag: tag::LOGIN,
        data: &login_x,
    });
    chunks.push(PackChunk {
        tag: tag::PASSWORD,
        data: req.password_mac,
    });
    if let Some(c) = &cipher_list_back {
        chunks.push(PackChunk {
            tag: TAG_S_DATA_CIPHER_ALG,
            data: c,
        });
    }
    if let Some(c) = &compress_list_back {
        chunks.push(PackChunk {
            tag: TAG_S_DATA_COMPRESS_ALG,
            data: c,
        });
    }
    // NAME + ICON — always emit, even empty/zero. Mirrors
    // the legacy HOPE step-2 client, which emits both unconditionally.
    // mhxd rejects (and silently closes) a step-2 that omits them;
    // Janus tolerates their absence. Caught against live mhxd.
    chunks.push(PackChunk {
        tag: tag::NAME,
        data: req.name,
    });
    chunks.push(PackChunk {
        tag: tag::ICON,
        data: &icon_be,
    });
    if req.version != 0 {
        chunks.push(PackChunk {
            tag: crate::login::TAG_VERSION,
            data: &version_be,
        });
    }
    // Echo cipher_mode if non-default — preserves the STREAM/
    // AEAD distinction the server expects.
    if req.choice.cipher_mode != b"STREAM" {
        chunks.push(PackChunk {
            tag: TAG_S_DATA_CIPHER_MODE,
            data: &req.choice.cipher_mode,
        });
    }
    // CAPABILITIES — always emit (mirrors the legacy HOPE step-2 client).
    chunks.push(PackChunk {
        tag: crate::login::TAG_CAPABILITIES,
        data: &caps_be,
    });

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
        let list = encode_alg_list(&[b"HMAC-SHA256", b"HMAC-SHA1", b"HMAC-MD5"]).unwrap();
        // Byte-exact against the trace from production:
        // 00 03 0b HMAC-SHA256 09 HMAC-SHA1 08 HMAC-MD5
        let expected = b"\x00\x03\x0bHMAC-SHA256\x09HMAC-SHA1\x08HMAC-MD5";
        assert_eq!(&list[..], &expected[..]);
    }

    #[test]
    fn parse_alg_list_round_trips() {
        let encoded = encode_alg_list(&[b"HMAC-SHA256", b"HMAC-SHA1"]).unwrap();
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
        reply.mac_alg = Some(encode_alg_list(&[b"HMAC-SHA256"]).unwrap());
        reply.cipher_alg = Some(encode_alg_list(&[b"BLOWFISH"]).unwrap());
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
    fn select_algorithms_empty_cipher_means_no_cipher() {
        // mhxd's non-cipher_only secure-login: sessionkey + MAC present,
        // an empty (length-0) cipher list. The choice should carry an
        // empty cipher_alg (no transport cipher), not return None.
        let mut reply = LoginReply::default();
        reply.sessionkey = Some(vec![0u8; 64]);
        reply.mac_alg = Some(encode_alg_list(&[b"HMAC-SHA256"]).unwrap());
        reply.cipher_alg = Some(Vec::new()); // empty cipher list

        let choice = select_algorithms(&reply).expect("choice");
        assert_eq!(&choice.mac_alg, b"HMAC-SHA256");
        assert!(choice.cipher_alg.is_empty(), "no cipher negotiated");
    }

    #[test]
    fn select_algorithms_absent_cipher_means_no_cipher() {
        // Cipher chunk entirely absent → also no cipher.
        let mut reply = LoginReply::default();
        reply.sessionkey = Some(vec![0u8; 64]);
        reply.mac_alg = Some(encode_alg_list(&[b"HMAC-SHA256"]).unwrap());
        reply.cipher_alg = None;

        let choice = select_algorithms(&reply).expect("choice");
        assert!(choice.cipher_alg.is_empty());
    }

    #[test]
    fn build_step2_login_omits_cipher_chunk_when_no_cipher() {
        // No-cipher secure login: step 2 must NOT echo a CIPHER_ALG
        // chunk (an empty-entry list would make mhxd close the
        // connection). It still carries LOGIN (HMAC) + PASSWORD.
        let choice = HopeAlgorithmChoice {
            mac_alg: b"HMAC-SHA256".to_vec(),
            cipher_alg: Vec::new(),
            cipher_mode: b"STREAM".to_vec(),
            compress_alg: None,
            sessionkey: vec![0u8; 64],
        };
        let mac = hmac_password(b"pw", &choice.sessionkey, &choice.mac_alg).expect("hmac");
        let req = HopeStep2Request {
            trans: 2,
            login: b"guest",
            password_mac: &mac,
            choice: &choice,
            name: b"",
            icon: 0,
            version: 185,
            caps: 0x001f,
            secure_login: true,
        };
        let frame = build_step2_login(&req).expect("build");

        use hotline_proto::wire::ChunkIter;
        let mut saw_cipher = false;
        let mut saw_login = false;
        for chunk in ChunkIter::over_message(&frame, frame.len()) {
            if chunk.tag == TAG_S_DATA_CIPHER_ALG {
                saw_cipher = true;
            }
            if chunk.tag == tag::LOGIN {
                saw_login = true;
            }
        }
        assert!(!saw_cipher, "step 2 must omit CIPHER_ALG when no cipher");
        assert!(saw_login, "step 2 must still carry the LOGIN chunk");
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
            caps: 0x001f,
            secure_login: false,
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
