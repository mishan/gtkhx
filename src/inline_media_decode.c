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
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gtk/gtk.h>
#include <string.h>

#include "inline_media_decode.h"
#include "compat.h"  /* PACKED — required before hotline.h */
#include "hotline.h" /* HX_MEDIA_DEFAULT_* fallbacks */

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
 * rather than silently miscategorising at runtime. */
_Static_assert (INLINE_MEDIA_FORMAT_UNKNOWN == 0,
                "rust discriminant pin (unknown)");
_Static_assert (INLINE_MEDIA_FORMAT_JPEG == 1,
                "rust discriminant pin (jpeg)");
_Static_assert (INLINE_MEDIA_FORMAT_PNG == 2,
                "rust discriminant pin (png)");
_Static_assert (INLINE_MEDIA_FORMAT_GIF == 3,
                "rust discriminant pin (gif)");
_Static_assert (INLINE_MEDIA_FORMAT_SVG == 4,
                "rust discriminant pin (svg)");
_Static_assert (INLINE_MEDIA_FORMAT_WEBP == 5,
                "rust discriminant pin (webp)");
_Static_assert (INLINE_MEDIA_FORMAT_AVIF == 6,
                "rust discriminant pin (avif)");
_Static_assert (INLINE_MEDIA_FORMAT_HEIC == 7,
                "rust discriminant pin (heic)");
_Static_assert (INLINE_MEDIA_FORMAT_TIFF == 8,
                "rust discriminant pin (tiff)");
_Static_assert (INLINE_MEDIA_FORMAT_ICO == 9,
                "rust discriminant pin (ico)");
_Static_assert (INLINE_MEDIA_FORMAT_BMP == 10,
                "rust discriminant pin (bmp)");

HxInlineMediaFormat
inline_media_sniff (const guint8 *bytes, gsize len)
{
    return (HxInlineMediaFormat) hx_image_decode_sniff (bytes, len);
}

gboolean
inline_media_format_is_allowed (HxInlineMediaFormat f)
{
    return hx_image_decode_format_is_allowed ((guint32) f);
}

const char *
inline_media_format_to_mime (HxInlineMediaFormat f)
{
    return hx_image_decode_format_to_mime ((guint32) f);
}

/* ---- Decode pipeline ----------------------------------------- */

/* Size-prepared callback context. The signal fires once the
 * loader has parsed enough of the header to know the image
 * dimensions but BEFORE the pixel data has been rendered.
 * Setting the loader's size to a smaller value can be used for
 * scaling, but we want to reject oversized inputs outright:
 * gdk_pixbuf_loader_set_size(loader, 1, 1) signals "downscale to
 * 1x1," which serves as a graceful abort — the eventual full
 * decode won't allocate megabytes of pixel buffer. We still
 * abandon the loader after the bound check; the abort is
 * belt-and-suspenders. */
struct decode_ctx {
    guint32 max_dimension;
    guint32 max_pixels;
    gboolean rejected;
    guint32 observed_width;
    guint32 observed_height;
};

static void
on_size_prepared (GdkPixbufLoader *loader, gint w, gint h, gpointer user_data)
{
    struct decode_ctx *ctx = user_data;
    if (w < 1 || h < 1) {
        ctx->rejected = TRUE;
        gdk_pixbuf_loader_set_size (loader, 1, 1);
        return;
    }
    ctx->observed_width = (guint32) w;
    ctx->observed_height = (guint32) h;

    if ((guint32) w > ctx->max_dimension
        || (guint32) h > ctx->max_dimension) {
        ctx->rejected = TRUE;
        gdk_pixbuf_loader_set_size (loader, 1, 1);
        return;
    }
    /* Pixel count overflow-safe multiplication. */
    guint64 pixels = (guint64) w * (guint64) h;
    if (pixels > (guint64) ctx->max_pixels) {
        ctx->rejected = TRUE;
        gdk_pixbuf_loader_set_size (loader, 1, 1);
        return;
    }
}

/* MediaErrorCode wire values mirror inline_media.rs's
 * MediaErrorCode. Keep this table in sync. */
#define MEDIA_ERROR_GENERIC          0
#define MEDIA_ERROR_TOO_LARGE        1
#define MEDIA_ERROR_UNSUPPORTED      2
/* RateLimited (3) / NotAuthorized (4) / ServerBusy (5) are
 * server-side; the local decoder never emits those. */

