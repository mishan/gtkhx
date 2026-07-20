/*
 * Copyright (C) 2000-2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

/*
 * news_recv_bridge.h — the C leaves the Rust 1.5 news receive handlers
 * (hxnews-recv) and the news browser (gtkhx-ui) can't reach across the FFI.
 * The reply carriers (gnews_folder / gnews_catalog / news_post) are Rust-owned
 * in hxnews-recv's `carrier` module; what's left here is the received frame
 * body (htlc->in), the live session version + access bitmap, and two view
 * leaves (post-date formatter + row-icon loader).
 */

#ifndef GTKHX_NEWS_RECV_BRIDGE_H
#define GTKHX_NEWS_RECV_BRIDGE_H

#include <glib.h>

G_BEGIN_DECLS

struct htlc_conn;

/* The received message buffer + its length (htlc->in.buf / htlc->in.pos — the
 * frame body the reply dispatch left staged). Generic htlc accessors, not
 * news-specific; the news parsers scan this for their chunk. */
const guint8 *hx_htlc_in_buf (struct htlc_conn *htlc);
gsize hx_htlc_in_pos (struct htlc_conn *htlc);

/* ---- session / htlc accessors for the Rust news browser ----
 * The browser gates its RPC + toolbar sensitivity on the live session version
 * and access bitmap, which live on the C htlc_conn. */
int gtkhx_news_htlc_version (void);
int gtkhx_news_access_has (int bit);
int gtkhx_news_access_permits (int bit);

/* Format a post node's date (hl_date_decode + strftime) as a newly-allocated
 * string (caller g_free's), and load a row-icon resource as a 1.5x-upscaled
 * GdkPaintable (NULL on miss). Both are C leaves the Rust news browser calls. */
char *gtkhx_news_node_date_string (void *node);
void *gtkhx_news_load_icon_paintable (const char *resource);

G_END_DECLS

#endif /* GTKHX_NEWS_RECV_BRIDGE_H */
