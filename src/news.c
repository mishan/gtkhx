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
 *
 * You should have received a copy of the GNU General
 * Public License along with this program; if not, write to the
 * Free Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <gtk/gtk.h>
#include <netinet/in.h>

#include "hx.h"
#include "hl_access.h"
#include "hotline_proto.h"
#include "network.h"
#include "proto_helpers.h" /* struct hx_chunk (stack-allocated below) */
#include "text_util.h"     /* gtkhx_text_for_wire */
#include "tasks.h"
#include "rcv.h"
#include "debug.h"
#include "news.h"

/* The flat 1.0/1.2 News window content — the button bar, read-only text view,
 * in-content Find search bar, the news-file/news-post output path, the
 * reload / open lifecycle, and the create_news_window shell — moved to Rust
 * (gtkhx-ui news.rs). The two wire senders below stay C for now (the news
 * receive-path parsers already live in hotline-proto, and hx_post_news's chunk
 * layout is gtkhx_proto_build_news_post_chunks); a later phase can move them
 * into a hxnews-send crate mirroring hxchat-send. The Create-Post composer
 * likewise lives in Rust (create_post.rs). */

void
hx_get_news (struct htlc_conn *htlc)
{
    task_new (htlc, RCV_TASK_FN (rcv_task_news_file), 0, 0, "news");
    /* NEWS_GETFILE is a zero-chunk opcode. */
    hlwrite_chunks (htlc, HTLC_HDR_NEWS_GETFILE, 0, NULL, 0);
}

void
hx_post_news (struct htlc_conn *htlc, const char *news, guint16 len)
{
    /* Phase E2/E3: news body — UTF-8 / Mac Roman + LF→CR for
     * legacy servers. The flat 1.0 news file is line-oriented,
     * so getting line endings right is what makes posts render
     * correctly on Mac clients. */
    gboolean utf8 = (htlc->caps & HTLC_CAP_TEXT_ENCODING) != 0;
    gsize wire_len = 0;
    char *wire
        = gtkhx_text_for_wire (news, len, utf8, /*is_body=*/TRUE, &wire_len);

    /* chunk layout moved to gtkhx_proto_build_news_post_chunks.
     * Build BEFORE task_new — see hx_send_msg for the rationale
     * (task_new snapshots htlc->trans into a pending entry; a builder
     * failure must not leave a phantom task behind). */
    struct hx_chunk chunks[1];
    int hc = (int)gtkhx_proto_build_news_post_chunks (
        (const uint8_t *)wire, wire_len, chunks, G_N_ELEMENTS (chunks));
    if (hc > 0) {
        task_new (htlc, 0, 0, 0, "post");
        hlwrite_chunks (htlc, HTLC_HDR_NEWS_POST, 0, chunks, hc);
    }
    g_free (wire);
}
