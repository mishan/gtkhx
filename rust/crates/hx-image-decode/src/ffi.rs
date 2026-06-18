//! C-ABI exports for the C-side `src/inline_media_decode.c` shim.
//!
//! The C side hand-declares `extern` blocks matching the prototypes
//! exposed here — the link step catches any drift as undefined
//! symbols, same discipline as the Phase R1 crypto crates.
//!
//! G.1 (this commit) exports just the sniff layer functions. G.2
//! adds `hx_image_decode_async` + cancel-token; G.3 adds the
//! animation-frame-iterator surface.

use std::ffi::{c_char, CStr};
use std::slice;

use crate::sniff::{format_is_allowed, sniff, Format};

/// Window the sniff layer ever reads. Mirrors the 32-byte bound
/// documented in `sniff` — every magic signature in the allowlist
/// + blocklist fits in the first 12 bytes, the SVG check scans
/// past leading whitespace but is bounded by `sniff` itself, and
/// the FFI shim clamps before constructing the slice so
/// `slice::from_raw_parts` only needs the *clamped* prefix to be
/// valid, not the caller's claimed `len`.
const SNIFF_WINDOW_BYTES: usize = 32;

/// Borrow a `(*const u8, usize)` pair as a `&[u8]` covering at
/// most the first [`SNIFF_WINDOW_BYTES`] bytes of the input.
/// Returns an empty slice on NULL pointer or zero length — the
/// sniff layer then returns `Unknown` cleanly without panicking.
///
/// # Safety
///
/// The caller must ensure the first
/// `min(len, SNIFF_WINDOW_BYTES)` bytes starting at `bytes` are
/// initialised and remain valid for the duration of the call.
/// Bytes beyond the clamp are **not** touched — passing a `len`
/// larger than the underlying allocation is safe as long as the
/// first 32 bytes are real, which matches the only contract every
/// caller of `inline_media_decode` already honours (the bytes
/// come from `gdk_pixbuf_loader_write` / `g_bytes_get_data` /
/// the chunked-download accumulator, all of which validate the
/// full slice before handing it here).
#[inline]
unsafe fn slice_from_raw<'a>(bytes: *const u8, len: usize) -> &'a [u8] {
    if bytes.is_null() || len == 0 {
        return &[];
    }
    let clamped = len.min(SNIFF_WINDOW_BYTES);
    slice::from_raw_parts(bytes, clamped)
}

/// C-ABI mirror of [`Format`]. The discriminants match the legacy
/// C `HxInlineMediaFormat` enum byte-for-byte.
///
/// We don't re-export the Rust enum directly across the FFI; the
/// `_Static_assert(sizeof(HxInlineMediaFormat) == 4, ...)` on the
/// C side (in inline_media_decode.c) pins the enum width to match
/// `#[repr(u32)]`, and casting through `u32` keeps the surface
/// explicit.
#[inline]
fn format_to_u32(f: Format) -> u32 {
    f as u32
}

/// Magic-byte sniff. C signature:
/// ```c
/// extern guint32 hx_image_decode_sniff(const guint8 *bytes, gsize len);
/// ```
/// Returns a discriminant matching `HxInlineMediaFormat`.
#[no_mangle]
pub unsafe extern "C" fn hx_image_decode_sniff(
    bytes: *const u8,
    len: usize,
) -> u32 {
    let buf = slice_from_raw(bytes, len);
    format_to_u32(sniff(buf))
}

/// True iff the format is on the inline-media allowlist (JPEG /
/// PNG / GIF). C signature:
/// ```c
/// extern gboolean hx_image_decode_format_is_allowed(guint32 fmt);
/// ```
///
/// Unknown discriminants map to `Format::Unknown` (rejected) — a
/// caller passing garbage gets `FALSE`, not a panic.
#[no_mangle]
pub unsafe extern "C" fn hx_image_decode_format_is_allowed(fmt: u32) -> i32 {
    let f = u32_to_format(fmt);
    if format_is_allowed(f) {
        1
    } else {
        0
    }
}

