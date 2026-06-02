//! In-place byte sanitisation matching the C macros/inlines the receive
//! handlers apply to chat / message / task-error text before it reaches the
//! UI.
//!
//! Two transforms, ported byte-for-byte from `src/compat.h` (`X2X` /
//! `CR2LF`) and `src/protocol.h` (`strip_ansi`):
//!
//! - [`cr2lf`] — the Hotline wire uses CR (`\r`) line endings; we use LF
//!   (`\n`) internally. Replace every `\r` with `\n`.
//! - [`strip_ansi`] — fold a band of low control bytes up into the
//!   printable range so stray terminal-control characters don't reach
//!   Pango. Faithful to the original signed-`char` comparison: only bytes
//!   14..=30 (excluding 15 and 22) are affected; bytes >= 0x80 are negative
//!   as `char` and therefore untouched.

/// Replace every `\r` (0x0D) with `\n` (0x0A) in place. Mirrors
/// `CR2LF` / `X2X` in `src/compat.h`.
pub fn cr2lf(buf: &mut [u8]) {
    for b in buf.iter_mut() {
        if *b == b'\r' {
            *b = b'\n';
        }
    }
}

/// Fold low control bytes into the printable range, in place. Mirrors
/// `strip_ansi` in `src/protocol.h`:
///
/// ```c
/// if (*p < 31 && *p > 13 && *p != 15 && *p != 22)
///     *p = (*p & 127) | 64;
/// ```
///
/// `*p` is a (signed) `char`, so the `> 13` test excludes every byte with
/// the high bit set — only 14..=30 minus {15, 22} qualify.
pub fn strip_ansi(buf: &mut [u8]) {
    for b in buf.iter_mut() {
        let c = *b as i8;
        if c < 31 && c > 13 && *b != 15 && *b != 22 {
            *b = (*b & 0x7f) | 0x40;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cr2lf_swaps_only_cr() {
        let mut v = b"a\rb\r\nc".to_vec();
        cr2lf(&mut v);
        assert_eq!(v, b"a\nb\n\nc");
    }

    #[test]
    fn strip_ansi_folds_control_band() {
        // 14 -> 14|64 = 'N'(78); 30 -> 30|64 = '^'(94).
        let mut v = vec![14u8, 30u8];
        strip_ansi(&mut v);
        assert_eq!(v, vec![78u8, 94u8]);
    }

    #[test]
    fn strip_ansi_preserves_boundaries_and_exceptions() {
        // 13 (CR) and 31 are outside the band; 15 and 22 are explicit
        // exceptions; printable ASCII and high bytes are untouched.
        let mut v = vec![13u8, 31u8, 15u8, 22u8, b'A', 0x80, 0xFF, b'\n', b'\t'];
        let expect = v.clone();
        strip_ansi(&mut v);
        assert_eq!(v, expect);
    }
}
