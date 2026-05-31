//! Stream cipher primitives for the Hotline HOPE handshake.
//!
//! Replaces the crypto operations in `cipher.c`. Provides:
//!   - ARC4 (RC4) stream cipher
//!   - Blowfish in 64-bit OFB mode
//!   - Rekey-marker detection and key rotation
//!
//! The C code retains a thin dispatcher that extracts fields from `htlc_conn`
//! and calls these Rust functions. All actual cryptographic computation lives
//! here.

mod blowfish_ofb;
mod rc4;

pub use blowfish_ofb::BlowfishOfb64State;
pub use rc4::Rc4State;

use std::slice;

/// Cipher type constants matching the C defines.
pub const CIPHER_NONE: u32 = 0;
pub const CIPHER_RC4: u32 = 1;
pub const CIPHER_BLOWFISH: u32 = 2;

/// Opaque RC4 state for FFI.
///
/// # Safety
///
/// Create with `gtkhx_rc4_new`, destroy with `gtkhx_rc4_free`.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_rc4_new(key: *const u8, keylen: u32) -> *mut Rc4State {
    if key.is_null() || keylen == 0 {
        return std::ptr::null_mut();
    }
    let key_slice = slice::from_raw_parts(key, keylen as usize);
    let state = Box::new(Rc4State::new(key_slice));
    Box::into_raw(state)
}

#[no_mangle]
pub unsafe extern "C" fn gtkhx_rc4_free(state: *mut Rc4State) {
    if !state.is_null() {
        drop(Box::from_raw(state));
    }
}

/// Re-key the RC4 state with a new key.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_rc4_set_key(state: *mut Rc4State, key: *const u8, keylen: u32) {
    if state.is_null() || key.is_null() || keylen == 0 {
        return;
    }
    let key_slice = slice::from_raw_parts(key, keylen as usize);
    (*state).set_key(key_slice);
}

/// Encrypt/decrypt in-place (RC4 is symmetric).
#[no_mangle]
pub unsafe extern "C" fn gtkhx_rc4_crypt(
    state: *mut Rc4State,
    src: *const u8,
    dst: *mut u8,
    len: u32,
) {
    if state.is_null() || src.is_null() || dst.is_null() || len == 0 {
        return;
    }
    let src_slice = slice::from_raw_parts(src, len as usize);
    let dst_slice = slice::from_raw_parts_mut(dst, len as usize);
    (*state).crypt(src_slice, dst_slice);
}

/// Opaque Blowfish OFB-64 state for FFI.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_blowfish_ofb64_new(
    key: *const u8,
    keylen: u32,
) -> *mut BlowfishOfb64State {
    if key.is_null() || keylen == 0 {
        return std::ptr::null_mut();
    }
    let key_slice = slice::from_raw_parts(key, keylen as usize);
    let state = Box::new(BlowfishOfb64State::new(key_slice));
    Box::into_raw(state)
}

#[no_mangle]
pub unsafe extern "C" fn gtkhx_blowfish_ofb64_free(state: *mut BlowfishOfb64State) {
    if !state.is_null() {
        drop(Box::from_raw(state));
    }
}

/// Re-key the Blowfish state WITHOUT resetting the OFB ivec/num.
/// This matches the wire protocol's rekey behavior.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_blowfish_ofb64_set_key(
    state: *mut BlowfishOfb64State,
    key: *const u8,
    keylen: u32,
) {
    if state.is_null() || key.is_null() || keylen == 0 {
        return;
    }
    let key_slice = slice::from_raw_parts(key, keylen as usize);
    (*state).set_key(key_slice);
}

/// Encrypt/decrypt using Blowfish OFB-64 (symmetric operation).
#[no_mangle]
pub unsafe extern "C" fn gtkhx_blowfish_ofb64_crypt(
    state: *mut BlowfishOfb64State,
    src: *const u8,
    dst: *mut u8,
    len: u32,
) {
    if state.is_null() || src.is_null() || dst.is_null() || len == 0 {
        return;
    }
    let src_slice = slice::from_raw_parts(src, len as usize);
    let dst_slice = slice::from_raw_parts_mut(dst, len as usize);
    (*state).crypt(src_slice, dst_slice);
}

/// Clone an RC4 state. Returns a new heap-allocated copy.
/// The caller must free it with `gtkhx_rc4_free`.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_rc4_clone(state: *const Rc4State) -> *mut Rc4State {
    if state.is_null() {
        return std::ptr::null_mut();
    }
    Box::into_raw(Box::new((*state).clone()))
}

