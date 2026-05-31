//! ChaCha20-Poly1305 AEAD primitives for GtkHx.
//!
//! Replaces `cipher_aead.c`. Implements:
//!   - HKDF-SHA256 one-shot (extract + expand)
//!   - Session key derivation
//!   - HTXF transfer key derivation
//!   - Seal / Open frame codec with deterministic nonce construction
//!
//! All functions are pure; no protocol structs are touched here.

use chacha20poly1305::aead::{Aead, KeyInit, Payload};
use chacha20poly1305::{ChaCha20Poly1305, Key, Nonce};
use hkdf::Hkdf;
use sha2::Sha256;
use std::slice;

/// Maximum AEAD frame size (ciphertext + tag). 16 MiB cap.
pub const AEAD_MAX_FRAME_SIZE: u32 = 16 * 1024 * 1024;

/// Poly1305 tag size.
pub const AEAD_TAG_SIZE: usize = 16;

/// Length prefix size (big-endian u32).
pub const AEAD_LENGTH_PREFIX: usize = 4;

/// Direction byte for nonce: server → client.
pub const AEAD_DIR_SERVER_TO_CLIENT: u8 = 0x00;

/// Direction byte for nonce: client → server.
pub const AEAD_DIR_CLIENT_TO_SERVER: u8 = 0x01;

/// AEAD state for one direction of a connection.
#[repr(C)]
pub struct AeadState {
    pub key: [u8; 32],
    pub counter: u64,
    pub dir: u8,
}

impl AeadState {
    fn build_nonce(&self) -> [u8; 12] {
        let mut nonce = [0u8; 12];
        nonce[0] = self.dir;
        // bytes 1..3 = 0x000000
        // bytes 4..11 = counter (big-endian u64)
        nonce[4] = (self.counter >> 56) as u8;
        nonce[5] = (self.counter >> 48) as u8;
        nonce[6] = (self.counter >> 40) as u8;
        nonce[7] = (self.counter >> 32) as u8;
        nonce[8] = (self.counter >> 24) as u8;
        nonce[9] = (self.counter >> 16) as u8;
        nonce[10] = (self.counter >> 8) as u8;
        nonce[11] = self.counter as u8;
        nonce
    }
}

// ---- HKDF-SHA256 --------------------------------------------------------

/// One-shot HKDF-SHA256: extract + expand.
fn hkdf_sha256(salt: &[u8], ikm: &[u8], info: &[u8], out: &mut [u8]) {
    let hk = Hkdf::<Sha256>::new(Some(salt), ikm);
    hk.expand(info, out)
        .expect("HKDF expand should not fail for valid output length");
}

/// FFI: HKDF-SHA256 one-shot.
///
/// # Safety
/// All pointers must be valid for their respective lengths.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_aead_hkdf_sha256(
    salt: *const u8,
    salt_len: usize,
    ikm: *const u8,
    ikm_len: usize,
    info: *const u8,
    info_len: usize,
    out: *mut u8,
    out_len: usize,
) {
    let salt_s = if salt.is_null() { &[] } else { slice::from_raw_parts(salt, salt_len) };
    let ikm_s = if ikm.is_null() { &[] } else { slice::from_raw_parts(ikm, ikm_len) };
    let info_s = if info.is_null() { &[] } else { slice::from_raw_parts(info, info_len) };
    let out_s = slice::from_raw_parts_mut(out, out_len);
    hkdf_sha256(salt_s, ikm_s, info_s, out_s);
}

// ---- Key derivation -----------------------------------------------------

/// Derive session keys for AEAD mode.
///
/// # Safety
/// All pointers must be valid for their respective lengths.
/// `encode_out` and `decode_out` must point to valid `AeadState` structs.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_aead_derive_session_keys(
    encode_out: *mut AeadState,
    decode_out: *mut AeadState,
    session_key: *const u8,
    session_key_len: usize,
    encode_key: *const u8,
    encode_key_len: usize,
    decode_key: *const u8,
    decode_key_len: usize,
) {
    let sk = slice::from_raw_parts(session_key, session_key_len);
    let ek = slice::from_raw_parts(encode_key, encode_key_len);
    let dk = slice::from_raw_parts(decode_key, decode_key_len);

    let enc = &mut *encode_out;
    let dec = &mut *decode_out;

    *enc = std::mem::zeroed();
    *dec = std::mem::zeroed();

    // Spec's encode_key_256 — server's outbound key → client decode
    hkdf_sha256(sk, ek, b"hope-chacha-encode", &mut dec.key);
    // Spec's decode_key_256 — server's inbound key → client encode
    hkdf_sha256(sk, dk, b"hope-chacha-decode", &mut enc.key);

    enc.dir = AEAD_DIR_CLIENT_TO_SERVER;
    dec.dir = AEAD_DIR_SERVER_TO_CLIENT;
    enc.counter = 0;
    dec.counter = 0;
}

