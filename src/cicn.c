/*
 * Copyright (C) 2000-2002 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General
 * Public License along with this program; if not, write to the
 * Free Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/*
 * Phase 3.4 rewrite: produce GdkPixbuf directly instead of going through
 * the GTK 1/2 GdkImage → GdkPixmap + GdkBitmap mask pipeline.  GdkImage,
 * gdk_image_put_pixel, gdk_pixmap_new, gdk_draw_image, gdk_image_new_bitmap,
 * and the colormap-based pixel allocator are all gone in GTK 3.
 *
 * Approach: walk the Mac classic cicn resource the same way the original
 * decoder did (1/2/4/8 bpp pixel paths against a per-icon color table,
 * falling back to the canonical Mac palette for any indexes the table
 * doesn't override) but write packed RGBA bytes straight into a
 * GdkPixbuf.  The mask bitmap is folded into the pixbuf's alpha channel
 * (Mac classic iconMask: 1 = visible, 0 = transparent), removing the need
 * for a separate GdkBitmap output.
 *
 * Public API:
 *   GdkPixbuf *cicn_to_pixbuf (void *cicn_rsrc, unsigned int len);
 *   void load_icon (GtkWidget *, guint16, struct ifn *, char recurse,
 *                   GdkPixbuf **out, GdkPixbuf **mask_unused);
 *
 * The mask out-param is kept for source-level compatibility with the old
 * 5-parameter call sites; it is always set to NULL and the gtk_hlist
 * compat shim's pixmap_to_pixbuf() already ignores it (alpha lives in the
 * pixbuf).  The widget out-param is also kept, but is now only consulted
 * for a fallback colormap-free decode (no GdkVisual lookup needed).
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <fnmatch.h>
#include <netinet/in.h>
#include <gdk/gdk.h>
#include <gtk/gtk.h>
#include <sys/time.h>
#include <time.h>
#include "macres.h"
#include "hx.h"
#include "cicn.h"
#include "users.h"
#include "gtkhx.h"

#define DEFAULT_ICON	135

/* Canonical Mac classic 8-bit palette.  Each channel is in the legacy
 * 16-bit-per-channel form (high byte is the 8-bit value, low byte is
 * usually a copy or zero), to match the per-icon ColorTable stored inside
 * cicn resources.  Anywhere we'd packed an X11 pixel before, we now pack
 * 0xRRGGBB -- alpha is added separately when we write the pixbuf. */