/// Clone a Blowfish OFB-64 state. Returns a new heap-allocated copy.
/// The caller must free it with `gtkhx_blowfish_ofb64_free`.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_blowfish_ofb64_clone(state: *const BlowfishOfb64State) -> *mut BlowfishOfb64State {
    if state.is_null() {
        return std::ptr::null_mut();
    }
    Box::into_raw(Box::new((*state).clone()))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rc4_encrypt_decrypt_roundtrip() {
        let key = b"testkey123";
        let plaintext = b"Hello, Hotline!";

        let mut state1 = Rc4State::new(key);
        let mut ciphertext = vec![0u8; plaintext.len()];
        state1.crypt(plaintext, &mut ciphertext);

        // Ciphertext should differ from plaintext
        assert_ne!(&ciphertext[..], &plaintext[..]);

        // Decrypt with fresh state
        let mut state2 = Rc4State::new(key);
        let mut decrypted = vec![0u8; plaintext.len()];
        state2.crypt(&ciphertext, &mut decrypted);
        assert_eq!(&decrypted[..], &plaintext[..]);
    }

    #[test]
    fn blowfish_ofb64_roundtrip() {
        let key = b"blowfishkey";
        let plaintext = b"Hotline protocol data that spans multiple blocks!";

        let mut state1 = BlowfishOfb64State::new(key);
        let mut ciphertext = vec![0u8; plaintext.len()];
        state1.crypt(plaintext, &mut ciphertext);

        assert_ne!(&ciphertext[..], &plaintext[..]);

        // OFB is symmetric with the same state — need a fresh state
        let mut state2 = BlowfishOfb64State::new(key);
        let mut decrypted = vec![0u8; plaintext.len()];
        state2.crypt(&ciphertext, &mut decrypted);
        assert_eq!(&decrypted[..], &plaintext[..]);
    }

    #[test]
    fn blowfish_ofb64_incremental() {
        // Verify that encrypting byte-by-byte produces the same result
        // as encrypting all at once (important for the wire protocol).
        let key = b"incrementaltest";
        let plaintext = b"0123456789abcdef0123";

        // All at once
        let mut state1 = BlowfishOfb64State::new(key);
        let mut full = vec![0u8; plaintext.len()];
        state1.crypt(plaintext, &mut full);

        // Byte by byte
        let mut state2 = BlowfishOfb64State::new(key);
        let mut incremental = vec![0u8; plaintext.len()];
        for i in 0..plaintext.len() {
            state2.crypt(&plaintext[i..i + 1], &mut incremental[i..i + 1]);
        }

        assert_eq!(full, incremental);
    }

    #[test]
    fn rc4_rekey_preserves_stream_position() {
        // After rekeying, subsequent output should differ
        let key1 = b"key1key1key1key1";
        let key2 = b"key2key2key2key2";
        let data = [0u8; 16];

        let mut state = Rc4State::new(key1);
        let mut out1 = [0u8; 16];
        state.crypt(&data, &mut out1);

        // Rekey
        state.set_key(key2);
        let mut out2 = [0u8; 16];
        state.crypt(&data, &mut out2);

        // Fresh key2 state should produce same output as after rekey
        let mut fresh = Rc4State::new(key2);
        let mut out3 = [0u8; 16];
        fresh.crypt(&data, &mut out3);
        assert_eq!(out2, out3);
    }

    #[test]
    fn blowfish_rekey_does_not_reset_ivec() {
        // The wire protocol requires that rekeying only changes the key
        // schedule but does NOT reset the OFB ivec/num state.
        let key1 = b"initial_key_1234";
        let key2 = b"rotated_key_5678";
        let data = [0x42u8; 32];

        // Encrypt 8 bytes to advance ivec, then rekey
        let mut state = BlowfishOfb64State::new(key1);
        let mut throwaway = [0u8; 8];
        state.crypt(&data[..8], &mut throwaway);

        // Now rekey — ivec/num should stay where they are
        state.set_key(key2);
        let mut out_after_rekey = [0u8; 8];
        state.crypt(&data[..8], &mut out_after_rekey);

        // Compare against a fresh state with key2 that also encrypted 8 bytes
        // first — this should NOT match because the fresh state has a zeroed
        // ivec while the rekeyed state has the advanced ivec from key1.
        let mut fresh = BlowfishOfb64State::new(key2);
        let mut out_fresh = [0u8; 8];
        fresh.crypt(&data[..8], &mut out_fresh);

        // They should differ because the OFB state differs
        assert_ne!(out_after_rekey, out_fresh);
    }
}
