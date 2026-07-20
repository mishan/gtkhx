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

/* hxnews-model — clear the "fetch in flight" flag so a failed GETTHREAD can be
 * retried. HxNewsNode * crosses as a GObject *. */
extern void hx_news_node_set_body_fetching (void *node, gboolean fetching);

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
