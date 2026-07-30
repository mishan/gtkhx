//! Stream cipher primitives for the Hotline HOPE handshake.
//!
//! Replaces the Blowfish OFB-64 crypto operations in src/cipher.c.
//! Today the only stream cipher offered is Blowfish — RC4 was retired
//! in claude/remove-rc4 (insecure stream cipher; advertising it under
//! a "Secure (HOPE)" label gave users a false sense of security).
//! ChaCha20-Poly1305 is the strongest negotiated path and lives in
//! the sister `hxcrypto-aead` crate.
//!
//! The C code retains a thin dispatcher in src/cipher.c that extracts
//! fields from `htlc_conn` and calls these Rust functions; all actual
//! cryptographic computation lives here.

#![allow(unsafe_op_in_unsafe_fn)]

mod blowfish_ofb;

pub use blowfish_ofb::{BlowfishOfb64State, BLOWFISH_OFB64_BLOCK_SIZE};

use std::slice;

/// Create a new opaque Blowfish OFB-64 state with the given key.
/// Returns NULL on invalid pointer / zero key length / unsupported
/// key length (Blowfish accepts 1..=56 bytes). Callers MUST free
/// non-NULL returns via `gtkhx_blowfish_ofb64_free`.
///
/// # Safety
/// `key` must be valid for `keylen` bytes if `keylen > 0`.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_blowfish_ofb64_new(
    key: *const u8,
    keylen: u32,
) -> *mut BlowfishOfb64State {
    if key.is_null() || keylen == 0 {
        return std::ptr::null_mut();
    }
    let key_slice = slice::from_raw_parts(key, keylen as usize);
    match BlowfishOfb64State::new(key_slice) {
        Some(state) => Box::into_raw(Box::new(state)),
        None => std::ptr::null_mut(),
    }
}

/// Free a state obtained from `gtkhx_blowfish_ofb64_new` /
/// `gtkhx_blowfish_ofb64_clone`. Safe on NULL.
///
/// # Safety
/// `state` must be a pointer returned by one of the
/// `gtkhx_blowfish_ofb64_*` constructors, or NULL.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_blowfish_ofb64_free(state: *mut BlowfishOfb64State) {
    if !state.is_null() {
        drop(Box::from_raw(state));
    }
}

/// Re-key the Blowfish state WITHOUT resetting the OFB ivec/num.
/// Matches the wire protocol's rekey behavior — see the comment on
/// `BlowfishOfb64State::set_key`. No-op on NULL / zero / oversized
/// key inputs; the existing key schedule is preserved so subsequent
/// crypt() still produces well-defined (if no longer wire-correct)
/// bytes rather than crashing.
///
/// # Safety
/// `state` must be a non-NULL state pointer; `key` must be valid
/// for `keylen` bytes if `keylen > 0`.
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
    let _ = (*state).set_key(key_slice);
}

/// Encrypt/decrypt using Blowfish OFB-64 (symmetric operation).
/// Safe for in-place operation.
///
/// # Safety
/// `state` must be a non-NULL state pointer; `src` and `dst` must
/// each be valid for `len` bytes.
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

/// Snapshot the OFB feedback state (ivec + num) into a caller-owned
/// 9-byte buffer (8 bytes ivec, 1 byte num). Used by the receive-side
/// rollback path in src/network_decode.c — replaces the previous
/// "clone the whole state" approach that allocated a fresh Blowfish
/// key schedule on every cipher_decode call. The key schedule doesn't
/// change during rollback, so saving + restoring it is wasted work.
///
/// # Safety
/// `state` must be a non-NULL state pointer; `out_ivec` must be
/// valid for 8 bytes; `out_num` must be a valid u32 pointer.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_blowfish_ofb64_save_state(
    state: *const BlowfishOfb64State,
    out_ivec: *mut u8,
    out_num: *mut u32,
) {
    if state.is_null() || out_ivec.is_null() || out_num.is_null() {
        return;
    }
    let mut ivec_buf = [0u8; BLOWFISH_OFB64_BLOCK_SIZE];
    let mut num = 0u32;
    (*state).save_ofb_state(&mut ivec_buf, &mut num);
    std::ptr::copy_nonoverlapping(ivec_buf.as_ptr(), out_ivec, BLOWFISH_OFB64_BLOCK_SIZE);
    *out_num = num;
}

