/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * Bounded image decoder for the inline-media extension (Phase 9.B).
 *
 * Layered defence per docs/inline-media-plan.md:
 *
 *   1. inline_media_sniff — magic-byte sniff. Rejects anything
 *      outside the JPEG / PNG / GIF allowlist before a single
 *      decoder byte runs. Specifically recognises (and rejects)
 *      SVG, WebP, AVIF, HEIC, TIFF, ICO, BMP — same blocklist
 *      the spec calls out, plus the formats most often
 *      misclassified by extension.
 *
 *   2. Pre-decode byte cap. Reject before even constructing a
 *      pixbuf loader.
 *
 *   3. Size-prepared gate. Feed bytes to GdkPixbufLoader; the
 *      "size-prepared" signal fires AFTER the format-specific
 *      header is parsed but BEFORE the pixel data is rendered.
 *      Reject here if width / height / pixels exceed the caps,
 *      stopping the loader before it allocates the decoded pixel
 *      buffer.
 *
 *   4. Loader-error gate. Any error during the full decode
 *      collapses to UnsupportedFormat (code 2).
 *
 * The bytes the server hands us have already been validated +
 * re-encoded server-side per the spec, but the client decoder
 * cannot trust the server (that's exactly what the spec calls
 * out under "Security Considerations / Decoder exploits in
 * clients"). This pipeline runs on every inbound payload.
 */

#include "config.h"
#include <gtk/gtk.h>
#include <string.h>

#include "inline_media_decode.h"
#include "compat.h"  /* PACKED — required before hotline.h */
#include "hotline.h" /* HX_MEDIA_DEFAULT_* fallbacks */
#include "debug.h"

/* ---- Sniff (glycin migration G.1) ---------------------------- */
/* Sniff impl moved to rust/crates/hx-image-decode (sniff.rs +
 * ffi.rs). The C entry points below are thin shims forwarding to
 * the Rust functions; the wire-level contract is byte-identical to
 * the pre-migration C impl, so tests/unit/test_inline_media_decode
 * stays green without changes.
 *
 * Drift surfaces at link time as an undefined symbol — same
 * discipline as the Phase R1 crypto crates. See
 * docs/glycin-migration-plan.md.
 */
extern guint32 hx_image_decode_sniff (const guint8 *bytes, gsize len);
extern gboolean hx_image_decode_format_is_allowed (guint32 fmt);
extern const char *hx_image_decode_format_to_mime (guint32 fmt);

/* Static-assert that the Rust enum discriminants match the C enum
 * one-for-one. The Rust ffi.rs comment pins these as `#[repr(u32)]`;
 * if a future commit reorders either side, the build trips here
 * rather than silently miscategorising at runtime. The sizeof
 * assert covers the corresponding `#[repr(u32)]` width pin — a C
 * toolchain shrinking the enum to a byte (some embedded targets
 * do this; not on our supported platforms but cheap to guard)
 * would otherwise miscast the Rust u32 return at the call sites. */
_Static_assert (sizeof (HxInlineMediaFormat) == 4,
                "rust discriminant pin (sizeof matches #[repr(u32)])");
_Static_assert (INLINE_MEDIA_FORMAT_UNKNOWN == 0,
                "rust discriminant pin (unknown)");
_Static_assert (INLINE_MEDIA_FORMAT_JPEG == 1, "rust discriminant pin (jpeg)");
_Static_assert (INLINE_MEDIA_FORMAT_PNG == 2, "rust discriminant pin (png)");
_Static_assert (INLINE_MEDIA_FORMAT_GIF == 3, "rust discriminant pin (gif)");
_Static_assert (INLINE_MEDIA_FORMAT_SVG == 4, "rust discriminant pin (svg)");
_Static_assert (INLINE_MEDIA_FORMAT_WEBP == 5, "rust discriminant pin (webp)");
_Static_assert (INLINE_MEDIA_FORMAT_AVIF == 6, "rust discriminant pin (avif)");
_Static_assert (INLINE_MEDIA_FORMAT_HEIC == 7, "rust discriminant pin (heic)");
_Static_assert (INLINE_MEDIA_FORMAT_TIFF == 8, "rust discriminant pin (tiff)");
_Static_assert (INLINE_MEDIA_FORMAT_ICO == 9, "rust discriminant pin (ico)");
_Static_assert (INLINE_MEDIA_FORMAT_BMP == 10, "rust discriminant pin (bmp)");

HxInlineMediaFormat
inline_media_sniff (const guint8 *bytes, gsize len)
{
    return (HxInlineMediaFormat)hx_image_decode_sniff (bytes, len);
}

gboolean
inline_media_format_is_allowed (HxInlineMediaFormat f)
{
    return hx_image_decode_format_is_allowed ((guint32)f);
}

const char *
inline_media_format_to_mime (HxInlineMediaFormat f)
{
    return hx_image_decode_format_to_mime ((guint32)f);
}

/* ---- Decode pipeline (glycin migration G.2) ------------------ */
/* Decode impl moved to rust/crates/hx-image-decode/src/decode.rs.
 * The C-side `inline_media_decode_async` /
 * `inline_media_decode_cancel` / `inline_media_decoded_free`
 * symbols are exported by the Rust crate — the prototypes in
 * inline_media_decode.h serve double duty as the FFI extern
 * declarations.
 *
 * The result struct's layout is pinned by both sides:
 *   - Rust: `#[repr(C)]` on HxInlineMediaDecoded plus an
 *     internal const-assert (G.6 adds the offset pins).
 *   - C: the sizeof check below catches a stray pad / field
 *     reorder that would silently corrupt the FFI return.
 *
 * 40 bytes on every supported 64-bit Linux ABI:
 *   8  GdkTexture *texture
 *   8  const char *canonical_mime
 *   4  HxInlineMediaFormat sniffed_format
 *   2  guint16 error_code
 *   2  guint16 _pad0
 *   8  const char *error_message
 *   8  GArray *frames
 *
 * The pointer-width assert below makes the 64-bit-only
 * expectation explicit. A 32-bit build (where pointers are 4
 * bytes and the struct would size to 24) would trip both
 * asserts and bail at compile time — naming the underlying
 * cause keeps the failure mode greppable rather than a
 * generic "40 != 24" mystery for someone retargeting. */
_Static_assert (sizeof (void *) == 8,
                "FFI struct layout pinned on 64-bit ABI; "
                "32-bit builds need a separate sizeof pin");
_Static_assert (sizeof (HxInlineMediaDecoded) == 40,
                "FFI struct size pin (HxInlineMediaDecoded)");

/* Pin the policy enum discriminants both sides. The Rust
 * ffi.rs maps these via u32_to_policy; reordering or shifting
 * either constant would silently flip the sniff allowlist
 * behaviour (an inline-media caller would suddenly accept
 * BMP/TIFF/etc.) without anything else complaining. The
 * sizeof assert pairs with the existing
 * HxInlineMediaFormat width pin — HxImageDecodePolicy crosses
 * the FFI as a u32 on the Rust side; a toolchain that shrinks
 * the C enum to a single byte (some embedded targets, not
 * one of our supported platforms but cheap to guard) would
 * otherwise truncate the value at the call boundary. */
_Static_assert (sizeof (HxImageDecodePolicy) == 4,
                "rust policy width pin (#[repr(u32)] on Rust side)");
_Static_assert (HX_IMAGE_DECODE_STRICT == 0,
                "rust policy discriminant pin (STRICT)");
_Static_assert (HX_IMAGE_DECODE_WIDE == 1,
                "rust policy discriminant pin (WIDE)");

/* Glycin sandbox + decode telemetry bridge. The Rust crate
 * (`rust/crates/hx-image-decode/src/telemetry.rs`) calls into
 * this for every decode-start / decode-done / decode-failed
 * line so the existing `debug_log("media", ...)` infrastructure
 * gates on `GTKHX_DEBUG=media` and the prefix matches the
 * surrounding C-side log lines (download progress, autofetch
 * lifecycle, dialog state). The Rust side preformats the
 * message into a Rust-owned CString; we just unwrap to the
 * `debug_log` format string and let it run. The pointer is
 * read-only for the duration of the call. */
void
hx_image_decode_log (const char *msg)
{
    if (!msg) {
        return;
    }
    debug_log ("media", "%s", msg);
}
