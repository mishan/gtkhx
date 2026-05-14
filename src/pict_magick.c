/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "config.h"

#ifdef HAVE_IMAGEMAGICK

#include <MagickWand/MagickWand.h>
#include <glib.h>
#include <gdk/gdk.h>

#include "pict_magick.h"

/* Genesis is process-wide and idempotent, but the docs are firm that
 * it must run before any other MagickWand call. We pay it lazily on
 * the first decode rather than at app startup so non-PICT runs don't
 * pull ImageMagick's allocator and coder-registration tables into
 * memory unnecessarily.
 *
 * Skipped MagickWandTerminus on shutdown: it's a flush-and-free for
 * the process-wide state that the kernel reclaims on exit anyway,
 * and skipping it sidesteps a class of "called after global dtors"
 * bugs that have surfaced in MagickWand over the years. */
static void
ensure_genesis (void)
{
    static gsize once = 0;
    if (g_once_init_enter (&once)) {
        MagickWandGenesis ();
        g_once_init_leave (&once, 1);
    }
}

/* Extract whatever string the wand stashed on its last failed call,
 * formatted as a GError. Frees ImageMagick's buffer either way. */
static void
set_error_from_wand (MagickWand *w, GError **err)
{
    ExceptionType sev;
    char *desc = MagickGetException (w, &sev);
    if (desc && *desc) {
        g_set_error_literal (err, G_FILE_ERROR, G_FILE_ERROR_FAILED, desc);
    } else {
        g_set_error_literal (err, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                             "ImageMagick decode failed (no detail)");
    }
    if (desc) {
        MagickRelinquishMemory (desc);
    }
}

/* Free hook for the GBytes that backs our GdkMemoryTexture: drop
 * the malloc-allocated RGBA pixel buffer. */
static void
free_rgba_buffer (gpointer p)
{
    g_free (p);
}

GdkTexture *
hx_pict_magick_decode (const guint8 *data, gsize len, GError **err)
{
    MagickWand *w;
    size_t width, height;
    guint8 *rgba;
    gsize stride;
    GBytes *gb;
    GdkTexture *tex;

    g_return_val_if_fail (data != NULL, NULL);
    g_return_val_if_fail (len > 0, NULL);

    ensure_genesis ();

    w = NewMagickWand ();
    if (!w) {
        g_set_error_literal (err, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                             "MagickWand alloc failed");
        return NULL;
    }

    /* Pin the input format to PICT. ImageMagick has decoders for ~200
	 * formats and will auto-detect from magic bytes by default; we
	 * explicitly want the PICT decoder here so a non-PICT blob (which
	 * shouldn't get here in practice — the caller gates on the v2
	 * version marker) doesn't accidentally hit a different decoder.
	 * Defense in depth against the long history of CVEs in
	 * ImageMagick's secondary coders. */
    MagickSetFormat (w, "PICT");

    if (MagickReadImageBlob (w, data, len) == MagickFalse) {
        set_error_from_wand (w, err);
        DestroyMagickWand (w);
        return NULL;
    }

    width = MagickGetImageWidth (w);
    height = MagickGetImageHeight (w);
    if (width == 0 || height == 0 || width > 32768 || height > 32768) {
        g_set_error (err, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                     "PICT image has implausible dimensions: %zu x %zu", width,
                     height);
        DestroyMagickWand (w);
        return NULL;
    }

    /* Extract raw RGBA at 8 bpc. This is the most direct path —
	 * MagickGetImageBlob would round-trip through a re-encoded PNG
	 * (and in practice returns the cached source bytes anyway, not
	 * the re-encoded ones), whereas MagickExportImagePixels hands
	 * back a flat buffer of pixels we can wrap in a GdkMemoryTexture
	 * with zero further decoding. */
    stride = (gsize)width * 4;
    rgba = g_malloc ((gsize)height * stride);

    if (MagickExportImagePixels (w, 0, 0, width, height, "RGBA", CharPixel,
                                 rgba)
        == MagickFalse) {
        set_error_from_wand (w, err);
        g_free (rgba);
        DestroyMagickWand (w);
        return NULL;
    }

    DestroyMagickWand (w);

    /* Wrap the pixel buffer in a GBytes — GdkMemoryTexture takes a
	 * reference to the bytes object. Use g_bytes_new_with_free_func
	 * so the texture's last-ref triggers our free hook. */
    gb = g_bytes_new_with_free_func (rgba, (gsize)height * stride,
                                     free_rgba_buffer, rgba);

    tex = gdk_memory_texture_new ((int)width, (int)height, GDK_MEMORY_R8G8B8A8,
                                  gb, stride);
    g_bytes_unref (gb);
    return tex;
}

#endif /* HAVE_IMAGEMAGICK */