/// Derive per-transfer keys.
///
/// # Safety
/// All pointers must be valid.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_aead_derive_transfer_keys(
    xfer_encode_out: *mut AeadState,
    xfer_decode_out: *mut AeadState,
    session_key: *const u8,
    session_key_len: usize,
    ctrl_encode: *const AeadState,
    ctrl_decode: *const AeadState,
    ref_num: u32,
) {
    let sk = slice::from_raw_parts(session_key, session_key_len);
    let ce = &*ctrl_encode;
    let cd = &*ctrl_decode;

    // ft_base_key = HKDF(ikm = encode_key_256 || decode_key_256,
    //                     salt = session_key, info = "hope-file-transfer")
    // ctrl_decode->key holds encode_key_256, ctrl_encode->key holds decode_key_256
    let mut ikm_concat = [0u8; 64];
    ikm_concat[..32].copy_from_slice(&cd.key);
    ikm_concat[32..].copy_from_slice(&ce.key);

    let mut ft_base_key = [0u8; 32];
    hkdf_sha256(sk, &ikm_concat, b"hope-file-transfer", &mut ft_base_key);

    // transfer_key = HKDF(ikm = ft_base_key, salt = ref (4 bytes BE),
    //                      info = "hope-ft-ref")
    let ref_be = ref_num.to_be_bytes();
    let mut transfer_key = [0u8; 32];
    hkdf_sha256(&ref_be, &ft_base_key, b"hope-ft-ref", &mut transfer_key);

    let xe = &mut *xfer_encode_out;
    let xd = &mut *xfer_decode_out;

    *xe = std::mem::zeroed();
    *xd = std::mem::zeroed();

    xe.key = transfer_key;
    xd.key = transfer_key;

    xe.dir = AEAD_DIR_CLIENT_TO_SERVER;
    xd.dir = AEAD_DIR_SERVER_TO_CLIENT;
    xe.counter = 0;
    xd.counter = 0;
}

// ---- Frame codec --------------------------------------------------------

/// Seal: encrypt + authenticate plaintext into a length-prefixed frame.
///
/// Returns total bytes written (prefix + ciphertext + tag) on success, 0 on error.
///
/// # Safety
/// `plaintext` must be valid for `pt_len` bytes. `out` must be valid for `out_cap` bytes.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_aead_seal(
    state: *mut AeadState,
    plaintext: *const u8,
    pt_len: usize,
    out: *mut u8,
    out_cap: usize,
) -> usize {
    if state.is_null() || plaintext.is_null() || out.is_null() {
        return 0;
    }

    let st = &mut *state;

    if pt_len > (AEAD_MAX_FRAME_SIZE as usize) - AEAD_TAG_SIZE {
        return 0;
    }

    let framed_len = AEAD_LENGTH_PREFIX + pt_len + AEAD_TAG_SIZE;
    if out_cap < framed_len {
        return 0;
    }

    let pt_slice = slice::from_raw_parts(plaintext, pt_len);
    let out_slice = slice::from_raw_parts_mut(out, out_cap);

    // Length prefix = ciphertext + tag (not including the 4-byte prefix itself)
    let body_len = (pt_len + AEAD_TAG_SIZE) as u32;
    out_slice[0] = (body_len >> 24) as u8;
    out_slice[1] = (body_len >> 16) as u8;
    out_slice[2] = (body_len >> 8) as u8;
    out_slice[3] = body_len as u8;

    let nonce_bytes = st.build_nonce();
    let nonce = Nonce::from_slice(&nonce_bytes);
    let key = Key::from_slice(&st.key);
    let cipher = ChaCha20Poly1305::new(key);

    // No AAD
    let payload = Payload {
        msg: pt_slice,
        aad: &[],
    };

    match cipher.encrypt(nonce, payload) {
        Ok(ciphertext_with_tag) => {
            out_slice[AEAD_LENGTH_PREFIX..AEAD_LENGTH_PREFIX + ciphertext_with_tag.len()]
                .copy_from_slice(&ciphertext_with_tag);
            st.counter += 1;
            framed_len
        }
        Err(_) => 0,
    }
}

