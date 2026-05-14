/*
 * pict_magick.h — PICT decoder backed by ImageMagick.
 *
 * Decodes a PICT v1 or v2 byte stream via the MagickWand C API and
 * returns the result as a GdkTexture suitable for display in a
 * GtkPicture.
 *
 * This is the "heavy" fallback for the preview pane (Option 1 in
 * the roadmap discussion). It handles the long tail of PICT files
 * that hx_pict_extract_embedded() can't help with — classic
 * QuickDraw raster opcodes (PackBitsRect/Rgn, DirectBitsRect/Rgn),
 * vector primitives, and color-table-indexed pixmaps. ImageMagick
 * has carried a robust PICT decoder for ~25 years; it's by far the
 * best free-software option for the format.
 *
 * Build-time optional: only compiled when HAVE_IMAGEMAGICK is set
 * (top-level meson.build probes for MagickWand pkg-config). On
 * systems without ImageMagick the function is stubbed out at the
 * header level and PICT files that can't be sniffed fall through
 * to the existing "Failed to decode image" message.
 *
 * Thread safety: MagickWand has process-wide global state
 * (MagickWandGenesis). We initialise lazily on first call from
 * any thread via g_once_init_enter/leave. The decode itself is
 * thread-safe per the MagickWand documentation.
 */

#ifndef HX_PICT_MAGICK_H
#define HX_PICT_MAGICK_H 1

#include "config.h"

#include <gdk/gdk.h>
#include <glib.h>

G_BEGIN_DECLS

#ifdef HAVE_IMAGEMAGICK

/* Decode `data` (whole PICT file) into a GdkTexture. Sets `err`
 * and returns NULL on failure. Caller owns the returned texture
 * (g_object_unref to free).
 *
 * The function is gated on HAVE_IMAGEMAGICK so callers don't need
 * to repeat the conditional. The companion stub below provides a
 * no-op that always returns NULL with err set, so call sites can
 * be unconditional.
 */
extern GdkTexture *hx_pict_magick_decode (const guint8 *data, gsize len,
                                          GError **err);

#else /* !HAVE_IMAGEMAGICK */

static inline GdkTexture *
hx_pict_magick_decode (const guint8 *data, gsize len, GError **err)
{
    (void)data;
    (void)len;
    g_set_error_literal (err, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                         "PICT decoding requires ImageMagick — "
                         "not compiled in");
    return NULL;
}

#endif /* HAVE_IMAGEMAGICK */

G_END_DECLS

#endif /* HX_PICT_MAGICK_H */
