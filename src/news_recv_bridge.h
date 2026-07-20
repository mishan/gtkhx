/*
 * Copyright (C) 2000-2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

/*
 * news_recv_bridge.h — thin C accessors for the 1.5 news receive handlers that
 * moved to Rust (hxnews-recv). The Rust handler parses out of htlc->in and
 * stashes the owned parse handle on the reply's carrier struct; these shims
 * expose the C-struct fields it can't reach across the FFI.
 */

#ifndef GTKHX_NEWS_RECV_BRIDGE_H
#define GTKHX_NEWS_RECV_BRIDGE_H

#include <glib.h>

G_BEGIN_DECLS

struct htlc_conn;
struct gnews_catalog;

/* The received message buffer + its length (htlc->in.buf / htlc->in.pos — the
 * frame body the reply dispatch left staged). Generic htlc accessors, not
 * news-specific; the news parsers scan this for their chunk. */
const guint8 *hx_htlc_in_buf (struct htlc_conn *htlc);
gsize hx_htlc_in_pos (struct htlc_conn *htlc);

/* Stash the owned CatList parse handle on the catalog carrier for
 * gnews_browser_handle_catlist to pick up + free. */
void gnews_catalog_set_parsed (struct gnews_catalog *g, void *parsed);

G_END_DECLS

#endif /* GTKHX_NEWS_RECV_BRIDGE_H */
