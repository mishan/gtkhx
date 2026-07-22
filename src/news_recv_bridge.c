/*
 * Copyright (C) 2000-2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

/*
 * news_recv_bridge.c — the C leaves the Rust 1.5 news receive handlers
 * (hxnews-recv) and the news browser (gtkhx-ui) can't reach across the FFI:
 * the received frame body (htlc->in), the live session version + access
 * bitmap, the post-date formatter, and the row-icon loader. The news reply
 * carriers themselves (gnews_folder / gnews_catalog / news_post) are now
 * Rust-owned in hxnews-recv's `carrier` module.
 */

#include "config.h"

#include <glib.h>
#include <gtk/gtk.h>
#include <time.h>

#include "protocol.h" /* struct htlc_conn / struct qbuf */
#include "hxconn.h"
#include "session.h"  /* struct date_time */
#include "hx.h"       /* hx_active_session */
#include "hl_access.h"
#include "hl_date.h"
#include "gtkhx_icon.h" /* gtkhx_icon_load */
#include "gtkutil.h"    /* gtkhx_texture_from_pixbuf */
#include "news_recv_bridge.h"

/* hxnews-model — a post node's parsed date, for the date formatter below. */
extern void hx_news_node_get_date (void *node, struct date_time *out);

/* ---- session / htlc accessors for the Rust news browser ---- */

int
gtkhx_news_htlc_version (void)
{
    return (int) hx_conn_version (hx_active_session ()->htlc);
}

int
gtkhx_news_access_has (int bit)
{
    return hx_conn_access_has (hx_active_session ()->htlc, bit);
}

int
gtkhx_news_access_permits (int bit)
{
    return hx_conn_access_permits (hx_active_session ()->htlc, bit);
}

/* ---- post-date formatting (C leaf: hl_date_decode + strftime) ----
 *
 * Format a post's date the way the browser's post pane + reply-context card
 * show it. Auto-detects the Mac 1904 epoch vs. modern wire format via
 * hl_date_decode. Returns a newly-allocated string (caller g_free's), the
 * Rust news renderer's gtkhx_news_node_date_string extern. */
static char *
post_date_format (const struct date_time *dt)
{
    /* Pack the struct date_time back into the 8-byte wire layout
     * hl_date_decode expects (year:2 / pad:2 / seconds:4, big-endian). */
    guint8 buf[8];
    buf[0] = (guint8) (dt->base_year >> 8);
    buf[1] = (guint8) (dt->base_year & 0xff);
    buf[2] = (guint8) (dt->pad >> 8);
    buf[3] = (guint8) (dt->pad & 0xff);
    buf[4] = (guint8) (dt->seconds >> 24);
    buf[5] = (guint8) (dt->seconds >> 16);
    buf[6] = (guint8) (dt->seconds >> 8);
    buf[7] = (guint8) (dt->seconds & 0xff);

    time_t t;
    if (!hl_date_decode (buf, &t)) {
        return g_strdup ("");
    }

    struct tm tm_buf;
    if (!localtime_r (&t, &tm_buf)) {
        return g_strdup ("");
    }

    char out[64];
    if (strftime (out, sizeof out, "%a %b %e %H:%M:%S %Y", &tm_buf) == 0) {
        return g_strdup ("");
    }
    return g_strdup (out);
}

char *
gtkhx_news_node_date_string (void *node)
{
    struct date_time dt;
    if (!node) {
        return NULL;
    }
    hx_news_node_get_date (node, &dt);
    return post_date_format (&dt);
}

/* ---- row-icon loader (C leaf: pixbuf → 1.5x → texture) ----
 *
 * Load one icon resource through the theme resolver, upscale 1.5x with
 * nearest-neighbour to keep the pixel-art crisp, and wrap it as a
 * GdkPaintable. Returns NULL on a missing resource (Rust null-checks). Goes
 * pixbuf → texture rather than gtk_image_set_from_resource, which renders
 * blank for our XPM/PNG pixmaps (the same reason gtkhx_pixmap_button does). */
void *
gtkhx_news_load_icon_paintable (const char *resource)
{
    GdkPixbuf *pb = gtkhx_icon_load (resource);
    if (!pb) {
        return NULL;
    }

    int w = (gdk_pixbuf_get_width (pb) * 3) / 2;
    int h = (gdk_pixbuf_get_height (pb) * 3) / 2;
    GdkPixbuf *scaled = gdk_pixbuf_scale_simple (pb, w, h, GDK_INTERP_NEAREST);
    g_object_unref (pb);
    if (!scaled) {
        return NULL;
    }

    GdkTexture *tex = gtkhx_texture_from_pixbuf (scaled);
    g_object_unref (scaled);
    return tex ? GDK_PAINTABLE (tex) : NULL;
}