/// Peek at the length prefix to determine the total frame size.
/// Returns 0 if not enough data or invalid prefix.
///
/// # Safety
/// `framed` must be valid for `framed_len` bytes.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_aead_peek_frame_size(
    framed: *const u8,
    framed_len: usize,
) -> usize {
    if framed.is_null() || framed_len < AEAD_LENGTH_PREFIX {
        return 0;
    }

    let f = slice::from_raw_parts(framed, framed_len);
    let body_len = ((f[0] as u32) << 24)
        | ((f[1] as u32) << 16)
        | ((f[2] as u32) << 8)
        | (f[3] as u32);

    if body_len < AEAD_TAG_SIZE as u32 {
        return 0;
    }
    if body_len > AEAD_MAX_FRAME_SIZE {
        return 0;
    }

    AEAD_LENGTH_PREFIX + body_len as usize
}

/// Open: verify tag and decrypt a framed buffer.
///
/// Returns plaintext length on success, 0 on failure.
///
/// # Safety
/// `framed` must be valid for `framed_len` bytes. `out` must be valid for `out_cap` bytes.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_aead_open(
    state: *mut AeadState,
    framed: *const u8,
    framed_len: usize,
    out: *mut u8,
    out_cap: usize,
) -> usize {
    if state.is_null() || framed.is_null() || out.is_null() {
        return 0;
    }

    let frame_total = gtkhx_aead_peek_frame_size(framed, framed_len);
    if frame_total == 0 || framed_len < frame_total {
        return 0;
    }

    let f = slice::from_raw_parts(framed, frame_total);
    let body_len = frame_total - AEAD_LENGTH_PREFIX;
    let pt_len = body_len - AEAD_TAG_SIZE;
    if out_cap < pt_len {
        return 0;
    }

    let st = &mut *state;
    let nonce_bytes = st.build_nonce();
    let nonce = Nonce::from_slice(&nonce_bytes);
    let key = Key::from_slice(&st.key);
    let cipher = ChaCha20Poly1305::new(key);

    // ciphertext + tag is everything after the length prefix
    let ct_and_tag = &f[AEAD_LENGTH_PREFIX..];
    let payload = Payload {
        msg: ct_and_tag,
        aad: &[],
    };

    match cipher.decrypt(nonce, payload) {
        Ok(plaintext) => {
            let out_slice = slice::from_raw_parts_mut(out, out_cap);
            out_slice[..pt_len].copy_from_slice(&plaintext);
            st.counter += 1;
            pt_len
        }
        Err(_) => 0,
    }
}

/// Convenience: seal with allocation. Returns a newly allocated buffer
/// (caller must free with `gtkhx_aead_seal_alloc_free`), or null on failure.
/// Writes framed byte count to `*out_len` if non-null.
///
/// # Safety
/// `plaintext` must be valid for `pt_len` bytes.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_aead_seal_alloc(
    state: *mut AeadState,
    plaintext: *const u8,
    pt_len: usize,
    out_len: *mut usize,
) -> *mut u8 {
    let framed_cap = AEAD_LENGTH_PREFIX + pt_len + AEAD_TAG_SIZE;
    let mut buf = vec![0u8; framed_cap];
    let n = gtkhx_aead_seal(state, plaintext, pt_len, buf.as_mut_ptr(), framed_cap);
    if n == 0 {
        return std::ptr::null_mut();
    }
    if !out_len.is_null() {
        *out_len = n;
    }
    let ptr = buf.as_mut_ptr();
    std::mem::forget(buf);
    ptr
}

