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
 * (useful for logging). Always set, even on failure.
 *
 * Memory layout pinned `#[repr(C)]` on the Rust side
 * (rust/crates/hx-image-decode); a `_Static_assert` in
 * inline_media_decode.c catches drift at compile time.
 *
 * G.3 adds an animation-frames pointer here; G.2 leaves it
 * NULL on every result. */
typedef struct {
    GdkTexture *texture;
    const char *canonical_mime;
    HxInlineMediaFormat sniffed_format;
    /* Maps to inline_media.rs::MediaErrorCode wire values
	 * (0 = generic, 1 = too large, 2 = unsupported, etc.). */
    guint16 error_code;
    /* 16-bit pad to match Rust #[repr(C)] field layout. */
    guint16 _pad0;
    /* Human-readable failure reason for the log. Static
	 * string; caller doesn't free. NULL on success. */
    const char *error_message;
    /* Animation frames (G.3 reservation; always NULL in G.2).
	 * When non-NULL: a GArray<HxInlineMediaFrame> with the
	 * full frame set + per-frame delay. The static-image case
	 * leaves this NULL and the consumer reads `texture`
	 * directly. */
    GArray *frames;
} HxInlineMediaDecoded;

/* Async decode entry. The bytes pointer is consumed
 * synchronously (copied into a glib::Bytes that survives the
 * subprocess hand-off), so the caller's buffer can be released
 * as soon as this returns.
 *
 * The result is delivered to `cb(result, user_data)` on the GLib
 * main thread once the glycin sandbox completes — typically
 * 30–200 ms for the inline-media cap (256 KiB encoded, 2048×2048
 * decoded). The callback OWNS the `HxInlineMediaDecoded *` and
 * MUST call `inline_media_decoded_free(result)` to release it.
 *
 * Returns an opaque cancel token, or NULL on synchronous reject
 * (NULL bytes / zero-length / bytes-cap exceeded / blocked
 * format from sniff). When NULL is returned, the callback fires
 * exactly once before this call returns with the cap or sniff
 * error encoded — no async work was scheduled. When non-NULL,
 * the callback fires once async unless the caller invokes
 * `inline_media_decode_cancel(token)` first.
 *
 * The cancel token is reference-shared with the in-flight decode
 * task — either side dropping its ref is safe, and a successful
 * decode plus a late cancel are safely race-free. The caller
 * MUST eventually call `inline_media_decode_cancel(token)` on
 * non-NULL tokens (it's the canonical free function for the
 * token; cancel-after-completion is a no-op but still frees).
 *
 * caps may be NULL — the decoder falls back to HX_MEDIA_DEFAULT_*
 * per missing field. */
typedef void (*HxInlineMediaDecodeCallback)(HxInlineMediaDecoded *result,
                                            gpointer user_data);

extern gpointer inline_media_decode_async (const guint8 *bytes,
                                           gsize len,
                                           const HxInlineMediaCaps *caps,
                                           HxInlineMediaDecodeCallback cb,
                                           gpointer user_data);

/* Cancel an in-flight decode + release the cancel token. The
 * callback will NOT fire after this call returns — the
 * subprocess result, if it lands afterwards, is dropped on the
 * floor. Safe to call with NULL. */
extern void inline_media_decode_cancel (gpointer token);

/* Release a HxInlineMediaDecoded handed to a callback. Drops
 * the GdkTexture ref (if any), the G.3 frames GArray (if any),
 * and frees the struct itself. NULL-safe. */
extern void inline_media_decoded_free (HxInlineMediaDecoded *decoded);

#endif /* HX_INLINE_MEDIA_DECODE_H */
