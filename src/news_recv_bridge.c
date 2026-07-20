/*
 * Copyright (C) 2000-2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

/*
 * news_recv_bridge.c — C-struct accessors for the Rust 1.5 news receive
 * handlers (hxnews-recv). Mirrors news_send_bridge.c's role for the senders.
 */

#include "config.h"

#include <glib.h>
#include <gtk/gtk.h>
#include <time.h>

#include "protocol.h" /* struct htlc_conn / struct qbuf */
#include "session.h"  /* struct gnews_catalog / gnews_folder / date_time */
#include "hx.h"       /* hx_active_session */
#include "hl_access.h"
#include "hl_date.h"
#include "gtkhx_icon.h" /* gtkhx_icon_load */
#include "gtkutil.h"    /* gtkhx_texture_from_pixbuf */
#include "news_recv_bridge.h"

/* hxnews-model — clear the "fetch in flight" flag so a failed GETTHREAD can be
 * retried. HxNewsNode * crosses as a GObject *. */
extern void hx_news_node_set_body_fetching (void *node, gboolean fetching);
extern void hx_news_node_get_date (void *node, struct date_time *out);

/* ---- session / htlc accessors for the Rust news browser ---- */

int
gtkhx_news_htlc_version (void)
{
    return (int) hx_active_session ()->htlc.version;
}

int
gtkhx_news_access_has (int bit)
{
    return hl_access_has ((const guint8 *) &hx_active_session ()->htlc.access,
                          bit);
}

int
gtkhx_news_access_permits (int bit)
{
    return hl_access_permits (
        (const guint8 *) &hx_active_session ()->htlc.access, bit);
}

/* ---- gnews_folder / gnews_catalog carriers (fetch → send → rcv → handle) ----
 *
 * The Rust browser can't sizeof these session.h structs across the FFI, so it
 * allocates / reads ->parsed / frees them here. The send path reads ->path +
 * flips ->listing (news_send_bridge.c); the receive path stashes ->parsed
 * (gnews_*_set_parsed above). */

void *
gnews_folder_new (const char *path)
{
    struct gnews_folder *g = g_malloc0 (sizeof *g);
    g->path = g_strdup (path);
    return g;
}

void *
gnews_folder_parsed (void *g)
{
    return g ? ((struct gnews_folder *) g)->parsed : NULL;
}

void
gnews_folder_free (void *g)
{
    struct gnews_folder *f = g;
    if (f) {
        g_free (f->path);
        g_free (f);
    }
}

void *
gnews_catalog_new (const char *path)
{
    struct gnews_catalog *g = g_malloc0 (sizeof *g);
    g->path = g_strdup (path);
    return g;
}

void *
gnews_catalog_parsed (void *g)
{
    return g ? ((struct gnews_catalog *) g)->parsed : NULL;
}

void
gnews_catalog_free (void *g)
{
    struct gnews_catalog *c = g;
    if (c) {
        g_free (c->path);
        g_free (c);
    }
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

const guint8 *
hx_htlc_in_buf (struct htlc_conn *htlc)
{
    return htlc ? htlc->in.buf : NULL;
}

gsize
hx_htlc_in_pos (struct htlc_conn *htlc)
{
    return htlc ? htlc->in.pos : 0;
}

void
gnews_catalog_set_parsed (struct gnews_catalog *g, void *parsed)
{
    if (g) {
        g->parsed = parsed;
    }
}

void
gnews_folder_set_parsed (struct gnews_folder *g, void *parsed)
{
    if (g) {
        g->parsed = parsed;
    }
}

void *
news_post_new (void *target, const guint8 *body, gsize body_len)
{
    /* The news-thread carrier: the parsed body (owned, g_strndup'd so
     * gnews_browser_handle_thread frees it with g_free) + the HxNewsNode being
     * fetched (carrying its transfer-full ref). Created per-reply — the Rust
     * receive handler can't sizeof the C struct across the FFI, so it's here. */
    struct news_post *post = g_malloc (sizeof (struct news_post));
    post->buf = g_strndup ((const char *) body, body_len);
    post->target = target;
    return post;
}

void
news_post_fetch_failed (void *target)
{
    /* GETTHREAD reply that carried no usable body (TASK_ERROR / missing
     * NEWSDATA). No news_post is emitted, so release the transfer-full target
     * ref here and clear body_fetching so the user can retry. */
    if (target) {
        hx_news_node_set_body_fetching (target, FALSE);
        g_object_unref (target);
    }
}

/* news_post carrier read/free for the Rust GETTHREAD reply handler
 * (gnews_browser_handle_thread) — it can't sizeof the C struct across the FFI. */
void *
news_post_target (void *post)
{
    return post ? ((struct news_post *) post)->target : NULL;
}

const char *
news_post_body (void *post)
{
    return post ? ((struct news_post *) post)->buf : NULL;
}

void
news_post_free (void *post)
{
    struct news_post *p = post;
    if (p) {
        g_free (p->buf);
        g_free (p);
    }
}
