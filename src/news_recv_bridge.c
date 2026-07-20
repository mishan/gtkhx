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
