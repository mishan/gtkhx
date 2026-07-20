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

#include "protocol.h" /* struct htlc_conn / struct qbuf */
#include "session.h"  /* struct gnews_catalog / gnews_folder */
#include "news_recv_bridge.h"

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
news_post_new (struct news_item *item, const guint8 *body, gsize body_len)
{
    /* The news-thread carrier: the parsed body (owned, g_strndup'd so
     * gnews_browser_handle_thread frees it with g_free) + the stub news_item
     * that keys pending_threads. Created per-reply — the Rust receive handler
     * can't sizeof the C struct across the FFI, so it's built here. */
    struct news_post *post = g_malloc (sizeof (struct news_post));
    post->buf = g_strndup ((const char *) body, body_len);
    post->item = item;
    return post;
}
