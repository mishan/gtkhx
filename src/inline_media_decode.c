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

/* ---- Sniff ---------------------------------------------------- */

static gboolean
prefix_matches (const guint8 *buf, gsize len, const guint8 *needle,
                gsize needle_len)
{
    if (len < needle_len) {
        return FALSE;
    }
    return memcmp (buf, needle, needle_len) == 0;
}

/* Sniff SVG. Allowlisted by the spec for rejection: SVG is XML,
 * scriptable, can fetch network resources. The detection is
 * conservative — any leading whitespace / BOM, plus the literal
 * "<?xml" or "<svg" prefix. We don't bother with full XML
 * parsing; the bytes are getting rejected either way. */
static gboolean
sniff_svg (const guint8 *buf, gsize len)
{
    gsize i = 0;
    /* Skip BOM. */
    if (len >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) {
        i = 3;
    }
    /* Skip leading whitespace. */
    while (i < len && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n'
                       || buf[i] == '\r')) {
        i++;
    }
    if (i + 5 <= len && memcmp (buf + i, "<?xml", 5) == 0) {
        return TRUE;
    }
    if (i + 4 <= len && memcmp (buf + i, "<svg", 4) == 0) {
        return TRUE;
    }
    return FALSE;
}

HxInlineMediaFormat
inline_media_sniff (const guint8 *bytes, gsize len)
{
    if (!bytes || len == 0) {
        return INLINE_MEDIA_FORMAT_UNKNOWN;
    }

    /* Bound the sniff window at 32 bytes regardless of input
	 * length. Every magic signature in the allowlist + blocklist
	 * below fits in the first 12 bytes; the SVG check
	 * additionally scans past leading whitespace, so clamping
	 * `len` here is what enforces the documented "bounded hot
	 * path" contract — without it sniff_svg could walk an
	 * arbitrarily long leading-whitespace run, defeating the
	 * O(1)-sniff promise in the header. */
    if (len > 32) {
        len = 32;
    }

    /* JPEG: SOI marker FF D8 FF (then a third byte that's any
	 * APPn / SOI marker). The third FF byte is checked to rule
	 * out 0xFFD8 in random data. */
    if (prefix_matches (bytes, len, (const guint8 *) "\xFF\xD8\xFF", 3)) {
        return INLINE_MEDIA_FORMAT_JPEG;
    }

    /* PNG: 8-byte signature 89 50 4E 47 0D 0A 1A 0A. */
    if (prefix_matches (bytes, len,
                        (const guint8 *) "\x89PNG\r\n\x1A\n", 8)) {
        return INLINE_MEDIA_FORMAT_PNG;
    }

    /* GIF: ASCII "GIF87a" or "GIF89a". */
    if (prefix_matches (bytes, len, (const guint8 *) "GIF87a", 6)
        || prefix_matches (bytes, len, (const guint8 *) "GIF89a", 6)) {
        return INLINE_MEDIA_FORMAT_GIF;
    }

    /* RIFF...WEBP. RIFF is 4-byte magic at offset 0; WEBP is
	 * 4 ASCII bytes at offset 8 (after the RIFF + 4-byte size). */
    if (len >= 12 && prefix_matches (bytes, 4, (const guint8 *) "RIFF", 4)
        && memcmp (bytes + 8, "WEBP", 4) == 0) {
        return INLINE_MEDIA_FORMAT_WEBP;
    }

    /* ISO BMFF container: ....ftypavif / ....ftypheic / ....ftyphei[xcsm]
	 * at offset 4. AVIF: brand "avif" / "avis". HEIC: brand
	 * "heic" / "heix" / "hevc" / "hevx" / "heim" / "heis" / "mif1". */
    if (len >= 12 && memcmp (bytes + 4, "ftyp", 4) == 0) {
        const guint8 *brand = bytes + 8;
        if (memcmp (brand, "avif", 4) == 0 || memcmp (brand, "avis", 4) == 0) {
            return INLINE_MEDIA_FORMAT_AVIF;
        }
        if (memcmp (brand, "heic", 4) == 0 || memcmp (brand, "heix", 4) == 0
            || memcmp (brand, "hevc", 4) == 0 || memcmp (brand, "hevx", 4) == 0
            || memcmp (brand, "heim", 4) == 0 || memcmp (brand, "heis", 4) == 0
            || memcmp (brand, "hevm", 4) == 0 || memcmp (brand, "hevs", 4) == 0
            || memcmp (brand, "mif1", 4) == 0) {
            return INLINE_MEDIA_FORMAT_HEIC;
        }
    }

    /* TIFF: 49 49 2A 00 (little-endian) or 4D 4D 00 2A (big-endian). */
    if (prefix_matches (bytes, len,
                        (const guint8 *) "\x49\x49\x2A\x00", 4)
        || prefix_matches (bytes, len,
                           (const guint8 *) "\x4D\x4D\x00\x2A", 4)) {
        return INLINE_MEDIA_FORMAT_TIFF;
    }

    /* ICO: 00 00 01 00 (reserved + image type). */
    if (prefix_matches (bytes, len,
                        (const guint8 *) "\x00\x00\x01\x00", 4)) {
        return INLINE_MEDIA_FORMAT_ICO;
    }

    /* BMP: "BM" at offset 0. */
    if (prefix_matches (bytes, len, (const guint8 *) "BM", 2)) {
        return INLINE_MEDIA_FORMAT_BMP;
    }

    /* SVG check last — it requires scanning past whitespace
	 * and is more expensive than the prefix-equality checks
	 * above. */
    if (sniff_svg (bytes, len)) {
        return INLINE_MEDIA_FORMAT_SVG;
    }

    return INLINE_MEDIA_FORMAT_UNKNOWN;
}

gboolean
inline_media_format_is_allowed (HxInlineMediaFormat f)
{
    switch (f) {
    case INLINE_MEDIA_FORMAT_JPEG:
    case INLINE_MEDIA_FORMAT_PNG:
    case INLINE_MEDIA_FORMAT_GIF:
        return TRUE;
    default:
        return FALSE;
    }
}

const char *
inline_media_format_to_mime (HxInlineMediaFormat f)
{
    switch (f) {
    case INLINE_MEDIA_FORMAT_JPEG:
        return "image/jpeg";
    case INLINE_MEDIA_FORMAT_PNG:
        return "image/png";
    case INLINE_MEDIA_FORMAT_GIF:
        return "image/gif";
    case INLINE_MEDIA_FORMAT_SVG:
        return "image/svg+xml";
    case INLINE_MEDIA_FORMAT_WEBP:
        return "image/webp";
    case INLINE_MEDIA_FORMAT_AVIF:
        return "image/avif";
    case INLINE_MEDIA_FORMAT_HEIC:
        return "image/heic";
    case INLINE_MEDIA_FORMAT_TIFF:
        return "image/tiff";
    case INLINE_MEDIA_FORMAT_ICO:
        return "image/x-icon";
    case INLINE_MEDIA_FORMAT_BMP:
        return "image/bmp";
    case INLINE_MEDIA_FORMAT_UNKNOWN:
    default:
        return NULL;
    }
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

    /* GdkTexture wants raw bytes; the cleanest GTK 4 path is
	 * gdk_texture_new_for_pixbuf (deprecated in 4.16, same as
	 * the other gtkutil.c wrappers around the helper) — but it
	 * still works and replaces our progressive-loader pixbuf
	 * with a paintable suitable for GtkPicture. The receive
	 * dialog (Phase 9.D) consumes the GdkTexture directly. */
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
