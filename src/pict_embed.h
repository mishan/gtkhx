/*
 * pict_embed.h — extract an embedded JPEG/PNG/GIF/TIFF image from
 * a PICT v2 byte stream.
 *
 * This is the "sniff" approach to PICT preview (Option 3 in the
 * roadmap). It does NOT decode classic QuickDraw raster opcodes —
 * the long tail of PackBits-compressed pixmaps, framed/filled
 * vector primitives, etc. — but it handles the common case where
 * a PICT v2 file is essentially a thin wrapper around a real image
 * format. That covers the majority of screenshots produced by mid-
 * 1990s+ Mac tools (GraphicConverter, the macOS classic
 * screencapture, etc.), which are by far the most common .pict
 * files seen on Hotline servers.
 *
 * If this returns NULL, fall back to a heavier decoder (ImageMagick
 * or netpbm picttoppm — neither integrated yet).
 *
 * Pure GLib; safe to call from any thread; no allocations beyond
 * the returned GBytes (which holds its own copy).
 */

#ifndef HX_PICT_EMBED_H
#define HX_PICT_EMBED_H 1

#include <glib.h>

G_BEGIN_DECLS

/* Search `data` (whole PICT file, including the 512-byte header)
 * for the first JPEG, PNG, GIF, or TIFF magic signature. If one is
 * found, return a newly-allocated GBytes spanning that signature to
 * the end of the input — caller g_bytes_unref's. Returns NULL when
 * no signature is found or the input is too short to be a PICT
 * (less than 512+1 bytes).
 *
 * Returning from-signature-to-end rather than computing the exact
 * embedded blob size is deliberate: the image decoders (GdkPixbuf,
 * libjpeg, libpng, libgif, libtiff) all parse only as much as they
 * need from a byte stream and stop at their own end-of-image
 * markers. The trailing PICT opcodes after the embedded blob are
 * ignored harmlessly. */
extern GBytes *hx_pict_extract_embedded (const guint8 *data, gsize len);

G_END_DECLS

#endif /* HX_PICT_EMBED_H */