/// Restore the OFB feedback state from a prior `_save_state`.
/// Pairs with `gtkhx_blowfish_ofb64_save_state` on the rollback path.
///
/// # Safety
/// `state` must be a non-NULL state pointer; `ivec` must be valid
/// for 8 bytes.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_blowfish_ofb64_restore_state(
    state: *mut BlowfishOfb64State,
    ivec: *const u8,
    num: u32,
) {
    if state.is_null() || ivec.is_null() {
        return;
    }
    let mut ivec_buf = [0u8; BLOWFISH_OFB64_BLOCK_SIZE];
    std::ptr::copy_nonoverlapping(ivec, ivec_buf.as_mut_ptr(), BLOWFISH_OFB64_BLOCK_SIZE);
    (*state).restore_ofb_state(&ivec_buf, num);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn blowfish_crypt_in_place_matches_crypt() {
        // crypt_in_place should produce the same bytes as crypt
        // against fresh state, byte-for-byte. Different scratch
        // shapes — single big call vs. byte-at-a-time — should
        // also agree, validating that the OFB state advances
        // identically.
        let key = b"keymaterial!!!";
        let plaintext: &[u8] = b"The quick brown fox jumps over the lazy dog";

        // Reference: classic two-buffer crypt.
        let mut ref_state = BlowfishOfb64State::new(key).expect("valid key");
        let mut reference = vec![0u8; plaintext.len()];
        ref_state.crypt(plaintext, &mut reference);

        // crypt_in_place, single big call.
        let mut state1 = BlowfishOfb64State::new(key).expect("valid key");
        let mut buf1 = plaintext.to_vec();
        state1.crypt_in_place(&mut buf1);
        assert_eq!(buf1, reference);

        // crypt_in_place, byte at a time — proves OFB state
        // advances per byte.
        let mut state2 = BlowfishOfb64State::new(key).expect("valid key");
        let mut buf2 = plaintext.to_vec();
        for i in 0..buf2.len() {
            state2.crypt_in_place(&mut buf2[i..i + 1]);
        }
        assert_eq!(buf2, reference);

        // Symmetric: decrypt in place using a fresh state.
        let mut decrypt_state = BlowfishOfb64State::new(key).expect("valid key");
        decrypt_state.crypt_in_place(&mut buf1);
        assert_eq!(buf1, plaintext);
    }

    #[test]
    fn blowfish_ofb64_roundtrip() {
        let key = b"blowfishkey";
        let plaintext = b"Hotline protocol data that spans multiple blocks!";

        let mut state1 = BlowfishOfb64State::new(key).expect("valid key");
        let mut ciphertext = vec![0u8; plaintext.len()];
        state1.crypt(plaintext, &mut ciphertext);

        assert_ne!(&ciphertext[..], &plaintext[..]);

        // OFB is symmetric with the same state — need a fresh state
        let mut state2 = BlowfishOfb64State::new(key).expect("valid key");
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
        let mut state1 = BlowfishOfb64State::new(key).expect("valid key");
        let mut full = vec![0u8; plaintext.len()];
        state1.crypt(plaintext, &mut full);

        // Byte by byte
        let mut state2 = BlowfishOfb64State::new(key).expect("valid key");
        let mut incremental = vec![0u8; plaintext.len()];
        for i in 0..plaintext.len() {
            state2.crypt(&plaintext[i..i + 1], &mut incremental[i..i + 1]);
        }

        assert_eq!(full, incremental);
    }

    #[test]
    fn blowfish_rekey_does_not_reset_ivec() {
        // The wire protocol requires that rekeying only changes the key
        // schedule but does NOT reset the OFB ivec/num state.
        let key1 = b"initial_key_1234";
        let key2 = b"rotated_key_5678";
        let data = [0x42u8; 32];

        // Encrypt 8 bytes to advance ivec, then rekey
        let mut state = BlowfishOfb64State::new(key1).expect("valid key");
        let mut throwaway = [0u8; 8];
        state.crypt(&data[..8], &mut throwaway);

        // Now rekey — ivec/num should stay where they are
        assert!(state.set_key(key2));
        let mut out_after_rekey = [0u8; 8];
        state.crypt(&data[..8], &mut out_after_rekey);

        // Compare against a fresh state with key2 that also encrypted 8 bytes
        // first — this should NOT match because the fresh state has a zeroed
        // ivec while the rekeyed state has the advanced ivec from key1.
        let mut fresh = BlowfishOfb64State::new(key2).expect("valid key");
        let mut out_fresh = [0u8; 8];
        fresh.crypt(&data[..8], &mut out_fresh);

        assert_ne!(out_after_rekey, out_fresh);
    }

    #[test]
    fn blowfish_save_restore_round_trips_ofb_state() {
        // Save → advance state with crypt → restore → re-run the
        // same crypt — the second output must match the first.
        let key = b"saverestoretest";
        let plaintext = b"0123456789abcdef0123456789abcdef";

        let mut state = BlowfishOfb64State::new(key).expect("valid key");
        // Burn some bytes so ivec/num aren't at the block boundary.
        let mut throwaway = [0u8; 5];
        state.crypt(&plaintext[..5], &mut throwaway);

        let mut saved_ivec = [0u8; BLOWFISH_OFB64_BLOCK_SIZE];
        let mut saved_num = 0u32;
        state.save_ofb_state(&mut saved_ivec, &mut saved_num);

        // First crypt at saved point
        let mut first = vec![0u8; 16];
        state.crypt(&plaintext[5..21], &mut first);

        // Restore and re-crypt the same span
        state.restore_ofb_state(&saved_ivec, saved_num);
        let mut second = vec![0u8; 16];
        state.crypt(&plaintext[5..21], &mut second);

        assert_eq!(first, second);
    }

    #[test]
    fn blowfish_new_rejects_invalid_key_lengths() {
        // 0-byte and 57+-byte keys are outside Blowfish's accepted range.
        assert!(BlowfishOfb64State::new(&[]).is_none());
        let oversized = vec![0u8; 57];
        assert!(BlowfishOfb64State::new(&oversized).is_none());
    }

    #[test]
    fn blowfish_set_key_returns_false_on_invalid_length() {
        let mut state = BlowfishOfb64State::new(b"valid").expect("valid key");
        assert!(!state.set_key(&[]));
        let oversized = vec![0u8; 57];
        assert!(!state.set_key(&oversized));
    }

    #[test]
    fn blowfish_restore_ofb_state_masks_num() {
        // A hostile / corrupted caller could pass num=999 through the
        // FFI; without masking, the next crypt() would index ivec[999]
        // and panic. With the mask, num lands in 0..=7 and crypt()
        // proceeds with whatever feedback byte that corresponds to.
        let mut state = BlowfishOfb64State::new(b"keykey").expect("valid key");
        let ivec = [0xaau8; BLOWFISH_OFB64_BLOCK_SIZE];
        // num=999 → masked to 999 & 7 = 7
        state.restore_ofb_state(&ivec, 999);
        let mut out = [0u8; 1];
        state.crypt(&[0u8; 1], &mut out); // must not panic
                                          // num=u32::MAX → masked to 7
        state.restore_ofb_state(&ivec, u32::MAX);
        state.crypt(&[0u8; 1], &mut out); // must not panic
    }
}