/// Canonical MIME for the format. C signature:
/// ```c
/// extern const char *hx_image_decode_format_to_mime(guint32 fmt);
/// ```
/// Returns a borrowed pointer to a static NUL-terminated literal —
/// caller doesn't free. `NULL` for unknown / unrecognised
/// discriminants. The pointer is valid for the lifetime of the
/// process.
#[no_mangle]
pub unsafe extern "C" fn hx_image_decode_format_to_mime(
    fmt: u32,
) -> *const c_char {
    static_mime_for_format(u32_to_format(fmt)).unwrap_or(std::ptr::null())
}

/// Backing table of NUL-terminated MIME strings. Each variant
/// returns a pointer into a `b"...\0"` static; the C side reads
/// up to NUL and never frees.
fn static_mime_for_format(f: Format) -> Option<*const c_char> {
    let s: &[u8] = match f {
        Format::Jpeg => b"image/jpeg\0",
        Format::Png => b"image/png\0",
        Format::Gif => b"image/gif\0",
        Format::Svg => b"image/svg+xml\0",
        Format::Webp => b"image/webp\0",
        Format::Avif => b"image/avif\0",
        Format::Heic => b"image/heic\0",
        Format::Tiff => b"image/tiff\0",
        Format::Ico => b"image/x-icon\0",
        Format::Bmp => b"image/bmp\0",
        Format::Unknown => return None,
    };
    // SAFETY: each literal above is NUL-terminated by
    // construction. CStr::from_bytes_with_nul gives us the
    // sentinel + verification in one step; pointer remains valid
    // for 'static.
    let cstr = CStr::from_bytes_with_nul(s).ok()?;
    Some(cstr.as_ptr())
}

/// Reverse map from u32 discriminant to typed [`Format`]. Anything
/// out of the documented range collapses to `Unknown` —
/// defence-in-depth against a C caller passing a wild value.
fn u32_to_format(v: u32) -> Format {
    match v {
        1 => Format::Jpeg,
        2 => Format::Png,
        3 => Format::Gif,
        4 => Format::Svg,
        5 => Format::Webp,
        6 => Format::Avif,
        7 => Format::Heic,
        8 => Format::Tiff,
        9 => Format::Ico,
        10 => Format::Bmp,
        _ => Format::Unknown,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CStr;

    #[test]
    fn sniff_ffi_jpeg() {
        let jpg = [0xFFu8, 0xD8, 0xFF];
        let got = unsafe { hx_image_decode_sniff(jpg.as_ptr(), jpg.len()) };
        assert_eq!(got, Format::Jpeg as u32);
    }

    #[test]
    fn sniff_ffi_null_safe() {
        let got = unsafe { hx_image_decode_sniff(std::ptr::null(), 0) };
        assert_eq!(got, Format::Unknown as u32);
        // Zero-length with non-null pointer: still safe.
        let buf = [0u8; 1];
        let got = unsafe { hx_image_decode_sniff(buf.as_ptr(), 0) };
        assert_eq!(got, Format::Unknown as u32);
    }

    #[test]
    fn format_is_allowed_ffi() {
        assert_eq!(
            unsafe { hx_image_decode_format_is_allowed(Format::Jpeg as u32) },
            1
        );
        assert_eq!(
            unsafe { hx_image_decode_format_is_allowed(Format::Svg as u32) },
            0
        );
        assert_eq!(unsafe { hx_image_decode_format_is_allowed(99) }, 0);
    }

    #[test]
    fn format_to_mime_ffi() {
        for (f, expected) in [
            (Format::Jpeg, "image/jpeg"),
            (Format::Png, "image/png"),
            (Format::Gif, "image/gif"),
            (Format::Svg, "image/svg+xml"),
            (Format::Webp, "image/webp"),
        ] {
            let p = unsafe { hx_image_decode_format_to_mime(f as u32) };
            assert!(!p.is_null());
            let s = unsafe { CStr::from_ptr(p) }.to_str().unwrap();
            assert_eq!(s, expected);
        }
        let p = unsafe { hx_image_decode_format_to_mime(Format::Unknown as u32) };
        assert!(p.is_null());
    }
}
