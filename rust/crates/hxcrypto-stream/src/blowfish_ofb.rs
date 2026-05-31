//! Blowfish in 64-bit Output Feedback (OFB) mode.
//!
//! Uses the RustCrypto `blowfish` crate for the block cipher primitive
//! and implements the OFB-64 feedback loop manually. This is necessary
//! because the generic `ofb` crate requires fixed key sizes at compile
//! time, but the Hotline protocol uses variable-length keys (16-32 bytes
//! from HMAC outputs).
//!
//! Byte-for-byte compatible with OpenSSL's BF_ofb64_encrypt and the
//! Nettle-based implementation in the old cipher.c.
//!
//! OFB is symmetric: encrypt == decrypt. State carried across calls is
//! the 8-byte ivec (current keystream block) plus the byte index `num`
//! into that block (0..7). When `num` rolls to zero we re-encrypt ivec
//! in place to produce the next keystream block.

use blowfish::Blowfish;
use cipher::{Array, BlockCipherEncrypt, KeyInit};

/// Blowfish in 64-bit OFB mode. Carries state across calls.
#[derive(Clone)]
pub struct BlowfishOfb64State {
    cipher: Blowfish,
    ivec: [u8; 8],
    num: usize,
}

impl BlowfishOfb64State {
    /// Create a new Blowfish OFB-64 state with the given key.
    /// The ivec starts at all zeros and num at 0 (block boundary).
    ///
    /// # Panics
    /// Panics if `key` is empty or longer than 56 bytes (Blowfish limit).
    pub fn new(key: &[u8]) -> Self {
        BlowfishOfb64State {
            cipher: Blowfish::new_from_slice(key).expect("valid blowfish key length (1-56 bytes)"),
            ivec: [0u8; 8],
            num: 0,
        }
    }

    /// Re-key without resetting the OFB ivec/num.
    /// This matches the wire protocol's rekey behavior: only the key
    /// schedule changes, the OFB feedback state continues from where
    /// it was.
    ///
    /// # Panics
    /// Panics if `key` is empty or longer than 56 bytes.
    pub fn set_key(&mut self, key: &[u8]) {
        self.cipher = Blowfish::new_from_slice(key).expect("valid blowfish key length (1-56 bytes)");
    }

    /// Encrypt/decrypt using OFB-64 (symmetric operation).
    /// Safe for in-place operation.
    pub fn crypt(&mut self, src: &[u8], dst: &mut [u8]) {
        debug_assert_eq!(src.len(), dst.len());
        let mut n = self.num;
        for (s, d) in src.iter().zip(dst.iter_mut()) {
            if n == 0 {
                #[allow(deprecated)]
                let block = Array::from_mut_slice(&mut self.ivec);
                self.cipher.encrypt_block(block);
            }
            *d = *s ^ self.ivec[n];
            n = (n + 1) & 7;
        }
        self.num = n;
    }
}
