//! C-ABI exports for the C-side `src/inline_media_decode.c` shim.
//!
//! The C side hand-declares `extern` blocks matching the prototypes
//! exposed here — the link step catches any drift as undefined
//! symbols, same discipline as the Phase R1 crypto crates.
//!
//! G.1 (this commit) exports just the sniff layer functions. G.2
//! adds `hx_image_decode_async` + cancel-token; G.3 adds the
//! animation-frame-iterator surface.

use std::ffi::{c_char, c_void, CStr};
use std::rc::Rc;
use std::slice;

pub use crate::caps::{HxInlineMediaCaps};
use crate::decode::{decode_async, DecodeCallback, DecodePolicy, DecodeToken};
use crate::ffi_result::decoded_drop;
/// Re-export so integration tests and other Rust consumers can
/// name the decoded-result type without touching the internal
/// `ffi_result` module path.
pub use crate::ffi_result::HxInlineMediaDecoded;
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

/// ---- Async decode (G.2) ----------------------------------------
///
/// C signatures (mirror `src/inline_media_decode.h`):
/// ```c
/// extern gpointer inline_media_decode_async(
///     const guint8 *bytes, gsize len,
///     const HxInlineMediaCaps *caps,
///     HxInlineMediaDecodeCallback cb, gpointer user_data);
/// extern void inline_media_decode_cancel(gpointer token);
/// extern void inline_media_decoded_free(HxInlineMediaDecoded *r);
/// ```

/// Wire values for `HxImageDecodePolicy`. Discriminants pinned
/// `_Static_assert`-style on the C side so a reordering on
/// either end trips the linker / compile rather than silently
/// flipping the sniff allowlist behaviour.
/// Wire value pinned to `HX_IMAGE_DECODE_STRICT` on the C side.
/// Kept here even though `u32_to_policy` collapses everything
/// other than `WIDE` to it — name + value match make the FFI
/// constant table searchable in both directions.
#[allow(dead_code)]
const POLICY_STRICT: u32 = 0;
const POLICY_WIDE: u32 = 1;

#[inline]
fn u32_to_policy(v: u32) -> DecodePolicy {
    match v {
        POLICY_WIDE => DecodePolicy::Wide,
        // Default + STRICT fall through here. Treating an
        // unknown discriminant as STRICT is the conservative
        // failure mode — a buggy caller gets the tighter
        // allowlist, not the looser one.
        _ => DecodePolicy::Strict,
    }
}

#[inline]
unsafe fn decode_async_common(
    bytes: *const u8,
    len: usize,
    caps_in: *const HxInlineMediaCaps,
    policy: DecodePolicy,
    cb: DecodeCallback,
    user_data: *mut c_void,
) -> *mut c_void {
    // Decode-pipeline reads aren't bounded to 32 bytes the way
    // sniff is — glycin needs the whole payload. The slice
    // contract is therefore the standard FFI one: caller's
    // bytes pointer + len must be a real, initialised buffer.
    // We still defend against NULL / zero-length up front; the
    // pipeline produces a synchronous reject for empty input
    // and the callback fires once before returning.
    let slice: &[u8] = if bytes.is_null() || len == 0 {
        &[]
    } else {
        // Defensive cap. `slice::from_raw_parts` UB-requires
        // len ≤ isize::MAX per the safety contract. The
        // inline-media byte cap is 256 KiB by default; clamping
        // here at isize::MAX is a sanity floor against a buggy
        // caller, not a policy decision.
        let safe_len = len.min(isize::MAX as usize);
        slice::from_raw_parts(bytes, safe_len)
    };

    // Caller may pass NULL caps — fall back to spec defaults.
    let caps = if caps_in.is_null() {
        HxInlineMediaCaps::SPEC
    } else {
        (*caps_in).with_defaults()
    };

    match decode_async(slice, caps, policy, cb, user_data) {
        Some(token) => Rc::into_raw(token) as *mut c_void,
        None => std::ptr::null_mut(),
    }
}

/// Strict inline-media decode entry. Sniff gate enforces the
/// fogWraith inline-media spec allowlist (JPEG / PNG / GIF);
/// other formats fail at sniff before glycin spawns.
#[no_mangle]
pub unsafe extern "C" fn inline_media_decode_async(
    bytes: *const u8,
    len: usize,
    caps_in: *const HxInlineMediaCaps,
    cb: DecodeCallback,
    user_data: *mut c_void,
) -> *mut c_void {
    decode_async_common(bytes, len, caps_in, DecodePolicy::Strict, cb, user_data)
}

/// Generic decode entry with explicit format policy. Same
/// contract as `inline_media_decode_async` modulo the gate:
/// `policy == 0` (STRICT) is identical to the inline-media
/// entry; `policy == 1` (WIDE) skips the sniff allowlist and
/// hands anything non-empty to glycin. Used by the file-preview
/// path where the user explicitly opened a BMP / TIFF / WebP /
/// HEIC / etc. — formats the inline-media spec forbids but
/// glycin's bundled loader set knows how to handle.
///
/// Cancel + free contract is the same as
/// `inline_media_decode_async`; reuse the existing
/// `inline_media_decode_cancel` / `inline_media_decoded_free`
/// helpers regardless of which entry built the token.
#[no_mangle]
pub unsafe extern "C" fn hx_image_decode_async_with_policy(
    bytes: *const u8,
    len: usize,
    caps_in: *const HxInlineMediaCaps,
    policy: u32,
    cb: DecodeCallback,
    user_data: *mut c_void,
) -> *mut c_void {
    decode_async_common(bytes, len, caps_in, u32_to_policy(policy), cb, user_data)
}

#[no_mangle]
pub unsafe extern "C" fn inline_media_decode_cancel(token: *mut c_void) {
    if token.is_null() {
        return;
    }
    // Resurrect the Rc the FFI handed out — drop it, decrementing
    // the strong count. The async block holds its own clone, so
    // the underlying DecodeToken sticks around until the future
    // completes; the future polls the `cancelled` cell on its
    // way to the callback and bails if set.
    let rc = Rc::from_raw(token as *const DecodeToken);
    rc.cancelled.set(true);
    drop(rc);
}

#[no_mangle]
pub unsafe extern "C" fn inline_media_decoded_free(
    result: *mut HxInlineMediaDecoded,
) {
    decoded_drop(result);
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
    fn u32_to_policy_maps_known_discriminants() {
        // Pin the discriminants so a renumber on either side
        // surfaces here rather than at runtime.
        assert_eq!(POLICY_STRICT, 0);
        assert_eq!(POLICY_WIDE, 1);
        assert_eq!(u32_to_policy(POLICY_STRICT), DecodePolicy::Strict);
        assert_eq!(u32_to_policy(POLICY_WIDE), DecodePolicy::Wide);
    }

    #[test]
    fn u32_to_policy_unknown_collapses_to_strict() {
        // Conservative failure mode: a buggy caller passing
        // a stale / future / garbage discriminant gets the
        // tighter allowlist, not the looser one. Cover both
        // a couple of representative wild values and the
        // u32::MAX edge so a regression of the wildcard arm
        // (e.g. someone replacing `_` with `2..=u32::MAX`)
        // doesn't slip past.
        assert_eq!(u32_to_policy(2), DecodePolicy::Strict);
        assert_eq!(u32_to_policy(42), DecodePolicy::Strict);
        assert_eq!(u32_to_policy(u32::MAX), DecodePolicy::Strict);
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
