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
#include "hotline_proto.h" /* gtkhx_proto_parse_history_entry */
#include "network.h"       /* hlwrite_chunks */
#include "chat_history.h"
#include "debug.h"

/* ---- Packed-binary entry parser -------------------------------- */

HxHistoryEntry *
hx_history_entry_parse (const guint8 *data, gsize len)
{
    /* packed-binary decode (24-byte fixed header + nick
	 * + message + best-effort mini-TLV walk) moved to the Rust
	 * hotline-proto crate's parse_history_entry. The C side keeps
	 * the HxHistoryEntry allocation and the by-length nick /
	 * message copies (g_malloc + memcpy + trailing NUL — see the
	 * comment on those allocations below for why g_strndup is
	 * wrong here) — the entry's owner expects g_free-able
	 * strings, and crossing the FFI allocator boundary would
	 * complicate the contract for no gain. */
    struct gtkhx_proto_history_entry parsed;
    if (!gtkhx_proto_parse_history_entry (data, len, &parsed)) {
        return NULL;
    }

    HxHistoryEntry *entry = g_new0 (HxHistoryEntry, 1);
    entry->message_id = parsed.message_id;
    entry->timestamp = parsed.timestamp;
    entry->flags = parsed.flags;
    entry->icon_id = parsed.icon_id;

    /* Copy by length, not by g_strndup. g_strndup stops at the
	 * first embedded NUL and allocates only what it copied + 1,
	 * but we record nick_len / message_len from the wire and
	 * downstream consumers (e.g. g_strstr_len) use those lengths
	 * with length-aware APIs. A wire payload that contains an
	 * interior NUL (the server has no obligation to scrub them)
	 * would then have the length-aware reader walk past the
	 * truncated allocation. g_malloc + memcpy + trailing NUL
	 * keeps allocation length and recorded length in lockstep. */
    entry->nick_len = parsed.nick_len;
    entry->nick = g_malloc (parsed.nick_len + 1);
    if (parsed.nick_len) {
        memcpy (entry->nick, data + parsed.nick_off, parsed.nick_len);
    }
    entry->nick[parsed.nick_len] = '\0';

    entry->message_len = parsed.msg_len;
    entry->message = g_malloc (parsed.msg_len + 1);
    if (parsed.msg_len) {
        memcpy (entry->message, data + parsed.msg_off, parsed.msg_len);
    }
    entry->message[parsed.msg_len] = '\0';

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
