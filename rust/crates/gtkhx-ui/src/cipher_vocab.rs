//! Cipher / compression picker vocabulary (ported from the old
//! `src/cipher_vocab.c`, itself carved out of `connect.c` in R5.3).
//!
//! [`VALID_CIPHERS`] / [`VALID_COMPRESSORS`] are the UI dropdown models
//! shared by the Connect dialog ([`crate::connect`]) and the Bookmarks
//! dialog ([`crate::bookmarks`]). The first entry is the default; the
//! combos prepend a synthetic "none" row at index 0.
//!
//! The on-disk cipher byte uses a SEPARATE stable vocabulary
//! (`bookmark_cipher.c`, still C — driven by Tier 1 tests), so reordering
//! or shortening these arrays never shifts the meaning of a saved bookmark
//! byte. The two translators here ([`dropdown_to_cipher_byte`] /
//! [`cipher_byte_to_dropdown`]) bridge the UI index and the stable byte by
//! calling into that C module.

use crate::ffi::{bookmark_cipher_byte_from_name, bookmark_cipher_name, BOOKMARK_CIPHER_BYTE_NONE};
use crate::{cs, cstr};

/// HOPE stream / AEAD ciphers this build offers, strongest-preference
/// first. RC4 was retired in `claude/remove-rc4` (known-broken stream
/// cipher shipped under a "Secure" label); its stable byte stays reserved
/// in `bookmark_cipher.h` but it is never offered here. CHACHA20-POLY1305
/// is the modern AEAD choice; BLOWFISH is the minimum acceptable bar.
pub const VALID_CIPHERS: &[&str] = &["BLOWFISH", "CHACHA20-POLY1305"];

/// Compression algorithms offered, default-first: ZSTD (best ratio per the
/// HOPE-Secure-Login spec), LZ4 (fastest), GZIP (universally supported,
/// the legacy default).
pub const VALID_COMPRESSORS: &[&str] = &["ZSTD", "LZ4", "GZIP"];

/// 1 if `name` is a HOPE cipher this build offers.
pub fn valid_cipher(name: &str) -> bool {
    VALID_CIPHERS.contains(&name)
}

/// 1 if `name` is a compression algorithm this build offers.
pub fn valid_compress(name: &str) -> bool {
    VALID_COMPRESSORS.contains(&name)
}

/// The compressor at 0-based index `i` into [`VALID_COMPRESSORS`], or None
/// if out of range. Callers translating a dropdown index subtract the 1
/// for the synthetic "none" row first.
pub fn compress_name(i: usize) -> Option<&'static str> {
    VALID_COMPRESSORS.get(i).copied()
}

/// Translate the dropdown's cipher index (0 = no cipher, 1..=N indexes
/// `VALID_CIPHERS[N-1]`) to a stable on-disk bookmark byte. Save paths use
/// this so the byte's meaning stays stable across dropdown reorderings.
pub fn dropdown_to_cipher_byte(dropdown_idx: u32) -> u8 {
    if dropdown_idx == 0 || dropdown_idx as usize > VALID_CIPHERS.len() {
        return BOOKMARK_CIPHER_BYTE_NONE;
    }
    let name = VALID_CIPHERS[(dropdown_idx - 1) as usize];
    unsafe { bookmark_cipher_byte_from_name(cs(name).as_ptr()) }
}

/// Inverse of [`dropdown_to_cipher_byte`]: a stable bookmark cipher byte
/// back to the matching dropdown index, or 0 ("no cipher") if the byte
/// names a cipher the dropdown no longer offers (e.g. RC4).
pub fn cipher_byte_to_dropdown(byte: u8) -> u32 {
    if byte == BOOKMARK_CIPHER_BYTE_NONE {
        return 0;
    }
    let name_ptr = unsafe { bookmark_cipher_name(byte) };
    if name_ptr.is_null() {
        return 0;
    }
    let name = unsafe { cstr(name_ptr) };
    VALID_CIPHERS
        .iter()
        .position(|c| *c == name)
        .map(|i| (i + 1) as u32)
        .unwrap_or(0)
}
