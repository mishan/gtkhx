//! Blowfish in 64-bit Output Feedback (OFB) mode.
//!
//! Uses the RustCrypto `blowfish` crate for the block cipher primitive
//! and implements the OFB-64 feedback loop manually. This is necessary
//! because the generic `ofb` crate requires fixed key sizes at compile
//! time, but the Hotline protocol uses variable-length keys (16-32 bytes
//! from HMAC outputs).
//!
//! Byte-for-byte compatible with OpenSSL's BF_ofb64_encrypt and the
//! pre-port Nettle-based implementation in cipher.c. OFB is symmetric:
//! encrypt == decrypt. State carried across calls is the 8-byte ivec
//! (current keystream block) plus the byte index `num` into that block
//! (0..7). When `num` rolls to zero we re-encrypt ivec in place to
//! produce the next keystream block.

use blowfish::Blowfish;
use cipher::{Array, BlockCipherEncrypt, KeyInit};

/// Blowfish OFB-64 block size: 8 bytes.
pub const BLOWFISH_OFB64_BLOCK_SIZE: usize = 8;

/// Blowfish in 64-bit OFB mode. Carries state across calls.
#[derive(Clone)]
pub struct BlowfishOfb64State {
    cipher: Blowfish,
    ivec: [u8; BLOWFISH_OFB64_BLOCK_SIZE],
    num: usize,
}

impl BlowfishOfb64State {
    /// Create a new Blowfish OFB-64 state with the given key.
    /// The ivec starts at all zeros and num at 0 (block boundary).
    ///
    /// Returns `None` if the key length is outside the Blowfish-accepted
    /// range (1..=56 bytes). HOPE-derived keys are always 16/20/32 bytes
    /// so this never trips on a well-formed wire, but a malicious or
    /// malfunctioning server could send something unexpected. The FFI
    /// wrapper translates `None` into a NULL return so the C dispatcher
    /// fails closed instead of aborting the whole client.
    pub fn new(key: &[u8]) -> Option<Self> {
        let cipher = Blowfish::new_from_slice(key).ok()?;
        Some(BlowfishOfb64State {
            cipher,
            ivec: [0u8; BLOWFISH_OFB64_BLOCK_SIZE],
            num: 0,
        })
    }

    /// Re-key without resetting the OFB ivec/num.
    ///
    /// This matches the wire protocol's rekey behavior — the legacy
    /// HOPE per-message rekey trick stamps a 1..63 iteration count
    /// into the type field's high byte and rotates the cipher key by
    /// that many HMAC iterations on both sides. The contract is
    /// "rotate the key schedule, KEEP the OFB ivec/num where they
    /// are" — mhxd's cipher_encode_init does the same (just BF_set_key,
    /// no memset). An early port revision added a defensive memset
    /// here that desynced the cipher state every rekey; the new Tier 3
    /// test_hope_blowfish caught it against live mhxd.
    ///
    /// Returns `false` on invalid key length (see `new` for the
    /// rationale); on `false` the existing key schedule is left in
    /// place so any subsequent crypt() call still produces deterministic
    /// (if wrong) bytes rather than dereferencing past stale state.
    pub fn set_key(&mut self, key: &[u8]) -> bool {
        match Blowfish::new_from_slice(key) {
            Ok(c) => {
                self.cipher = c;
                true
            }
            Err(_) => false,
        }
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

    /// In-place encrypt/decrypt (symmetric). Same loop as
    /// [`Self::crypt`] but operates on a single buffer — useful
    /// for streaming consumers that don't want a scratch
    /// allocation per call. The R3.3.c `BlowfishStream` adapter
    /// in `hxnet` calls this on every `poll_read` / `poll_write`
    /// directly against the inner stream's buffer, avoiding a
    /// per-poll memcpy.
    pub fn crypt_in_place(&mut self, buf: &mut [u8]) {
        let mut n = self.num;
        for d in buf.iter_mut() {
            if n == 0 {
                #[allow(deprecated)]
                let block = Array::from_mut_slice(&mut self.ivec);
                self.cipher.encrypt_block(block);
            }
            *d ^= self.ivec[n];
            n = (n + 1) & 7;
        }
        self.num = n;
    }

    /// Snapshot the OFB feedback state — ivec + num — into `out_ivec` /
    /// `out_num`. Used by the receive-side rollback path in
    /// network_decode.c, which has to undo a speculative cipher_decode
    /// when the downstream compressor consumes fewer bytes than the
    /// cipher decoded.
    ///
    /// 9 bytes total of state (8-byte ivec + 1-byte num). The previous
    /// implementation cloned the whole state — including the full
    /// Blowfish key schedule (~4 KiB of P/S boxes) — once per
    /// cipher_decode call. That's an allocation per Hotline transaction
    /// on the hot path. Snapshotting just ivec/num is what OpenSSL's
    /// BF_KEY/ivec/num pattern was — the key schedule doesn't change
    /// during a single rollback, so saving + restoring it is wasted
    /// work.
    pub fn save_ofb_state(
        &self,
        out_ivec: &mut [u8; BLOWFISH_OFB64_BLOCK_SIZE],
        out_num: &mut u32,
    ) {
        *out_ivec = self.ivec;
        *out_num = self.num as u32;
    }

    /// Restore the OFB feedback state from a prior save_ofb_state.
    /// Pairs with save_ofb_state on the rollback path; see that
    /// function for the rationale. `num` is masked to the
    /// 0..=BLOCK_SIZE-1 range so a bad value coming through the FFI
    /// can't index ivec out of bounds and panic.
    pub fn restore_ofb_state(&mut self, ivec: &[u8; BLOWFISH_OFB64_BLOCK_SIZE], num: u32) {
        self.ivec = *ivec;
        self.num = (num as usize) & (BLOWFISH_OFB64_BLOCK_SIZE - 1);
    }
}
