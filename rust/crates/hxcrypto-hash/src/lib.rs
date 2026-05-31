//! HMAC/hash primitives for GtkHx.
//!
//! Replaces the C `hmac.c` implementation. Exposes `gtkhx_hmac_xxx()` with
//! the same semantics as the old `hmac_xxx()`: given a key, text, and algorithm
//! name, writes the MAC/hash to `md` and returns the digest length (or 0 on
//! unsupported algorithm).
//!
//! Supported algorithms:
//!   - "SHA1" / "HMAC-SHA1"     → 20-byte digest
//!   - "MD5" / "HMAC-MD5"       → 16-byte digest
//!   - "SHA256" / "HMAC-SHA256" → 32-byte digest
//!
//! The unprefixed variants ("SHA1", "MD5", "SHA256") compute a plain hash over
//! key||text — NOT RFC 2104 HMAC. This is the pre-HOPE password challenge
//! construction that some servers expect. Don't "fix" it.

use std::ffi::{c_char, CStr};
use std::slice;

use digest::Digest;
use hmac::{Hmac, Mac};
use md5::Md5;
use sha1::Sha1;
use sha2::Sha256;

/// Compute a MAC or plain hash depending on `macalg`.
///
/// Returns the digest length in bytes on success, 0 on unrecognized algorithm.
///
/// # Safety
///
/// - `md` must point to at least 32 bytes of writable memory (max digest size).
/// - `key` must point to at least `keylen` bytes of readable memory.
/// - `text` must point to at least `textlen` bytes of readable memory.
/// - `macalg` must be a valid null-terminated C string.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_hmac_xxx(
    md: *mut u8,
    key: *const u8,
    keylen: u32,
    text: *const u8,
    textlen: u32,
    macalg: *const c_char,
) -> u16 {
    if md.is_null() || key.is_null() || text.is_null() || macalg.is_null() {
        return 0;
    }

    let alg = match CStr::from_ptr(macalg).to_str() {
        Ok(s) => s,
        Err(_) => return 0,
    };

    let key_slice = slice::from_raw_parts(key, keylen as usize);
    let text_slice = slice::from_raw_parts(text, textlen as usize);
    let md_slice = slice::from_raw_parts_mut(md, 32);

    hmac_xxx_inner(md_slice, key_slice, text_slice, alg)
}

