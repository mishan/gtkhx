/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

/*
 * chat_send_bridge.c — see chat_send_bridge.h. Model / caps accessors for
 * the Rust chat wire-senders (hxchat-send). Keeps the htlc_conn + struct
 * chat layout on the C side.
 */

#include "config.h"

#include "hx.h"
#include "hxconn.h"
#include "protocol.h"
#include "session.h"
#include "hotline.h"
#include "chat.h"
#include "chat_send_bridge.h"

gboolean
hx_htlc_text_encoding_cap (struct htlc_conn *htlc)
{
    return htlc && (hx_conn_has_cap (htlc, HTLC_CAP_TEXT_ENCODING)) != 0;
}

void *
hx_chat_lookup (struct htlc_conn *htlc, guint32 cid)
{
    if (!htlc) {
        return NULL;
    }
    return chat_with_cid (sess_from_htlc (htlc), cid);
}

void *
hx_chat_lookup_or_create (struct htlc_conn *htlc, guint32 cid)
{
    struct chat *chat;

    if (!htlc) {
        return NULL;
    }
    chat = chat_with_cid (sess_from_htlc (htlc), cid);
    if (!chat) {
        chat = chat_new (sess_from_htlc (htlc), cid);
    }
    return chat;
}
