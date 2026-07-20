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
struct gnews_folder;

/* The received message buffer + its length (htlc->in.buf / htlc->in.pos — the
 * frame body the reply dispatch left staged). Generic htlc accessors, not
 * news-specific; the news parsers scan this for their chunk. */
const guint8 *hx_htlc_in_buf (struct htlc_conn *htlc);
gsize hx_htlc_in_pos (struct htlc_conn *htlc);

/* Stash the owned parse handle on the reply carrier for the view handler
 * (gnews_browser_handle_catlist / _dirlist) to pick up + free. */
void gnews_catalog_set_parsed (struct gnews_catalog *g, void *parsed);
void gnews_folder_set_parsed (struct gnews_folder *g, void *parsed);

/* Build the news-thread carrier (struct news_post) for the Rust GETTHREAD
 * handler: g_strndup's the parsed body + carries the target HxNewsNode (and its
 * transfer-full ref). Returned opaque; gnews_browser_handle_thread frees it. */
void *news_post_new (void *target, const guint8 *body, gsize body_len);

/* Release the transfer-full target ref + clear body_fetching when a GETTHREAD
 * reply carries no body (so no news_post is emitted to carry it onward). */
void news_post_fetch_failed (void *target);

/* news_post carrier read/free for the Rust GETTHREAD reply handler. */
void *news_post_target (void *post);
const char *news_post_body (void *post);
void news_post_free (void *post);

/* ---- session / htlc accessors for the Rust news browser ----
 * The browser gates its RPC + toolbar sensitivity on the live session version
 * and access bitmap, which live on the C htlc_conn. */
int gtkhx_news_htlc_version (void);
int gtkhx_news_access_has (int bit);
int gtkhx_news_access_permits (int bit);

/* ---- gnews_folder / gnews_catalog carriers ----
 * The Rust browser can't sizeof the session.h structs, so it allocates the
 * dirlist / catlist request carriers (path-only stubs), reads their parsed
 * handle after the reply lands, and frees them here. */
void *gnews_folder_new (const char *path);
void *gnews_folder_parsed (void *g);
void gnews_folder_free (void *g);
void *gnews_catalog_new (const char *path);
void *gnews_catalog_parsed (void *g);
void gnews_catalog_free (void *g);

/* Format a post node's date (hl_date_decode + strftime) as a newly-allocated
 * string (caller g_free's), and load a row-icon resource as a 1.5x-upscaled
 * GdkPaintable (NULL on miss). Both are C leaves the Rust news browser calls. */
char *gtkhx_news_node_date_string (void *node);
void *gtkhx_news_load_icon_paintable (const char *resource);

G_END_DECLS

#endif /* GTKHX_NEWS_RECV_BRIDGE_H */