/// Free a buffer allocated by `gtkhx_aead_seal_alloc`.
///
/// # Safety
/// `ptr` must have been returned by `gtkhx_aead_seal_alloc` with the given `len`.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_aead_seal_alloc_free(ptr: *mut u8, len: usize) {
    if !ptr.is_null() && len > 0 {
        drop(Vec::from_raw_parts(ptr, len, len));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn seal_open_roundtrip() {
        let mut encode_state = AeadState {
            key: [0x42u8; 32],
            counter: 0,
            dir: AEAD_DIR_CLIENT_TO_SERVER,
        };
        let mut decode_state = AeadState {
            key: [0x42u8; 32],
            counter: 0,
            dir: AEAD_DIR_CLIENT_TO_SERVER,
        };

        let plaintext = b"Hello, Hotline AEAD!";
        let mut framed = vec![0u8; AEAD_LENGTH_PREFIX + plaintext.len() + AEAD_TAG_SIZE];

        unsafe {
            let sealed = gtkhx_aead_seal(
                &mut encode_state,
                plaintext.as_ptr(),
                plaintext.len(),
                framed.as_mut_ptr(),
                framed.len(),
            );
            assert_eq!(sealed, framed.len());
            assert_eq!(encode_state.counter, 1);

            let mut decrypted = vec![0u8; plaintext.len()];
            let opened = gtkhx_aead_open(
                &mut decode_state,
                framed.as_ptr(),
                framed.len(),
                decrypted.as_mut_ptr(),
                decrypted.len(),
            );
            assert_eq!(opened, plaintext.len());
            assert_eq!(&decrypted, plaintext);
            assert_eq!(decode_state.counter, 1);
        }
    }

    #[test]
    fn peek_frame_size_valid() {
        // A frame with body_len = 36 (20 bytes plaintext + 16 tag)
        let frame = [0x00, 0x00, 0x00, 0x24u8]; // 36 in big-endian
        unsafe {
            let size = gtkhx_aead_peek_frame_size(frame.as_ptr(), frame.len());
            assert_eq!(size, 4 + 36);
        }
    }

    #[test]
    fn peek_frame_size_too_small() {
        let frame = [0x00, 0x00]; // less than 4 bytes
        unsafe {
            let size = gtkhx_aead_peek_frame_size(frame.as_ptr(), frame.len());
            assert_eq!(size, 0);
        }
    }

    #[test]
    fn open_fails_on_tampered_frame() {
        let mut state = AeadState {
            key: [0x42u8; 32],
            counter: 0,
            dir: AEAD_DIR_CLIENT_TO_SERVER,
        };

        let plaintext = b"secret data";
        let mut framed = vec![0u8; AEAD_LENGTH_PREFIX + plaintext.len() + AEAD_TAG_SIZE];

        unsafe {
            let sealed = gtkhx_aead_seal(
                &mut state,
                plaintext.as_ptr(),
                plaintext.len(),
                framed.as_mut_ptr(),
                framed.len(),
            );
            assert!(sealed > 0);

            // Tamper with ciphertext
            framed[AEAD_LENGTH_PREFIX] ^= 0xff;

            // Reset counter for open
            state.counter = 0;
            let mut decrypted = vec![0u8; plaintext.len()];
            let opened = gtkhx_aead_open(
                &mut state,
                framed.as_ptr(),
                framed.len(),
                decrypted.as_mut_ptr(),
                decrypted.len(),
            );
            assert_eq!(opened, 0); // Authentication failure
            assert_eq!(state.counter, 0); // Counter not advanced
        }
    }

    #[test]
    fn session_key_derivation_produces_different_keys() {
        let session_key = [0xAA; 64];
        let encode_key = [0xBB; 20];
        let decode_key = [0xCC; 20];

        let mut enc = AeadState {
            key: [0; 32],
            counter: 0,
            dir: 0,
        };
        let mut dec = AeadState {
            key: [0; 32],
            counter: 0,
            dir: 0,
        };

        unsafe {
            gtkhx_aead_derive_session_keys(
                &mut enc,
                &mut dec,
                session_key.as_ptr(),
                session_key.len(),
                encode_key.as_ptr(),
                encode_key.len(),
                decode_key.as_ptr(),
                decode_key.len(),
            );
        }

        // Keys should be non-zero and different from each other
        assert_ne!(enc.key, [0u8; 32]);
        assert_ne!(dec.key, [0u8; 32]);
        assert_ne!(enc.key, dec.key);
        assert_eq!(enc.dir, AEAD_DIR_CLIENT_TO_SERVER);
        assert_eq!(dec.dir, AEAD_DIR_SERVER_TO_CLIENT);
    }

    #[test]
    fn hkdf_sha256_basic() {
        let salt = b"salt";
        let ikm = b"input key material";
        let info = b"context info";
        let mut out1 = [0u8; 32];
        let mut out2 = [0u8; 32];

        hkdf_sha256(salt, ikm, info, &mut out1);
        hkdf_sha256(salt, ikm, info, &mut out2);

        assert_ne!(out1, [0u8; 32]);
        assert_eq!(out1, out2); // Deterministic
    }
}