static const RGBColor rgb_8[256] = {
	{ 0xff00, 0xff00, 0xff00 }, { 0xff00, 0xfe00, 0xcb00 },
	{ 0xff00, 0xfe00, 0x9a00 }, { 0xff00, 0xfe00, 0x6600 },
	{ 0xff00, 0xfe00, 0x3300 }, { 0xfe00, 0xfe00, 0x0000 },
	{ 0xff00, 0xcb00, 0xfe00 }, { 0xff00, 0xcb00, 0xcb00 },
	{ 0xff00, 0xcc00, 0x9a00 }, { 0xff00, 0xcc00, 0x6600 },
	{ 0xff00, 0xcc00, 0x3300 }, { 0xfe00, 0xcb00, 0x0000 },
	{ 0xff00, 0x9a00, 0xfe00 }, { 0xff00, 0x9a00, 0xcc00 },
	{ 0xff00, 0x9a00, 0x9a00 }, { 0xff00, 0x9900, 0x6600 },
	{ 0xff00, 0x9900, 0x3300 }, { 0xfe00, 0x9800, 0x0000 },
	{ 0xff00, 0x6600, 0xfe00 }, { 0xff00, 0x6600, 0xcc00 },
	{ 0xff00, 0x6600, 0x9900 }, { 0xff00, 0x6600, 0x6600 },
	{ 0xff00, 0x6600, 0x3300 }, { 0xfe00, 0x6500, 0x0000 },
	{ 0xff00, 0x3300, 0xfe00 }, { 0xff00, 0x3300, 0xcc00 },
	{ 0xff00, 0x3300, 0x9900 }, { 0xff00, 0x3300, 0x6600 },
	{ 0xff00, 0x3300, 0x3300 }, { 0xfe00, 0x3200, 0x0000 },
	{ 0xfe00, 0x0000, 0xfe00 }, { 0xfe00, 0x0000, 0xcb00 },
	{ 0xfe00, 0x0000, 0x9800 }, { 0xfe00, 0x0000, 0x6500 },
	{ 0xfe00, 0x0000, 0x3200 }, { 0xfe00, 0x0000, 0x0000 },
	{ 0xcb00, 0xff00, 0xff00 }, { 0xcb00, 0xff00, 0xcb00 },
	{ 0xcc00, 0xff00, 0x9a00 }, { 0xcc00, 0xff00, 0x6600 },
	{ 0xcc00, 0xff00, 0x3300 }, { 0xcb00, 0xfe00, 0x0000 },
	{ 0xcb00, 0xcb00, 0xff00 }, { 0xcc00, 0xcc00, 0xcc00 },
	{ 0xcc00, 0xcc00, 0x9900 }, { 0xcc00, 0xcc00, 0x6600 },
	{ 0xcb00, 0xcb00, 0x3200 }, { 0xcd00, 0xcd00, 0x0000 },
	{ 0xcc00, 0x9a00, 0xff00 }, { 0xcc00, 0x9900, 0xcc00 },
	{ 0xcc00, 0x9900, 0x9900 }, { 0xcc00, 0x9900, 0x6600 },
	{ 0xcb00, 0x9800, 0x3200 }, { 0xcd00, 0x9a00, 0x0000 },
	{ 0xcc00, 0x6600, 0xff00 }, { 0xcc00, 0x6600, 0xcc00 },
	{ 0xcc00, 0x6600, 0x9900 }, { 0xcc00, 0x6600, 0x6600 },
	{ 0xcb00, 0x6500, 0x3200 }, { 0xcd00, 0x6600, 0x0000 },
	{ 0xcc00, 0x3300, 0xff00 }, { 0xcb00, 0x3200, 0xcb00 },
	{ 0xcb00, 0x3200, 0x9800 }, { 0xcb00, 0x3200, 0x6500 },
	{ 0xcb00, 0x3200, 0x3200 }, { 0xcd00, 0x3300, 0x0000 },
	{ 0xcb00, 0x0000, 0xfe00 }, { 0xcd00, 0x0000, 0xcd00 },
	{ 0xcd00, 0x0000, 0x9a00 }, { 0xcd00, 0x0000, 0x6600 },
	{ 0xcd00, 0x0000, 0x3300 }, { 0xcd00, 0x0000, 0x0000 },
	{ 0x9a00, 0xff00, 0xff00 }, { 0x9a00, 0xff00, 0xcc00 },
	{ 0x9a00, 0xff00, 0x9a00 }, { 0x9900, 0xff00, 0x6600 },
	{ 0x9900, 0xff00, 0x3300 }, { 0x9900, 0xfe00, 0x0000 },
	{ 0x9a00, 0xcc00, 0xff00 }, { 0x9900, 0xcc00, 0xcc00 },
	{ 0x0000, 0x9800, 0x6500 }, { 0x9900, 0xcc00, 0x6600 },
	{ 0x9900, 0xcb00, 0x3200 }, { 0x9a00, 0xcd00, 0x0000 },
	{ 0x9a00, 0x9a00, 0xff00 }, { 0x9900, 0x9900, 0xcc00 },
	{ 0x9900, 0x9900, 0x9900 }, { 0x9800, 0x9800, 0x6500 },
	{ 0x9a00, 0x9a00, 0x3300 }, { 0x9800, 0x9800, 0x0000 },
	{ 0x9900, 0x6600, 0xff00 }, { 0x9900, 0x6600, 0xcc00 },
	{ 0x9800, 0x6500, 0x9800 }, { 0x9800, 0x6500, 0x6500 },
	{ 0x9a00, 0x6600, 0x3300 }, { 0x9800, 0x6500, 0x0000 },
	{ 0x9900, 0x3300, 0xff00 }, { 0x9800, 0x3200, 0xcb00 },
	{ 0x9a00, 0x3300, 0x9a00 }, { 0x9a00, 0x3300, 0x6600 },
	{ 0x9a00, 0x3300, 0x3300 }, { 0x9800, 0x3200, 0x0000 },
	{ 0x9800, 0x0000, 0xfe00 }, { 0x9a00, 0x0000, 0xcd00 },
	{ 0x9800, 0x0000, 0x9800 }, { 0x9800, 0x0000, 0x6500 },
	{ 0x9800, 0x0000, 0x3200 }, { 0x9800, 0x0000, 0x0000 },
	{ 0x6600, 0xff00, 0xff00 }, { 0x6600, 0xff00, 0xcc00 },
	{ 0x6600, 0xff00, 0x9900 }, { 0x6600, 0xff00, 0x6600 },
	{ 0x6600, 0xff00, 0x3300 }, { 0x6600, 0xfe00, 0x0000 },
	{ 0x6600, 0xcc00, 0xff00 }, { 0x6600, 0xcc00, 0xcc00 },
	{ 0x6600, 0xcc00, 0x9900 }, { 0x6600, 0xcc00, 0x6600 },
	{ 0x6600, 0xcb00, 0x3200 }, { 0x6600, 0xcd00, 0x0000 },
	{ 0x6600, 0x9900, 0xff00 }, { 0x6600, 0x9900, 0xcc00 },
	{ 0x6500, 0x9800, 0x9800 }, { 0x6500, 0x9800, 0x6500 },
	{ 0x6600, 0x9a00, 0x3300 }, { 0x6500, 0x9800, 0x0000 },
	{ 0x6600, 0x6600, 0xff00 }, { 0x6600, 0x6600, 0xcc00 },
	{ 0x6500, 0x6500, 0x9800 }, { 0x6600, 0x6600, 0x6600 },
	{ 0x6500, 0x6500, 0x3200 }, { 0x6600, 0x6600, 0x0000 },
	{ 0x6600, 0x3300, 0xff00 }, { 0x6500, 0x3200, 0xcb00 },
	{ 0x6600, 0x3300, 0x9a00 }, { 0x6500, 0x3200, 0x6500 },
	{ 0x6500, 0x3200, 0x3200 }, { 0x6600, 0x3300, 0x0000 },
	{ 0x6500, 0x0000, 0xfe00 }, { 0x6600, 0x0000, 0xcd00 },
	{ 0x6500, 0x0000, 0x9800 }, { 0x6600, 0x0000, 0x6600 },
	{ 0x6600, 0x0000, 0x3300 }, { 0x6600, 0x0000, 0x0000 },
	{ 0x3300, 0xff00, 0xff00 }, { 0x3300, 0xff00, 0xcc00 },
	{ 0x3300, 0xff00, 0x9900 }, { 0x3300, 0xff00, 0x6600 },
	{ 0x3300, 0xff00, 0x3300 }, { 0x3300, 0xfe00, 0x0000 },
	{ 0x3300, 0xcc00, 0xff00 }, { 0x3200, 0xcb00, 0xcb00 },
	{ 0x3200, 0xcb00, 0x9800 }, { 0x3200, 0xcb00, 0x6500 },
	{ 0x3300, 0xcb00, 0x3200 }, { 0x3300, 0xcd00, 0x0000 },
	{ 0x3300, 0x9900, 0xff00 }, { 0x3200, 0x9900, 0xcb00 },
	{ 0x3300, 0x9a00, 0x9a00 }, { 0x3300, 0x9a00, 0x6600 },
	{ 0x3300, 0x9a00, 0x3300 }, { 0x3200, 0x9800, 0x0000 },
	{ 0x3300, 0x6600, 0xff00 }, { 0x3200, 0x6600, 0xcb00 },
	{ 0x3300, 0x6600, 0x9a00 }, { 0x3200, 0x6500, 0x6500 },
	{ 0x3200, 0x6500, 0x3200 }, { 0x3300, 0x6600, 0x0000 },
	{ 0x3300, 0x3300, 0xff00 }, { 0x3200, 0x3300, 0xcb00 },
	{ 0x3300, 0x3300, 0x9a00 }, { 0x3200, 0x3200, 0x6500 },
	{ 0x3300, 0x3300, 0x3300 }, { 0x3300, 0x3300, 0x0000 },
	{ 0x3200, 0x0000, 0xfe00 }, { 0x3300, 0x0000, 0xcd00 },
	{ 0x3200, 0x0000, 0x9800 }, { 0x3300, 0x0000, 0x6600 },
	{ 0x3300, 0x0000, 0x3300 }, { 0x3300, 0x0000, 0x0000 },
	{ 0x0000, 0xfe00, 0xfe00 }, { 0x0000, 0xfe00, 0xcb00 },
	{ 0x0000, 0xfe00, 0x9800 }, { 0x0000, 0xfe00, 0x6500 },
	{ 0x0000, 0xfe00, 0x3200 }, { 0x0000, 0xfe00, 0x0000 },
	{ 0x0000, 0xcb00, 0xfe00 }, { 0x0000, 0xcd00, 0xcd00 },
	{ 0x0000, 0xcd00, 0x9a00 }, { 0x0000, 0xcd00, 0x6600 },
	{ 0x0000, 0xcd00, 0x3300 }, { 0x0000, 0xcd00, 0x0000 },
	{ 0x0000, 0x9800, 0xfe00 }, { 0x0000, 0x9a00, 0xcd00 },
	{ 0x0000, 0x9800, 0x9800 }, { 0x0000, 0x9800, 0x6500 },
	{ 0x0000, 0x9800, 0x3200 }, { 0x0000, 0x9800, 0x0000 },
	{ 0x0000, 0x6600, 0xfe00 }, { 0x0000, 0x6600, 0xcd00 },
	{ 0x0000, 0x6500, 0x9800 }, { 0x0000, 0x6600, 0x6600 },
	{ 0x0000, 0x6600, 0x3300 }, { 0x0000, 0x6600, 0x0000 },
	{ 0x0000, 0x3300, 0xfe00 }, { 0x0000, 0x3300, 0xcd00 },
	{ 0x0000, 0x3200, 0x9800 }, { 0x0000, 0x3300, 0x6600 },
	{ 0x0000, 0x3300, 0x3300 }, { 0x0000, 0x3300, 0x0000 },
	{ 0x0000, 0x0000, 0xfe00 }, { 0x0000, 0x0000, 0xcd00 },
	{ 0x0000, 0x0000, 0x9800 }, { 0x0000, 0x0000, 0x6600 },
	{ 0x0000, 0x0000, 0x3300 }, { 0xef00, 0x0000, 0x0000 },
	{ 0xdc00, 0x0000, 0x0000 }, { 0xba00, 0x0000, 0x0000 },
	{ 0xab00, 0x0000, 0x0000 }, { 0x8900, 0x0000, 0x0000 },
	{ 0x7700, 0x0000, 0x0000 }, { 0x5500, 0x0000, 0x0000 },
	{ 0x4400, 0x0000, 0x0000 }, { 0x2200, 0x0000, 0x0000 },
	{ 0x1100, 0x0000, 0x0000 }, { 0x0000, 0xef00, 0x0000 },
	{ 0x0000, 0xdc00, 0x0000 }, { 0x0000, 0xba00, 0x0000 },
	{ 0x0000, 0xab00, 0x0000 }, { 0x0000, 0x8900, 0x0000 },
	{ 0x0000, 0x7700, 0x0000 }, { 0x0000, 0x5500, 0x0000 },
	{ 0x0000, 0x4400, 0x0000 }, { 0x0000, 0x2200, 0x0000 },
	{ 0x0000, 0x1100, 0x0000 }, { 0x0000, 0x0000, 0xef00 },
	{ 0x0000, 0x0000, 0xdc00 }, { 0x0000, 0x0000, 0xba00 },
	{ 0x0000, 0x0000, 0xab00 }, { 0x0000, 0x0000, 0x8900 },
	{ 0x0000, 0x0000, 0x7700 }, { 0x0000, 0x0000, 0x5500 },
	{ 0x0000, 0x0000, 0x4400 }, { 0x0000, 0x0000, 0x2200 },
	{ 0x0000, 0x0000, 0x1100 }, { 0xee00, 0xee00, 0xee00 },
	{ 0xdd00, 0xdd00, 0xdd00 }, { 0xbb00, 0xbb00, 0xbb00 },
	{ 0xaa00, 0xaa00, 0xaa00 }, { 0x8800, 0x8800, 0x8800 },
	{ 0x7700, 0x7700, 0x7700 }, { 0x5500, 0x5500, 0x5500 },
	{ 0x4400, 0x4400, 0x4400 }, { 0x2200, 0x2200, 0x2200 },
	{ 0x1100, 0x1100, 0x1100 }, { 0x0000, 0x0000, 0x0000 }
};

