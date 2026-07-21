/*
 * Copyright (C) 2000-2026 Misha Nasledov <misha@nasledov.com>
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
 */

/*
 * cicn.c — the thin GdkPixbuf wrapper over the Rust cicn decoder.
 *
 * The decode itself (PixMap / BitMap / ColorTable walk, palette build, pixel
 * unpack, mask→alpha fold, and the 1px halo) moved to the `hxcicn` crate, which
 * produces a packed RGBA buffer with no GTK dependency. Here we wrap that RGBA
 * into a GdkPixbuf (zero-copy: the pixbuf takes ownership of the g_malloc'd
 * buffer via a g_free destroy-notify) and keep the icon-file lookup glue.
 */

#include "config.h"

#include <gtk/gtk.h>

#include "macres.h"
#include "cicn.h"
#include "session.h" /* struct ifn */
#include "debug.h"

#define DEFAULT_ICON 135

/* hxcicn crate: decode a cicn resource to a freshly g_malloc'd RGBA buffer +
 * its dimensions, or NULL on malformed input. */
extern guchar *hxcicn_decode (const guint8 *rsrc, gsize len, guint *out_w,
                              guint *out_h);

/* GdkPixbufDestroyNotify shape (guchar *pixels, gpointer data) — a wrapper
 * rather than a cast of g_free, to avoid -Wcast-function-type. */
static void
free_pixels (guchar *pixels, gpointer data)
{
    (void) data;
    g_free (pixels);
}

GdkPixbuf *
cicn_to_pixbuf (void *cicn_rsrc, unsigned int len)
{
    guint w = 0, h = 0;
    guchar *rgba = hxcicn_decode ((const guint8 *) cicn_rsrc, len, &w, &h);

    if (!rgba) {
        return NULL;
    }

    /* Zero-copy: on success the pixbuf owns the RGBA buffer and g_free's it on
     * finalize. If construction fails the destroy-notify never runs, so free
     * the buffer here rather than leak it. */
    GdkPixbuf *pb = gdk_pixbuf_new_from_data (rgba, GDK_COLORSPACE_RGB, TRUE, 8,
                                              w, h, (int) (w * 4), free_pixels,
                                              NULL);
    if (!pb) {
        g_free (rgba);
    }
    return pb;
}

/*
 * Look up an icon by Mac resource id across the loaded resource files, decode
 * it, and hand back ownership via *pixbuf_out. The mask out-param is unused
 * (alpha lives in the pixbuf) and set to NULL for source compatibility with the
 * historical 5-arg signature.
 */
void
load_icon (GtkWidget *widget, guint16 icon, struct ifn *ifn, char recurse,
           GdkPixbuf **pixbuf_out, GdkPixbuf **mask_out)
{
    macres_res *cicn = NULL;
    unsigned int i;

    (void) widget;

    for (i = 0; i < ifn->n; i++) {
        if (!ifn->cicns[i]) {
            continue;
        }
        cicn = macres_file_get_resid_of_type (ifn->cicns[i], TYPE_cicn, icon);
        if (cicn) {
            break;
        }
    }

    if (cicn) {
        GdkPixbuf *pb = cicn_to_pixbuf (cicn->data, cicn->datalen);
        /* macres_file_get_resid_of_type returns a g_malloc'd wrapper we own. */
        g_free (cicn);
        if (pb) {
            debug_log ("icon", "load_icon: id=%u -> %dx%d pixbuf",
                       (unsigned) icon, gdk_pixbuf_get_width (pb),
                       gdk_pixbuf_get_height (pb));
            *pixbuf_out = pb;
            if (mask_out) {
                *mask_out = NULL;
            }
            return;
        }
    }

    debug_log ("icon", "load_icon: id=%u not resolved (n=%u)%s", (unsigned) icon,
               ifn->n, recurse ? " — falling back" : "");

    if (recurse) {
        load_icon (widget, DEFAULT_ICON, ifn, 0, pixbuf_out, mask_out);
    } else {
        *pixbuf_out = NULL;
        if (mask_out) {
            *mask_out = NULL;
        }
    }
}
