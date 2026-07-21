//! Stable bookmark cipher-byte vocabulary (port of the C
//! `src/bookmark_cipher.c`).
//!
//! A bookmark stores its cipher selection as a single stable byte, decoupled
//! from the Connect dialog's dropdown order so reordering the UI never shifts
//! a saved bookmark's meaning. `0` = "no cipher"; `1+` names a specific HOPE
//! cipher. The byte values are part of the on-disk format (both the new TOML
//! and the legacy HTsc) — don't change them; retired ciphers keep their slot
//! (e.g. RC4 stays named so a legacy RC4 bookmark can be detected and
//! migrated), and new ciphers get new bytes at the tail.

/// No cipher selected (HOPE may still be on without a stream/AEAD cipher).
pub const NONE: u8 = 0;
/// Legacy RC4 — never offered; detected on load to trigger the migration
/// prompt.
pub const RC4: u8 = 1;
pub const BLOWFISH: u8 = 2;
pub const CHACHA20_POLY1305: u8 = 3;

/// Table indexed by stable byte. Index 0 = "no cipher" (`None`).
const TABLE: &[Option<&str>] = &[
    /* 0 */ None,
    /* 1 */ Some("RC4"),
    /* 2 */ Some("BLOWFISH"),
    /* 3 */ Some("CHACHA20-POLY1305"),
];

/// Resolve a stable byte to a HOPE cipher-name (`"BLOWFISH"`,
/// `"CHACHA20-POLY1305"`, or `"RC4"` for the legacy slot). `None` for byte 0
/// and for any byte outside the table (corrupt / forward-compat file).
pub fn name(byte: u8) -> Option<&'static str> {
    TABLE.get(byte as usize).copied().flatten()
}

/// Inverse: map a HOPE cipher-name to its stable byte. Returns [`NONE`] for
/// an empty / unknown name.
pub fn byte_from_name(name: &str) -> u8 {
    if name.is_empty() {
        return NONE;
    }
    TABLE
        .iter()
        .enumerate()
        .skip(1)
        .find_map(|(i, entry)| (*entry == Some(name)).then_some(i as u8))
        .unwrap_or(NONE)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn known_bytes() {
        assert_eq!(name(NONE), None);
        assert_eq!(name(RC4), Some("RC4"));
        assert_eq!(name(BLOWFISH), Some("BLOWFISH"));
        assert_eq!(name(CHACHA20_POLY1305), Some("CHACHA20-POLY1305"));
    }

    #[test]
    fn out_of_range_is_none() {
        assert_eq!(name(9), None);
        assert_eq!(name(255), None);
    }

    #[test]
    fn names_round_trip_to_bytes() {
        assert_eq!(byte_from_name("BLOWFISH"), BLOWFISH);
        assert_eq!(byte_from_name("CHACHA20-POLY1305"), CHACHA20_POLY1305);
        assert_eq!(byte_from_name("RC4"), RC4);
        assert_eq!(byte_from_name(""), NONE);
        assert_eq!(byte_from_name("nope"), NONE);
    }
}