static const RGBColor rgb_4[16] = {
	{ 0xffff, 0xffff, 0xffff }, { 0xffff, 0xffff, 0x0000 },
	{ 0xffff, 0xa0a0, 0x7a7a }, { 0xffff, 0x0000, 0x0000 },
	{ 0xffff, 0x1414, 0x9393 }, { 0x8a8a, 0x2b2b, 0xe2e2 },
	{ 0x0000, 0x0000, 0x8080 }, { 0x6464, 0x9595, 0xeded },
	{ 0x2222, 0x8b8b, 0x2222 }, { 0x0000, 0x6464, 0x0000 },
	{ 0x8b8b, 0x4545, 0x1313 }, { 0xd2d2, 0xb4b4, 0x8c8c },
	{ 0xd3d3, 0xd3d3, 0xd3d3 }, { 0xbebe, 0xbebe, 0xbebe },
	{ 0x6969, 0x6969, 0x6969 }, { 0x0000, 0x0000, 0x0000 }
};

static const RGBColor rgb_2[4] = {
	{ 0xffff, 0xffff, 0xffff }, { 0xffff, 0xffff, 0x0000 },
	{ 0x0000, 0xffff, 0xffff }, { 0x0000, 0x0000, 0x0000 }
};

static const RGBColor rgb_1[2] = {
	{ 0xffff, 0xffff, 0xffff },
	{ 0x0000, 0x0000, 0x0000 }
};