HxInlineMediaDecoded
inline_media_decode (const guint8 *bytes, gsize len,
                     const HxInlineMediaCaps *caps_in)
{
    HxInlineMediaDecoded out;
    memset (&out, 0, sizeof (out));

    /* Resolve caps — caller may pass NULL or zero fields. */
    HxInlineMediaCaps caps;
    caps.max_bytes
        = (caps_in && caps_in->max_bytes) ? caps_in->max_bytes
                                          : HX_MEDIA_DEFAULT_MAX_BYTES;
    caps.max_dimension = (caps_in && caps_in->max_dimension)
                             ? caps_in->max_dimension
                             : HX_MEDIA_DEFAULT_MAX_DIMENSION;
    caps.max_pixels = (caps_in && caps_in->max_pixels)
                          ? caps_in->max_pixels
                          : HX_MEDIA_DEFAULT_MAX_PIXELS;

    /* 0. Trivial guards. */
    if (!bytes || len == 0) {
        out.error_code = MEDIA_ERROR_UNSUPPORTED;
        out.error_message = "empty payload";
        return out;
    }

    /* 1. Byte-cap pre-check. */
    if (len > (gsize) caps.max_bytes) {
        out.error_code = MEDIA_ERROR_TOO_LARGE;
        out.error_message = "encoded payload exceeds size cap";
        return out;
    }

    /* 2. Magic-byte sniff. */
    out.sniffed_format = inline_media_sniff (bytes, len);
    if (!inline_media_format_is_allowed (out.sniffed_format)) {
        out.error_code = MEDIA_ERROR_UNSUPPORTED;
        out.error_message
            = inline_media_format_to_mime (out.sniffed_format) != NULL
                  ? "format rejected by inline-media allowlist"
                  : "unrecognised image magic bytes";
        return out;
    }
    out.canonical_mime = inline_media_format_to_mime (out.sniffed_format);

    /* 3. Bounded decode via GdkPixbufLoader with size-prepared
	 * gate. */
    GdkPixbufLoader *loader = gdk_pixbuf_loader_new ();
    struct decode_ctx ctx = {
        .max_dimension = caps.max_dimension,
        .max_pixels = caps.max_pixels,
        .rejected = FALSE,
        .observed_width = 0,
        .observed_height = 0,
    };
    g_signal_connect (loader, "size-prepared",
                      G_CALLBACK (on_size_prepared), &ctx);

    GError *err = NULL;
    gboolean wrote = gdk_pixbuf_loader_write (loader, bytes, len, &err);
    /* size-prepared may have fired during write; check before
	 * the close call. */
    if (!wrote) {
        out.error_code = MEDIA_ERROR_UNSUPPORTED;
        out.error_message = "decoder error on write";
        g_clear_error (&err);
        gdk_pixbuf_loader_close (loader, NULL);
        g_object_unref (loader);
        return out;
    }
    gboolean closed = gdk_pixbuf_loader_close (loader, &err);
    if (!closed) {
        /* close() == FALSE is a decode failure regardless of
		 * whether GError was set — gdk_pixbuf_loader_close()
		 * promises a meaningful GError on most paths, but the
		 * contract is the boolean. Treating "false + NULL err"
		 * as success would let us proceed against a loader the
		 * library already considers invalid. */
        out.error_code = MEDIA_ERROR_UNSUPPORTED;
        out.error_message = "decoder error on close";
        g_clear_error (&err);
        g_object_unref (loader);
        return out;
    }
    if (ctx.rejected) {
        out.error_code = MEDIA_ERROR_TOO_LARGE;
        out.error_message
            = "image dimensions exceed cap (width / height / pixels)";
        g_object_unref (loader);
        return out;
    }

    GdkPixbuf *pixbuf = gdk_pixbuf_loader_get_pixbuf (loader);
    if (!pixbuf) {
        out.error_code = MEDIA_ERROR_UNSUPPORTED;
        out.error_message = "decoder produced no pixbuf";
        g_object_unref (loader);
        return out;
    }
    /* gdk_pixbuf_loader_get_pixbuf returns a borrowed pointer;
	 * keep a strong ref so the loader unref doesn't free it. */
    g_object_ref (pixbuf);
    g_object_unref (loader);

    /* Promote the loader's GdkPixbuf to a GdkTexture suitable
	 * for GtkPicture. gdk_texture_new_for_pixbuf is deprecated
	 * in 4.16 (same status as the other gtkutil.c wrappers
	 * around the helper) but still works and is the cleanest
	 * Pixbuf → GdkPaintable conversion in the GTK 4 line-up.
	 * The receive dialog (Phase 9.D) consumes the GdkTexture
	 * directly.
	 *
	 * (The src/preview.c image path uses gdk_texture_new_from_bytes
	 * because it has the raw encoded bytes on hand. Here we
	 * already drove them through a loader to gate on
	 * size-prepared before allocating the pixel buffer, so the
	 * pixbuf is what we have.) */
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    GdkTexture *tex = gdk_texture_new_for_pixbuf (pixbuf);
    G_GNUC_END_IGNORE_DEPRECATIONS
    g_object_unref (pixbuf);

    if (!tex) {
        out.error_code = MEDIA_ERROR_UNSUPPORTED;
        out.error_message = "texture materialisation failed";
        return out;
    }
    out.texture = tex;
    out.error_code = 0;
    out.error_message = NULL;
    return out;
}
