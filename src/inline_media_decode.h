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
 * Two layers:
 *
 *   inline_media_sniff — magic-byte sniff. GTK-free (only basic
 *     GLib scalar typedefs in the signature: guint8, gsize,
 *     gboolean — no GObject / GTK / pixbuf), unit-testable in
 *     isolation, and bounded: inspects at most the first 32
 *     bytes of the input regardless of total length. Returns
 *     the detected format (JPEG / PNG / GIF / blocked /
 *     unknown). The allowlist mirrors the spec's "Supported
 *     Formats" table: JPEG, PNG, GIF only; SVG / WebP / AVIF /
 *     HEIC / TIFF are explicitly recognised so we can reject
 *     them even if a hostile or buggy server claims a different
 *     canonical MIME.
 *
 *   inline_media_decode — full decode pipeline. Calls sniff,
 *     enforces a dimension + pixel cap via the loader's
 *     "size-prepared" hook BEFORE rendering the full image,
 *     then materialises a GdkTexture. Maps every failure mode
 *     onto the spec's MediaErrorCode (0..=5).
 *
 * The spec mandates that clients NOT trust server-supplied
 * canonical MIME, dimensions, or byte size as a substitute for
 * the client's own decode validation. Even the server has run
 * the bytes through its validation + re-encode pipeline; we
 * still bound + sniff + decode locally as defence-in-depth
 * against a hostile or buggy server.
 *
 * Animated GIF is recognised by sniff and decoded by the
 * loader, but only the first frame is rendered in v1. Animated
 * playback is v2 (per docs/inline-media-plan.md).
 */

#ifndef HX_INLINE_MEDIA_DECODE_H
#define HX_INLINE_MEDIA_DECODE_H 1

#include <glib.h>
#include <stdint.h>

/* Forward declaration keeps this header GTK-free while letting
 * the result struct carry a strongly-typed GdkTexture pointer.
 * Consumers that actually use the texture pull in
 * <gdk/gdk.h> themselves. */
typedef struct _GdkTexture GdkTexture;

/* Detected image format. The "blocked" variants are recognised
 * specifically so the rejection log line is honest about WHY
 * the bytes were rejected. Unknown is the catch-all. */
typedef enum {
    INLINE_MEDIA_FORMAT_UNKNOWN = 0,
    /* Allowlisted. */
    INLINE_MEDIA_FORMAT_JPEG,
    INLINE_MEDIA_FORMAT_PNG,
    INLINE_MEDIA_FORMAT_GIF,
    /* Recognised and rejected (per spec). */
    INLINE_MEDIA_FORMAT_SVG,
    INLINE_MEDIA_FORMAT_WEBP,
    INLINE_MEDIA_FORMAT_AVIF,
    INLINE_MEDIA_FORMAT_HEIC,
    INLINE_MEDIA_FORMAT_TIFF,
    INLINE_MEDIA_FORMAT_ICO,
    INLINE_MEDIA_FORMAT_BMP,
} HxInlineMediaFormat;

/* Magic-byte sniff. Returns the detected format, or _UNKNOWN if
 * the bytes don't match any known image signature. Inspects at
 * most the first 32 bytes; safe to call with len < 32 (just
 * returns _UNKNOWN early). */
extern HxInlineMediaFormat inline_media_sniff (const guint8 *bytes,
                                               gsize len);

/* True for the formats that are allowed under the inline-media
 * cap bit (JPEG / PNG / GIF). False for everything else,
 * including the explicitly-blocked formats and _UNKNOWN. */
extern gboolean inline_media_format_is_allowed (HxInlineMediaFormat f);

/* Canonical MIME string for the format. Returns a constant
 * literal — caller doesn't free. Returns NULL for _UNKNOWN. */
extern const char *inline_media_format_to_mime (HxInlineMediaFormat f);

/* ---- Decode pipeline -------------------------------------------- */

/* Caps the decode pipeline enforces. Mirrors HX_MEDIA_DEFAULT_*
 * but takes them as explicit arguments so the caller can pass
 * the per-connection server-advertised limits where available.
 *
 * max_bytes: hard reject if input len exceeds this. The Phase 9.A
 *   builder already rejects oversized uploads, but the receive
 *   path could see anything the server hands back — so the
 *   decoder doesn't trust the caller to have checked.
 * max_dimension: width OR height limit in pixels (each axis).
 * max_pixels: width × height limit (decoded pixel count). The
 *   size-prepared callback gates here BEFORE the full decode runs.
 */
typedef struct {
    guint32 max_bytes;
    guint32 max_dimension;
    guint32 max_pixels;
} HxInlineMediaCaps;

/* Result of a decode attempt. On success texture is a strong
 * ref the caller owns; on failure texture is NULL and
 * error_code is the spec MediaErrorCode (0..=5).
 *
 * canonical_mime is a borrowed pointer to a static string; the
 * caller doesn't free.
 *
 * sniffed_format is the format the magic-byte sniff identified
 * (useful for logging). Always set, even on failure. */
typedef struct {
    GdkTexture *texture;
    const char *canonical_mime;
    HxInlineMediaFormat sniffed_format;
    /* Maps to inline_media.rs::MediaErrorCode wire values
	 * (0 = generic, 1 = too large, 2 = unsupported, etc.). */
    guint16 error_code;
    /* Human-readable failure reason for the log. Static
	 * string; caller doesn't free. NULL on success. */
    const char *error_message;
} HxInlineMediaDecoded;

/* Decode bytes into a GdkTexture under the supplied caps.
 *
 * Returns a populated HxInlineMediaDecoded:
 *   - texture non-NULL on success, NULL on failure.
 *   - error_code is the wire MediaErrorCode value.
 *   - sniffed_format is always set.
 *
 * Bound failures (max_bytes / max_dimension / max_pixels) map to
 * code 1 (PayloadTooLarge). Sniff failures (unknown or blocked
 * format) map to code 2 (UnsupportedFormat). Loader failures map
 * to code 2 (UnsupportedFormat) — the bytes claimed a known magic
 * but the actual decode failed.
 *
 * On success the texture is a strong reference; caller is
 * responsible for g_object_unref when done.
 *
 * Safe to call from any thread that can use GdkPixbufLoader
 * (Phase 9.D's main-thread call site is fine; a worker-thread
 * variant comes for free since the loader is thread-safe per its
 * own docs). The caller never holds the GDK lock — pure compute.
 */
extern HxInlineMediaDecoded inline_media_decode (const guint8 *bytes,
                                                 gsize len,
                                                 const HxInlineMediaCaps *caps);

#endif /* HX_INLINE_MEDIA_DECODE_H */