/* RGBColor stores each channel in the legacy 16-bit-per-channel form
 * (high byte = the actual 8-bit value).  Pack to 8-bit RGB. */
/* Take RGBColor by value — the alternative was '&ct->ctTable[i].rgb'
 * inside a packed struct, which trips -Waddress-of-packed-member
 * because the resulting pointer might not satisfy RGBColor's natural
 * alignment. RGBColor is only 6 bytes so the by-value copy is cheap. */
static inline guint32
rgb_pack (RGBColor c)
{
	return ((guint32)(c.red   >> 8) << 16)
	     | ((guint32)(c.green >> 8) <<  8)
	     | ((guint32)(c.blue  >> 8) <<  0);
}

/* Defined further down — declared here so cicn_to_pixbuf can wrap its
 * output through the halo before returning. */
GdkPixbuf *cicn_add_halo (GdkPixbuf *src);

/*
 * Build a 256-entry palette table for an icon: take the canonical Mac
 * palette as the default, then overlay any entries the per-icon
 * ColorTable specifies.  bpp determines how many entries we actually
 * care about, but we always size the array at 256 (cheap, simple).
 */
static void
build_palette (guint32 *out, unsigned int bpp, ColorTable *ct)
{
	unsigned int n = 1u << bpp;
	unsigned int i, ctsize;
	const RGBColor *defpal;

	switch (bpp) {
		case 8: defpal = rgb_8; break;
		case 4: defpal = rgb_4; break;
		case 2: defpal = rgb_2; break;
		case 1: defpal = rgb_1; break;
		default: return;
	}

	for (i = 0; i < n; i++)
		out[i] = rgb_pack (defpal[i]);

	ctsize = ntohs (ct->ctSize) + 1;
	for (i = 0; i < ctsize; i++) {
		unsigned int v = ntohs (ct->ctTable[i].value) & (n - 1);
		out[v] = rgb_pack (ct->ctTable[i].rgb);
	}
}

