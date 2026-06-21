//! HOPE post-handshake key derivation (Phase F-2 of
//! `hxnet-owns-the-whole-lifecycle`).
//!
//! After the HOPE step 2 reply succeeds (Phase F), both sides
//! derive the per-direction cipher keys from the sessionkey +
//! chosen MAC algorithm. The derivation is mechanical but
//! cipher-family-specific:
//!
//! - **Blowfish (stream)** — a three-step HMAC chain. The
//!   client's encode key is what the server uses as its decode
//!   key; the labels flip at the wire boundary so both sides
//!   produce identical keystreams.
//!
//! - **ChaCha20-Poly1305 (AEAD)** — HKDF-SHA256 over the
//!   sessionkey + the HMAC chain output (same `encode_key` /
//!   `decode_key` from Blowfish derivation are used as IKM for
//!   the AEAD key expansion). Mirrors `cipher_aead_derive_session_keys`
//!   on the C side.
//!
//! This module is the pure key-derivation arithmetic. The
//! actual transport rewrap — swapping the actor's stream from
//! plaintext to the cipher-wrapped variant mid-flight — is the
//! follow-up piece that touches the actor's lifecycle and lands
//! alongside Phase G's C-side rework.

use std::io;

/// Cipher kind chosen by the HOPE handshake. The label comes
/// out of [`crate::hope::HopeAlgorithmChoice::cipher_alg`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum HopeCipherKind {
    /// `BLOWFISH` in the cipher list.
    Blowfish,
    /// `CHACHA20-POLY1305` in the cipher list.
    ChaCha20Poly1305,
}

impl HopeCipherKind {
    /// Parse the chosen-cipher label into a kind enum. Returns
    /// `None` for unrecognised labels — production should
    /// surface that as a clean handshake failure rather than
    /// guess.
    pub fn from_label(label: &[u8]) -> Option<Self> {
        match label {
            b"BLOWFISH" => Some(Self::Blowfish),
            b"CHACHA20-POLY1305" => Some(Self::ChaCha20Poly1305),
            _ => None,
        }
    }
}

/// Per-direction Blowfish key material. Both directions get a
/// `key` of the same length (= MAC digest size); the OFB ivecs
/// start at zero per HOPE spec.
#[derive(Debug, Clone)]
pub struct BlowfishKeys {
    /// What the client uses to encrypt its writes. Server
    /// receives them and decrypts with the matching key on its
    /// side (which it derives the same way).
    pub encode_key: Vec<u8>,
    /// What the client uses to decrypt the server's writes.
    pub decode_key: Vec<u8>,
}

/// Per-direction ChaCha20-Poly1305 key material. 32 bytes
/// each, per the AEAD primitive's spec. Counter starts at 0.
#[derive(Debug, Clone)]
pub struct AeadKeys {
    pub encode_key: [u8; 32],
    pub decode_key: [u8; 32],
}

/// Compute the HOPE Blowfish HMAC chain.
///
/// Three-step derivation (mirrors `hope_compute_chain` in
/// `src/hope.c`):
///
///   password_mac = HMAC(password, sessionkey, macalg)
///   spec_encode  = HMAC(password, password_mac, macalg)
///   spec_decode  = HMAC(password, spec_encode,  macalg)
///
/// Returns `(password_mac, BlowfishKeys)`. The labels are
/// **crossed at the storage layer** vs. the spec — the client's
/// `encode_key` is the spec's `decode_key` and vice versa. The
/// C side has this same flip in `hope_store_chain_keys`; the
/// comment there has the historical rationale.
pub fn compute_blowfish_chain(
    password: &[u8],
    sessionkey: &[u8],
    mac_alg_label: &[u8],
) -> io::Result<(Vec<u8>, BlowfishKeys)> {
    let password_mac = crate::hope::hmac_password(password, sessionkey, mac_alg_label)?;
    let spec_encode =
        crate::hope::hmac_password(password, &password_mac, mac_alg_label)?;
    let spec_decode =
        crate::hope::hmac_password(password, &spec_encode, mac_alg_label)?;

    // Cross spec_encode / spec_decode into the client's
    // encode_key / decode_key. Same crossing the C side's
    // hope_store_chain_keys applies.
    let keys = BlowfishKeys {
        encode_key: spec_decode,
        decode_key: spec_encode,
    };
    Ok((password_mac, keys))
}

