/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "compat.h"   /* PACKED — required before hotline.h */
#include "hotline.h"
#include "protocol.h"
#include "proto_helpers.h" /* struct hx_chunk */
#include "network.h"       /* hlwrite_chunks */
#include "chat_history.h"
#include "debug.h"

/* ---- Packed-binary entry parser -------------------------------- */

/* Big-endian byte loaders. We deliberately don't include
 * <arpa/inet.h> for ntohs/ntohl — the parser stays self-contained
 * so Tier 2 fixtures can build it without the network headers. */

static inline guint16
be16 (const guint8 *p)
{
    return ((guint16) p[0] << 8) | (guint16) p[1];
}

static inline guint32
be32 (const guint8 *p)
{
    return ((guint32) p[0] << 24) | ((guint32) p[1] << 16)
         | ((guint32) p[2] << 8)  | (guint32) p[3];
}

static inline guint64
be64 (const guint8 *p)
{
    return ((guint64) be32 (p) << 32) | (guint64) be32 (p + 4);
}

HxHistoryEntry *
hx_history_entry_parse (const guint8 *data, gsize len)
{
    /* Minimum: 8 (msgid) + 8 (ts) + 2 (flags) + 2 (icon) + 2
     * (nick_len) + 2 (msg_len) = 24 bytes with empty nick + msg. */
    if (!data || len < 24) {
        return NULL;
    }

    guint16 nick_len = be16 (data + 20);
    /* Header (22) + nick + 2 (msg_len) ≤ len */
    if ((gsize) 22 + nick_len + 2 > len) {
        return NULL;
    }
    guint16 msg_len = be16 (data + 22 + nick_len);
    /* Header (24) + nick + msg ≤ len */
    if ((gsize) 24 + nick_len + msg_len > len) {
        return NULL;
    }

    HxHistoryEntry *entry = g_new0 (HxHistoryEntry, 1);
    entry->message_id = be64 (data + 0);
    /* timestamp is signed int64 per spec; cast preserves the
     * two's-complement bit pattern. */
    entry->timestamp = (gint64) be64 (data + 8);
    entry->flags     = be16 (data + 16);
    entry->icon_id   = be16 (data + 18);

    entry->nick_len    = nick_len;
    entry->nick        = g_malloc (nick_len + 1);
    if (nick_len) {
        memcpy (entry->nick, data + 22, nick_len);
    }
    entry->nick[nick_len] = '\0';

    entry->message_len = msg_len;
    entry->message     = g_malloc (msg_len + 1);
    if (msg_len) {
        memcpy (entry->message, data + 24 + nick_len, msg_len);
    }
    entry->message[msg_len] = '\0';

    /* Mini-TLV sub-fields follow after the message body. The spec
     * defines none in v1, but we must walk past any that appear so
     * a future server emitting them doesn't trip our caller. Each
     * sub-field: uint16 type, uint16 length, length bytes data.
     * Malformed sub-fields (length runs past the buffer) stop
     * iteration silently — we keep the entry, just don't expose
     * the sub-fields we couldn't parse. */
    gsize off = (gsize) 24 + nick_len + msg_len;
    while (off + 4 <= len) {
        guint16 sub_type = be16 (data + off);
        guint16 sub_len  = be16 (data + off + 2);
        if (off + 4 + sub_len > len) {
            debug_log ("chat-history",
                       "entry %" G_GUINT64_FORMAT ": malformed sub-field "
                       "(type=0x%04x, len=%u, off=%zu, total=%zu) — stopping",
                       entry->message_id, sub_type, sub_len, off, len);
            break;
        }
        /* No sub-field types defined yet — skip silently. */
        (void) sub_type;
        off += (gsize) 4 + sub_len;
    }

    return entry;
}

void
hx_history_entry_free (HxHistoryEntry *entry)
{
    if (!entry) {
        return;
    }
    g_free (entry->nick);
    g_free (entry->message);
    g_free (entry);
}

/* ---- Request sender -------------------------------------------- */

int
hx_get_chat_history_build_chunks (guint32 channel_id, guint64 before,
                                  guint64 after, guint16 limit,
                                  struct hx_chunk *chunks, int chunks_cap,
                                  struct hx_get_chat_history_scratch *scratch)
{
    if (!chunks || chunks_cap < 4 || !scratch) {
        return 0;
    }

    /* All numeric fields are sent big-endian. Stash host→network
     * conversions into caller-owned scratch storage so the
     * struct hx_chunk data pointers below remain valid past
     * function return — the eventual hlpack_chunks/hlwrite_chunks
     * call memcpys out of them, but the harness path may delay
     * the pack briefly past the build. */
    scratch->channel_be = GUINT32_TO_BE (channel_id);
    scratch->before_be  = GUINT64_TO_BE (before);
    scratch->after_be   = GUINT64_TO_BE (after);
    scratch->limit_be   = GUINT16_TO_BE (limit);

    /* Build the chunk list dynamically — only include optional
     * cursor / limit chunks when their host-side value is non-zero.
     * channel_id is always sent (the spec mandates it). The
     * hlwrite_chunks array-style packer collapses the 2^3 = 8
     * combinations of (before, after, limit) into one call site
     * — no variadic-dispatch enumeration. */
    int hc = 0;
    chunks[hc++] = (struct hx_chunk) { HTLC_DATA_CHANNEL_ID, 4,
                                       &scratch->channel_be };
    if (before) {
        chunks[hc++] = (struct hx_chunk) { HTLC_DATA_HISTORY_BEFORE, 8,
                                           &scratch->before_be };
    }
    if (after) {
        chunks[hc++] = (struct hx_chunk) { HTLC_DATA_HISTORY_AFTER, 8,
                                           &scratch->after_be };
    }
    if (limit) {
        chunks[hc++] = (struct hx_chunk) { HTLC_DATA_HISTORY_LIMIT, 2,
                                           &scratch->limit_be };
    }

    return hc;
}

gboolean
hx_get_chat_history (struct htlc_conn *htlc, guint32 channel_id,
                     guint64 before, guint64 after, guint16 limit)
{
    if (!htlc) {
        return FALSE;
    }

    /* Spec: clients MUST NOT send TRAN 700 if CAP_CHAT_HISTORY
     * wasn't echoed by the server in the login reply. Sending it
     * anyway earns a task-error every time. */
    if (!(htlc->caps & HTLC_CAP_CHAT_HISTORY)) {
        debug_log ("chat-history",
                   "skip GET_CHAT_HISTORY: server didn't echo "
                   "CAP_CHAT_HISTORY (caps=0x%" G_GINT64_MODIFIER "x)",
                   htlc->caps);
        return FALSE;
    }

    /* Note: callers are responsible for task_new()-registering
     * rcv_task_chat_history BEFORE invoking this function — the
     * task is keyed on htlc->trans which hlwrite_chunks is about
     * to consume. chat_history.c stays free of tasks.h / rcv.h
     * (which need the full hx.h context) so the Tier 2 fixture
     * tests can build the parser + sender without dragging in
     * the GTK pile. */
    struct hx_chunk chunks[4];
    struct hx_get_chat_history_scratch scratch;
    int hc = hx_get_chat_history_build_chunks (channel_id, before, after,
                                               limit, chunks, 4, &scratch);
    hlwrite_chunks (htlc, HTLC_HDR_GET_CHAT_HISTORY, 0, chunks, hc);
    return TRUE;
}