/*
 * Decode a Mac classic cicn resource into a freshly-allocated ARGB
 * GdkPixbuf.  Returns NULL on malformed input.  The caller owns the
 * pixbuf (one strong ref) and should g_object_unref() it when done.
 *
 * Mask handling: if the cicn has a mask BitMap, its bits become the
 * pixbuf alpha (Mac classic: 1 = visible, 0 = transparent).  If there
 * is no mask, the pixbuf is fully opaque.
 */
GdkPixbuf *
cicn_to_pixbuf (void *cicn_rsrc, unsigned int len)
{
	PixMap *pm  = (PixMap *)((unsigned char *)cicn_rsrc);
	BitMap *mbm = (BitMap *)((unsigned char *)cicn_rsrc + 50);
	BitMap *bm  = (BitMap *)((unsigned char *)cicn_rsrc + 64);
	/* Bounds are stored as (top, left, bottom, right) Mac classic Rects
	 * — guint16 each. Validate ordering BEFORE the subtraction; an
	 * inverted Rect (right < left or bottom < top, which we have seen
	 * in user-supplied icons.rsrc files) makes the unsigned diff wrap
	 * to a huge value that gets reinterpreted as a negative int by
	 * gdk_pixbuf_new, tripping its `width > 0' assertion. Cap at 4096
	 * on each axis as a sanity ceiling for cicn icons (the format's
	 * native max is 64). */
	guint16 b_top    = ntohs (pm->bounds.top);
	guint16 b_left   = ntohs (pm->bounds.left);
	guint16 b_bottom = ntohs (pm->bounds.bottom);
	guint16 b_right  = ntohs (pm->bounds.right);
	guint16 mb_top    = ntohs (mbm->bounds.top);
	guint16 mb_bottom = ntohs (mbm->bounds.bottom);
	guint16 bb_top    = ntohs (bm->bounds.top);
	guint16 bb_bottom = ntohs (bm->bounds.bottom);
	unsigned int width, height, mbm_h, bm_h;
	unsigned int bpp    = ntohs (pm->pixelSize);
	unsigned int mbm_rb = ntohs (mbm->rowBytes);
	unsigned int bm_rb  = ntohs (bm->rowBytes);
	unsigned int rowBytes = ntohs (pm->rowBytes) & 0x7fff;
	ColorTable *ct;
	guint32 palette[256];
	GdkPixbuf *pb;
	guchar *pixels, *prow;
	int rowstride, n_channels;
	const unsigned char *pixdata, *maskdata;
	unsigned int x, y;
	gboolean have_mask;

	if (bpp != 1 && bpp != 2 && bpp != 4 && bpp != 8)
		return NULL;
	if (b_right <= b_left || b_bottom <= b_top)
		return NULL;
	if (mb_bottom < mb_top || bb_bottom < bb_top)
		return NULL;

	width  = b_right  - b_left;
	height = b_bottom - b_top;
	mbm_h  = mb_bottom - mb_top;
	bm_h   = bb_bottom - bb_top;

	if (width > 4096 || height > 4096)
		return NULL;

	/* Color table sits after both bitmap data blocks; pixel data is
	 * the trailing rowBytes*height bytes of the resource. */
	ct = (ColorTable *)((unsigned char *)cicn_rsrc
		+ 50 + 14 + 14 + 4
		+ mbm_rb * mbm_h
		+ bm_rb  * bm_h);
	pixdata = ((unsigned char *)cicn_rsrc + len) - rowBytes * height;
	maskdata = (unsigned char *)cicn_rsrc + 82;
	have_mask = (mbm->bounds.right != 0 && mbm->bounds.bottom != 0);

	build_palette (palette, bpp, ct);

	pb = gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8, width, height);
	if (!pb)
		return NULL;
	pixels     = gdk_pixbuf_get_pixels (pb);
	rowstride  = gdk_pixbuf_get_rowstride (pb);
	n_channels = gdk_pixbuf_get_n_channels (pb);  /* always 4 with alpha */

	for (y = 0; y < height; y++) {
		const unsigned char *id = pixdata + rowBytes * y;
		const unsigned char *mp = have_mask ? (maskdata + mbm_rb * y) : NULL;
		prow = pixels + y * rowstride;
		for (x = 0; x < width; x++) {
			unsigned int idx;
			guint32 rgb;
			guchar a;

			switch (bpp) {
				case 1:
					idx = (id[x >> 3] >> (7 - (x & 7))) & 0x01;
					break;
				case 2:
					idx = (id[x >> 2] >> ((3 - (x & 3)) * 2)) & 0x03;
					break;
				case 4:
					idx = (id[x >> 1] >> ((1 - (x & 1)) * 4)) & 0x0f;
					break;
				case 8:
				default:
					idx = id[x];
					break;
			}
			rgb = palette[idx];
			a = mp ? (((mp[x >> 3] >> (7 - (x & 7))) & 0x01) ? 0xff : 0x00)
			       : 0xff;

			prow[0] = (rgb >> 16) & 0xff;  /* R */
			prow[1] = (rgb >>  8) & 0xff;  /* G */
			prow[2] = (rgb      ) & 0xff;  /* B */
			prow[3] = a;
			prow += n_channels;
		}
	}

	{
		GdkPixbuf *halo = cicn_add_halo (pb);
		if (halo) {
			g_object_unref (pb);
			pb = halo;
		}
	}

	return pb;
}