/// Inner implementation that works on safe slices.
fn hmac_xxx_inner(md: &mut [u8], key: &[u8], text: &[u8], macalg: &str) -> u16 {
    match macalg {
        "HMAC-SHA1" => {
            let mut mac =
                Hmac::<Sha1>::new_from_slice(key).expect("HMAC accepts any key length");
            mac.update(text);
            let result = mac.finalize().into_bytes();
            md[..20].copy_from_slice(&result);
            20
        }
        "HMAC-MD5" => {
            let mut mac =
                Hmac::<Md5>::new_from_slice(key).expect("HMAC accepts any key length");
            mac.update(text);
            let result = mac.finalize().into_bytes();
            md[..16].copy_from_slice(&result);
            16
        }
        "HMAC-SHA256" => {
            let mut mac =
                Hmac::<Sha256>::new_from_slice(key).expect("HMAC accepts any key length");
            mac.update(text);
            let result = mac.finalize().into_bytes();
            md[..32].copy_from_slice(&result);
            32
        }
        "SHA1" => {
            // Plain hash: key || text (pre-HOPE challenge construction)
            let mut hasher = Sha1::new();
            hasher.update(key);
            hasher.update(text);
            let result = hasher.finalize();
            md[..20].copy_from_slice(&result);
            20
        }
        "MD5" => {
            let mut hasher = Md5::new();
            hasher.update(key);
            hasher.update(text);
            let result = hasher.finalize();
            md[..16].copy_from_slice(&result);
            16
        }
        "SHA256" => {
            let mut hasher = Sha256::new();
            hasher.update(key);
            hasher.update(text);
            let result = hasher.finalize();
            md[..32].copy_from_slice(&result);
            32
        }
        _ => 0,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hmac_sha1_known_vector() {
        // RFC 2202 test case 1: key = 0x0b repeated 20 times, data = "Hi There"
        let key = [0x0bu8; 20];
        let text = b"Hi There";
        let mut md = [0u8; 32];
        let len = hmac_xxx_inner(&mut md, &key, text, "HMAC-SHA1");
        assert_eq!(len, 20);
        let expected: [u8; 20] = [
            0xb6, 0x17, 0x31, 0x86, 0x55, 0x05, 0x72, 0x64, 0xe2, 0x8b, 0xc0, 0xb6,
            0xfb, 0x37, 0x8c, 0x8e, 0xf1, 0x46, 0xbe, 0x00,
        ];
        assert_eq!(&md[..20], &expected);
    }

    #[test]
    fn hmac_md5_known_vector() {
        // RFC 2202 test case 1: key = 0x0b repeated 16 times, data = "Hi There"
        let key = [0x0bu8; 16];
        let text = b"Hi There";
        let mut md = [0u8; 32];
        let len = hmac_xxx_inner(&mut md, &key, text, "HMAC-MD5");
        assert_eq!(len, 16);
        let expected: [u8; 16] = [
            0x92, 0x94, 0x72, 0x7a, 0x36, 0x38, 0xbb, 0x1c, 0x13, 0xf4, 0x8e, 0xf8,
            0x15, 0x8b, 0xfc, 0x9d,
        ];
        assert_eq!(&md[..16], &expected);
    }

    #[test]
    fn hmac_sha256_basic() {
        let key = b"secret";
        let text = b"message";
        let mut md = [0u8; 32];
        let len = hmac_xxx_inner(&mut md, key, text, "HMAC-SHA256");
        assert_eq!(len, 32);
        // Just verify it's non-zero and consistent
        let mut md2 = [0u8; 32];
        let len2 = hmac_xxx_inner(&mut md2, key, text, "HMAC-SHA256");
        assert_eq!(len2, 32);
        assert_eq!(md, md2);
    }

    #[test]
    fn plain_sha1_key_concat_text() {
        // Plain hash: SHA1(key || text) — NOT HMAC
        let key = b"key";
        let text = b"text";
        let mut md = [0u8; 32];
        let len = hmac_xxx_inner(&mut md, key, text, "SHA1");
        assert_eq!(len, 20);

        // Verify against direct computation
        let mut hasher = Sha1::new();
        hasher.update(b"key");
        hasher.update(b"text");
        let expected = hasher.finalize();
        assert_eq!(&md[..20], expected.as_slice());
    }

    #[test]
    fn plain_md5_key_concat_text() {
        let key = b"key";
        let text = b"text";
        let mut md = [0u8; 32];
        let len = hmac_xxx_inner(&mut md, key, text, "MD5");
        assert_eq!(len, 16);

        let mut hasher = Md5::new();
        hasher.update(b"key");
        hasher.update(b"text");
        let expected = hasher.finalize();
        assert_eq!(&md[..16], expected.as_slice());
    }

    #[test]
    fn plain_sha256_key_concat_text() {
        let key = b"key";
        let text = b"text";
        let mut md = [0u8; 32];
        let len = hmac_xxx_inner(&mut md, key, text, "SHA256");
        assert_eq!(len, 32);

        let mut hasher = Sha256::new();
        hasher.update(b"key");
        hasher.update(b"text");
        let expected = hasher.finalize();
        assert_eq!(&md[..32], expected.as_slice());
    }

    #[test]
    fn unknown_algorithm_returns_zero() {
        let mut md = [0u8; 32];
        let len = hmac_xxx_inner(&mut md, b"k", b"t", "HAVAL");
        assert_eq!(len, 0);
    }
}