/// Derive ChaCha20-Poly1305 session keys from the HOPE
/// handshake outputs. Mirrors
/// `cipher_aead_derive_session_keys` in `src/cipher_aead.c`.
///
/// The encode_key / decode_key inputs come from the Blowfish
/// HMAC chain (`spec_encode_key` and `spec_decode_key` — the
/// _spec_ side labels, before the storage flip). HKDF-SHA256
/// over the sessionkey produces a fresh 32-byte AEAD key for
/// each direction.
///
/// The `info` strings (`hope-chacha-encode` / `hope-chacha-decode`)
/// are wire-pinned and must match the server's HKDF inputs
/// byte-exact — they're the protocol-level domain separator
/// that keeps the two directions' keystreams independent.
pub fn derive_aead_keys(
    sessionkey: &[u8],
    spec_encode_key: &[u8],
    spec_decode_key: &[u8],
) -> AeadKeys {
    use hkdf::Hkdf;
    use sha2::Sha256;

    let mut enc = [0u8; 32];
    let mut dec = [0u8; 32];
    // Same direction crossing as the Blowfish path — what the
    // spec calls encode_key derives the client's DECODE key
    // (because the server's encrypt-side is the client's
    // decrypt-side).
    // HKDF-expand of a 32-byte output (one SHA-256 block) is
    // infallible — it can only fail past 255*32 bytes. Discarding the
    // Result (the old `let _ =`) would silently leave the all-zero
    // buffers as "keys" if a future refactor changed the length, a
    // dangerous failure mode for crypto. Assert instead so it's loud,
    // not silent — see [[feedback_assert_over_debug_assert]].
    let hk_dec = Hkdf::<Sha256>::new(Some(sessionkey), spec_encode_key);
    hk_dec
        .expand(b"hope-chacha-encode", &mut dec)
        .expect("HKDF-SHA256 expand of 32 bytes is infallible");
    let hk_enc = Hkdf::<Sha256>::new(Some(sessionkey), spec_decode_key);
    hk_enc
        .expand(b"hope-chacha-decode", &mut enc)
        .expect("HKDF-SHA256 expand of 32 bytes is infallible");
    AeadKeys { encode_key: enc, decode_key: dec }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cipher_kind_from_label() {
        assert_eq!(HopeCipherKind::from_label(b"BLOWFISH"), Some(HopeCipherKind::Blowfish));
        assert_eq!(
            HopeCipherKind::from_label(b"CHACHA20-POLY1305"),
            Some(HopeCipherKind::ChaCha20Poly1305)
        );
        assert_eq!(HopeCipherKind::from_label(b"RC4"), None);
        assert_eq!(HopeCipherKind::from_label(b""), None);
    }

    #[test]
    fn compute_blowfish_chain_produces_keys_of_mac_digest_size() {
        let (password_mac, keys) =
            compute_blowfish_chain(b"hunter2", b"sessionkey1234", b"HMAC-SHA256")
                .expect("chain");
        assert_eq!(password_mac.len(), 32, "SHA256 password_mac");
        assert_eq!(keys.encode_key.len(), 32);
        assert_eq!(keys.decode_key.len(), 32);
        // encode and decode are derived from each other; they
        // must differ (else the chain didn't iterate).
        assert_ne!(keys.encode_key, keys.decode_key);
        // password_mac is the FIRST step output; encode / decode
        // shouldn't equal it.
        assert_ne!(keys.encode_key, password_mac);
        assert_ne!(keys.decode_key, password_mac);
    }

    #[test]
    fn compute_blowfish_chain_deterministic() {
        let (m1, k1) =
            compute_blowfish_chain(b"hunter2", b"sk1234", b"HMAC-SHA256").expect("1");
        let (m2, k2) =
            compute_blowfish_chain(b"hunter2", b"sk1234", b"HMAC-SHA256").expect("2");
        assert_eq!(m1, m2);
        assert_eq!(k1.encode_key, k2.encode_key);
        assert_eq!(k1.decode_key, k2.decode_key);
    }

    #[test]
    fn compute_blowfish_chain_different_password_different_keys() {
        let (_, k1) =
            compute_blowfish_chain(b"hunter2", b"sk", b"HMAC-SHA256").expect("1");
        let (_, k2) =
            compute_blowfish_chain(b"other", b"sk", b"HMAC-SHA256").expect("2");
        assert_ne!(k1.encode_key, k2.encode_key);
    }

    #[test]
    fn compute_blowfish_chain_sha1_produces_20_byte_keys() {
        let (password_mac, keys) =
            compute_blowfish_chain(b"x", b"sk", b"HMAC-SHA1").expect("chain");
        assert_eq!(password_mac.len(), 20);
        assert_eq!(keys.encode_key.len(), 20);
        assert_eq!(keys.decode_key.len(), 20);
    }

    #[test]
    fn derive_aead_keys_produces_32_byte_keys() {
        let sk = vec![0xa5u8; 64];
        let spec_enc = vec![0x12u8; 32];
        let spec_dec = vec![0x34u8; 32];
        let keys = derive_aead_keys(&sk, &spec_enc, &spec_dec);
        assert_ne!(keys.encode_key, [0u8; 32], "HKDF output shouldn't be all-zeros");
        assert_ne!(keys.decode_key, [0u8; 32]);
        assert_ne!(keys.encode_key, keys.decode_key);
    }

    #[test]
    fn derive_aead_keys_deterministic() {
        let sk = vec![0xa5u8; 64];
        let spec_enc = vec![0x12u8; 32];
        let spec_dec = vec![0x34u8; 32];
        let k1 = derive_aead_keys(&sk, &spec_enc, &spec_dec);
        let k2 = derive_aead_keys(&sk, &spec_enc, &spec_dec);
        assert_eq!(k1.encode_key, k2.encode_key);
        assert_eq!(k1.decode_key, k2.decode_key);
    }
}