/*
 * Add a 1px halo around the icon's visible silhouette.
 *
 * Mac classic cicn icons are designed against the platinum / white
 * Mac OS chat window background; rendered against a dark GTK theme
 * row, the icon body is fine but its edge has nothing separating it
 * from the dark background — light strokes inside the icon get
 * swallowed visually. We expand the pixbuf by 1px on each side and
 * paint a semi-transparent medium grey on each transparent pixel
 * adjacent to an opaque one. The halo blends with the row BG via
 * cairo, so on a light theme it reads as a faint outline and on a
 * dark theme it reads as a definition-improving rim, without
 * touching the icon's own pixels.
 *
 * Returns a newly-allocated pixbuf the caller owns; falls back to
 * NULL on allocation failure (caller continues using the un-haloed
 * source pixbuf).
 */
GdkPixbuf *
cicn_add_halo (GdkPixbuf *src)
{
	int sw, sh, dw, dh;
	int rowstride;
	guchar *pixels;
	guchar *alpha_snap;
	GdkPixbuf *dst;
	int x, y;

	if (!src || !gdk_pixbuf_get_has_alpha (src))
		return NULL;
	if (gdk_pixbuf_get_n_channels (src) != 4)
		return NULL;

	sw = gdk_pixbuf_get_width  (src);
	sh = gdk_pixbuf_get_height (src);
	dw = sw + 2;
	dh = sh + 2;

	dst = gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8, dw, dh);
	if (!dst)
		return NULL;

	gdk_pixbuf_fill (dst, 0x00000000);
	gdk_pixbuf_copy_area (src, 0, 0, sw, sh, dst, 1, 1);

	rowstride = gdk_pixbuf_get_rowstride (dst);
	pixels    = gdk_pixbuf_get_pixels    (dst);

	/* Snapshot the alpha plane so the halo we paint as we walk
	 * doesn't itself feed back into the next pixel's neighbour
	 * check. */
	alpha_snap = g_malloc ((gsize) dw * (gsize) dh);
	for (y = 0; y < dh; y++)
		for (x = 0; x < dw; x++)
			alpha_snap[y * dw + x] = pixels[y * rowstride + x * 4 + 3];

	for (y = 0; y < dh; y++) {
		for (x = 0; x < dw; x++) {
			guchar *p;
			gboolean has_neighbour;

			if (alpha_snap[y * dw + x] != 0)
				continue;	/* already part of the icon */

			has_neighbour = FALSE;
			if (x > 0    && alpha_snap[y * dw + (x - 1)] != 0) has_neighbour = TRUE;
			if (x < dw-1 && alpha_snap[y * dw + (x + 1)] != 0) has_neighbour = TRUE;
			if (y > 0    && alpha_snap[(y - 1) * dw + x] != 0) has_neighbour = TRUE;
			if (y < dh-1 && alpha_snap[(y + 1) * dw + x] != 0) has_neighbour = TRUE;

			if (!has_neighbour)
				continue;

			p = pixels + y * rowstride + x * 4;
			p[0] = 0x80;	/* medium grey reads on both light and dark themes */
			p[1] = 0x80;
			p[2] = 0x80;
			p[3] = 0xa0;	/* ~63% — definition without a heavy border */
		}
	}

	g_free (alpha_snap);
	return dst;
}

/*
 * Look up an icon by Mac resource ID across the loaded resource files,
 * decode it to a GdkPixbuf, and hand back ownership via *pixbuf_out.
 * The mask out-param is unused (alpha lives in the pixbuf) and is set
 * to NULL for source compatibility with the GTK 1.2-era 5-arg signature
 * still in use at call sites.
 */
void
load_icon (GtkWidget *widget, guint16 icon, struct ifn *ifn, char recurse,
           GdkPixbuf **pixbuf_out, GdkPixbuf **mask_out)
{
	macres_res *cicn = NULL;
	GdkPixbuf *pb;
	unsigned int i;

	(void)widget;

	for (i = 0; i < ifn->n; i++) {
		if (!ifn->cicns[i])
			continue;
		cicn = macres_file_get_resid_of_type (ifn->cicns[i], TYPE_cicn, icon);
		if (cicn)
			break;
	}
	if (!cicn)
		goto failure;

	pb = cicn_to_pixbuf (cicn->data, cicn->datalen);
	if (!pb)
		goto failure;

	*pixbuf_out = pb;
	if (mask_out)
		*mask_out = NULL;
	return;

failure:
	if (recurse) {
		load_icon (widget, DEFAULT_ICON, ifn, 0, pixbuf_out, mask_out);
	} else {
		*pixbuf_out = NULL;
		if (mask_out)
			*mask_out = NULL;
	}
}
