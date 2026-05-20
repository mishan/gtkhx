/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_chat_history.c — Tier 3 coverage for the
 * fogWraith chat-history extension (TRAN 700 / DATA_HISTORY_*).
 *
 * Every subtest filters the matrix for HX_TEST_CAP_CHAT_HISTORY and
 * skips if no capable target is configured. Today the only entry
 * advertising the bit is Janus (port 5510 in tests/janus/Dockerfile);
 * the test set will pick up any future matrix entry that also flips
 * the cap on.
 *
 * Coverage map (mirrors the chat-history-plan Phase 5 deliverable):
 *
 *   cap_negotiation       — server echoes HTLC_CAP_CHAT_HISTORY in
 *                           the LOGIN TASK reply, populating
 *                           htlc->caps.
 *   round_trip            — send a unique-marker chat line; immediately
 *                           fetch with no cursor and LIMIT=20; find the
 *                           entry we just sent, assert nick + body
 *                           round-trip through the SQLite store.
 *   limit                 — LIMIT=N respects the cap; reply carries
 *                           ≤ N HISTORY_ENTRY chunks.
 *   has_more              — small LIMIT against an active channel sets
 *                           DATA_HISTORY_HAS_MORE = 1.
 *   before_pagination     — fetch one batch, take the oldest msgid,
 *                           refetch with BEFORE=that_id; every returned
 *                           message_id is strictly < cursor.
 *   after_catchup         — fetch newest msgid, send a fresh marker
 *                           chat, refetch with AFTER=that_id; find
 *                           our new marker and assert message_id >
 *                           the cursor.
 *
 * Cross-talk handling: the integration test binaries run in parallel,
 * and Janus's chat-history table is shared and persistent. Every test
 * tags its chat lines with a process-unique 64-bit random marker hex-
 * stringified into the body, then filters HISTORY_ENTRY chunks down
 * to entries containing the marker. That keeps assertions robust to
 * messages from other concurrent test processes (and to residue from
 * earlier runs that didn't reset the container).
 */

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <netinet/in.h>
#include <glib.h>
#include "compat.h"
#include "hotline.h"
#include "protocol.h"
#include "proto_helpers.h"
#include "chat_history.h"
#include "integration_harness.h"
#include "server_matrix.h"

/* chat_history.c is bundled into the integration_harness lib (which
 * gives this test the new hx_get_chat_history_build_chunks builder
 * for free, used by integration_send_get_chat_history). The harness
 * also provides a stubbed hlwrite_chunks (production's send path
 * needs network.c which we deliberately don't link). No per-test
 * stubs needed here anymore.
 *
 * hlwrite stays stubbed here in case any non-chat-history code path
 * pulls it transitively — it's the production async-write entry that
 * never makes sense in a Tier 3 binary. */
void
hlwrite (struct htlc_conn *htlc, guint32 type, guint32 flag, int hc, ...)
{
    (void) htlc;
    (void) type;
    (void) flag;
    (void) hc;
    g_assert_not_reached ();
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static guint32
hdr_type (const struct htlc_conn *htlc)
{
    const struct hl_hdr *h = (const struct hl_hdr *) htlc->in.buf;
    return ntohl (h->type);
}

static guint32
hdr_trans (const struct htlc_conn *htlc)
{
    const struct hl_hdr *h = (const struct hl_hdr *) htlc->in.buf;
    return ntohl (h->trans);
}

/* Pick the first chat-history-capable server in the matrix, or NULL
 * if none survived the GTKHX_TEST_SERVERS env filter. Caller skips
 * if NULL. */
static const hx_test_server *
pick_chat_history_server (void)
{
    GPtrArray *servers = hx_test_servers_with (HX_TEST_CAP_CHAT_HISTORY);
    if (!servers) {
        return NULL;
    }
    const hx_test_server *srv = NULL;
    if (servers->len > 0) {
        srv = g_ptr_array_index (servers, 0);
    }
    g_ptr_array_unref (servers);
    return srv;
}

/* Build a process-unique 17-byte marker like "HX-a1b2c3d4e5f60718".
 * High-entropy enough that we can substring-match for our own
 * messages in the history stream without colliding with other
 * concurrent tests (or with stale residue from previous runs of
 * this same test). */
static void
make_marker (char *out, gsize cap)
{
    guint64 r = (((guint64) g_random_int ()) << 32) ^ (guint64) g_random_int ();
    /* Also mix in the pid + wall-clock seconds to defend against
	 * extremely unlucky g_random_int reseeds across test binaries
	 * that started in the same second. */
    r ^= ((guint64) getpid () << 16) ^ (guint64) time (NULL);
    g_snprintf (out, cap, "HX-%016" G_GINT64_MODIFIER "x", r);
}

/* Send one chat line, mirroring tests/integration/test_chat_roundtrip
 * shape. Public chat (cid 0). */
static gboolean
send_chat_line (int fd, struct htlc_conn *htlc, const char *text)
{
    guint16 style = htons (1);
    return integration_send_message (
        fd, htlc, HTLC_HDR_CHAT, /*flag=*/0, /*hc=*/2,
        (int) HTLC_DATA_STYLE, (int) sizeof (style), &style,
        (int) HTLC_DATA_CHAT, (int) strlen (text), (guint8 *) text);
}

/* Drain the next HTLS_HDR_TASK reply that correlates to `wanted_trans`.
 * Returns TRUE with the message left in htlc->in; FALSE on timeout.
 * Walks past unrelated broadcasts (USER_CHANGE, CHAT, BANNER) and
 * TASK replies for other clients' transactions. */
static gboolean
drain_until_task_with_trans (int fd, struct htlc_conn *htlc, guint32 wanted_trans,
                             int max_messages)
{
    for (int i = 0; i < max_messages; i++) {
        if (!integration_recv_message (fd, htlc, /*timeout_ms=*/4000)) {
            return FALSE;
        }
        if (hdr_type (htlc) == HTLS_HDR_TASK
            && hdr_trans (htlc) == wanted_trans) {
            return TRUE;
        }
    }
    return FALSE;
}

/* Walk a HTLS_HDR_TASK reply (currently sitting in htlc->in) for
 * chat-history entries. Calls hx_history_entry_parse on each
 * HTLS_DATA_HISTORY_ENTRY chunk and adds the result to `out`.
 * Also writes the DATA_HISTORY_HAS_MORE u8 (if present) into
 * `*has_more_out`; defaults to FALSE when the chunk is absent.
 * Caller frees the returned entries via hx_history_entry_free in
 * the array's free_func.
 *
 * Returns the number of well-formed entries appended. */
static guint
walk_history_reply (struct htlc_conn *htlc, GPtrArray *out,
                    gboolean *has_more_out)
{
    guint added = 0;
    gboolean has_more = FALSE;
    dh_start (htlc)
    {
        if (_type == HTLS_DATA_HISTORY_ENTRY) {
            HxHistoryEntry *e = hx_history_entry_parse (dh->data, _len);
            if (e) {
                g_ptr_array_add (out, e);
                added++;
            }
        } else if (_type == HTLS_DATA_HISTORY_HAS_MORE && _len >= 1) {
            has_more = (dh->data[0] != 0);
        }
    }
    dh_end ();
    if (has_more_out) {
        *has_more_out = has_more;
    }
    return added;
}

/* Find the entry whose message body contains `marker`. Returns
 * a borrowed pointer into the array (caller does NOT free), or
 * NULL if no entry matched. */
static const HxHistoryEntry *
find_entry_by_marker (GPtrArray *entries, const char *marker)
{
    for (guint i = 0; i < entries->len; i++) {
        HxHistoryEntry *e = g_ptr_array_index (entries, i);
        if (!e || !e->message) {
            continue;
        }
        if (g_strstr_len (e->message, e->message_len, marker)) {
            return e;
        }
    }
    return NULL;
}

/* Find the oldest (smallest message_id) entry in the array.
 * Convenience for BEFORE-cursor pagination. */
static guint64
oldest_msgid (GPtrArray *entries)
{
    guint64 oldest = G_MAXUINT64;
    for (guint i = 0; i < entries->len; i++) {
        HxHistoryEntry *e = g_ptr_array_index (entries, i);
        if (e && e->message_id > 0 && e->message_id < oldest) {
            oldest = e->message_id;
        }
    }
    return (oldest == G_MAXUINT64) ? 0 : oldest;
}

/* Symmetric: newest entry. */
static guint64
newest_msgid (GPtrArray *entries)
{
    guint64 newest = 0;
    for (guint i = 0; i < entries->len; i++) {
        HxHistoryEntry *e = g_ptr_array_index (entries, i);
        if (e && e->message_id > newest) {
            newest = e->message_id;
        }
    }
    return newest;
}

/* Tear-down helper used by every subtest. */
static void
close_session (int fd, struct htlc_conn *htlc)
{
    integration_release_htlc (htlc);
    integration_close (fd);
}

/* ------------------------------------------------------------------ */
/* Test cases                                                          */
/* ------------------------------------------------------------------ */

/* Capability negotiation: client advertises CHAT_HISTORY; cap-aware
 * server echoes the bit in the LOGIN TASK reply, which the harness
 * captures into htlc->caps. */
static void
test_chat_history_cap_negotiation (void)
{
    const hx_test_server *srv = pick_chat_history_server ();
    if (!srv) {
        g_test_skip ("no chat-history-capable server in matrix "
                     "(GTKHX_TEST_SERVERS filter excluded all).");
        return;
    }

    struct htlc_conn htlc;
    int fd = integration_open_login_to_caps_or_skip (
        srv, &htlc, "CapNeg Tier-3", 412, HTLC_CAP_CHAT_HISTORY);
    if (fd < 0) {
        return;
    }

    /* The drain helper stashed any DATA_CAPABILITIES echo it saw
	 * into htlc->caps. Janus / spec-compliant servers reply with
	 * just the bits they support; we asked for chat-history and
	 * the server says it has it, so the bit must be lit. */
    g_assert_cmphex ((htlc.caps & HTLC_CAP_CHAT_HISTORY), ==,
                     HTLC_CAP_CHAT_HISTORY);

    close_session (fd, &htlc);
}

/* End-to-end round-trip: send a marker chat, fetch via TRAN 700,
 * find our message coming back in the history stream. Pins down
 * the basic "server stored my chat" contract. */
static void
test_chat_history_round_trip (void)
{
    const hx_test_server *srv = pick_chat_history_server ();
    if (!srv) {
        g_test_skip ("no chat-history-capable server in matrix.");
        return;
    }

    struct htlc_conn htlc;
    int fd = integration_open_login_to_caps_or_skip (
        srv, &htlc, "RoundTrip Tier-3", 412, HTLC_CAP_CHAT_HISTORY);
    if (fd < 0) {
        return;
    }
    g_assert_cmphex ((htlc.caps & HTLC_CAP_CHAT_HISTORY), ==,
                     HTLC_CAP_CHAT_HISTORY);

    char marker[32];
    make_marker (marker, sizeof (marker));
    gchar *line = g_strdup_printf ("integration %s body", marker);
    g_assert_true (send_chat_line (fd, &htlc, line));

    /* Give the server a beat to persist the row before we fetch.
	 * Janus is SQLite-backed and the INSERT lands inside the same
	 * goroutine that received the chat, so this is normally fast,
	 * but the integration suite cross-talks heavily — a short
	 * sleep dodges any flake from "TRAN 700 ran before our row
	 * committed." */
    g_usleep (200000); /* 200 ms */

    guint32 trans = integration_send_get_chat_history (
        fd, &htlc, HX_HISTORY_CHANNEL_PUBLIC,
        /*before=*/0, /*after=*/0, /*limit=*/50);
    g_assert_cmpuint (trans, !=, 0);

    g_assert_true (drain_until_task_with_trans (fd, &htlc, trans, 32));

    GPtrArray *entries = g_ptr_array_new_with_free_func (
        (GDestroyNotify) hx_history_entry_free);
    gboolean has_more = FALSE;
    guint n = walk_history_reply (&htlc, entries, &has_more);
    (void) has_more;
    g_assert_cmpuint (n, >, 0);

    const HxHistoryEntry *mine = find_entry_by_marker (entries, marker);
    g_assert_nonnull (mine);
    /* The server stores our raw send body; Hotline doesn't reformat
	 * chat into the broadcast template before persisting it. So we
	 * expect to see exactly the line we sent. */
    g_assert_nonnull (g_strstr_len (mine->message, mine->message_len, marker));
    g_assert_cmpstr (mine->nick, ==, "RoundTrip Tier-3");
    /* message_id is server-assigned and monotonic; non-zero is the
	 * only spec guarantee. */
    g_assert_cmpuint (mine->message_id, >, 0);

    g_ptr_array_unref (entries);
    g_free (line);
    close_session (fd, &htlc);
}

/* LIMIT respects the cap: ask for a small N, verify the reply
 * carries at most N entries. The "at most" wording is per spec —
 * the server may return fewer (e.g. if it has fewer total). */
static void
test_chat_history_limit (void)
{
    const hx_test_server *srv = pick_chat_history_server ();
    if (!srv) {
        g_test_skip ("no chat-history-capable server in matrix.");
        return;
    }

    struct htlc_conn htlc;
    int fd = integration_open_login_to_caps_or_skip (
        srv, &htlc, "Limit Tier-3", 412, HTLC_CAP_CHAT_HISTORY);
    if (fd < 0) {
        return;
    }

    /* Seed five messages so any sane LIMIT≥5 actually has something
	 * to return — without this, a freshly-reset container would
	 * legitimately yield zero rows and the cap assertion would be
	 * trivially satisfied. */
    char marker[32];
    make_marker (marker, sizeof (marker));
    for (int i = 0; i < 5; i++) {
        gchar *line = g_strdup_printf ("%s seed %d", marker, i);
        g_assert_true (send_chat_line (fd, &htlc, line));
        g_free (line);
    }
    g_usleep (200000);

    const guint16 wanted = 3;
    guint32 trans = integration_send_get_chat_history (
        fd, &htlc, HX_HISTORY_CHANNEL_PUBLIC, /*before=*/0, /*after=*/0,
        /*limit=*/wanted);
    g_assert_cmpuint (trans, !=, 0);

    g_assert_true (drain_until_task_with_trans (fd, &htlc, trans, 32));

    GPtrArray *entries = g_ptr_array_new_with_free_func (
        (GDestroyNotify) hx_history_entry_free);
    gboolean has_more = FALSE;
    guint n = walk_history_reply (&htlc, entries, &has_more);
    g_assert_cmpuint (n, <=, wanted);

    g_ptr_array_unref (entries);
    close_session (fd, &htlc);
}

/* has_more semantics: enough messages in the channel that a small
 * LIMIT can't return all of them. Server must set the HAS_MORE flag.
 * We seed enough messages to be sure of "more than wanted" — but
 * because Janus's chat-history persists, on second runs the channel
 * almost certainly already had plenty; the seed is belt-and-braces. */
static void
test_chat_history_has_more (void)
{
    const hx_test_server *srv = pick_chat_history_server ();
    if (!srv) {
        g_test_skip ("no chat-history-capable server in matrix.");
        return;
    }

    struct htlc_conn htlc;
    int fd = integration_open_login_to_caps_or_skip (
        srv, &htlc, "HasMore Tier-3", 412, HTLC_CAP_CHAT_HISTORY);
    if (fd < 0) {
        return;
    }

    char marker[32];
    make_marker (marker, sizeof (marker));
    for (int i = 0; i < 6; i++) {
        gchar *line = g_strdup_printf ("%s pad %d", marker, i);
        g_assert_true (send_chat_line (fd, &htlc, line));
        g_free (line);
    }
    g_usleep (250000);

    guint32 trans = integration_send_get_chat_history (
        fd, &htlc, HX_HISTORY_CHANNEL_PUBLIC, /*before=*/0, /*after=*/0,
        /*limit=*/2);
    g_assert_cmpuint (trans, !=, 0);
    g_assert_true (drain_until_task_with_trans (fd, &htlc, trans, 32));

    GPtrArray *entries = g_ptr_array_new_with_free_func (
        (GDestroyNotify) hx_history_entry_free);
    gboolean has_more = FALSE;
    walk_history_reply (&htlc, entries, &has_more);
    g_assert_true (has_more);

    g_ptr_array_unref (entries);
    close_session (fd, &htlc);
}

/* BEFORE pagination: fetch one batch, pick the oldest msgid, refetch
 * with BEFORE=that_id. Every returned entry must have message_id
 * strictly less than the cursor. */
static void
test_chat_history_before_pagination (void)
{
    const hx_test_server *srv = pick_chat_history_server ();
    if (!srv) {
        g_test_skip ("no chat-history-capable server in matrix.");
        return;
    }

    struct htlc_conn htlc;
    int fd = integration_open_login_to_caps_or_skip (
        srv, &htlc, "Before Tier-3", 412, HTLC_CAP_CHAT_HISTORY);
    if (fd < 0) {
        return;
    }

    /* First batch — recent N. */
    guint32 t1 = integration_send_get_chat_history (
        fd, &htlc, HX_HISTORY_CHANNEL_PUBLIC, 0, 0, /*limit=*/10);
    g_assert_cmpuint (t1, !=, 0);
    g_assert_true (drain_until_task_with_trans (fd, &htlc, t1, 32));

    GPtrArray *first = g_ptr_array_new_with_free_func (
        (GDestroyNotify) hx_history_entry_free);
    gboolean has_more1 = FALSE;
    guint n1 = walk_history_reply (&htlc, first, &has_more1);
    if (n1 < 2) {
        /* Channel has fewer than two persisted messages — can't
		 * meaningfully exercise pagination. Skip silently rather
		 * than asserting; the round_trip / limit tests already
		 * cover the populated-channel happy path. */
        g_test_skip ("channel has too few persisted messages for "
                     "BEFORE-cursor pagination.");
        g_ptr_array_unref (first);
        close_session (fd, &htlc);
        return;
    }

    guint64 cursor = oldest_msgid (first);
    g_assert_cmpuint (cursor, >, 0);

    /* Second batch — strictly older than cursor. */
    guint32 t2 = integration_send_get_chat_history (
        fd, &htlc, HX_HISTORY_CHANNEL_PUBLIC, /*before=*/cursor,
        /*after=*/0, /*limit=*/10);
    g_assert_cmpuint (t2, !=, 0);
    g_assert_true (drain_until_task_with_trans (fd, &htlc, t2, 32));

    GPtrArray *second = g_ptr_array_new_with_free_func (
        (GDestroyNotify) hx_history_entry_free);
    gboolean has_more2 = FALSE;
    guint n2 = walk_history_reply (&htlc, second, &has_more2);
    (void) has_more2;

    /* Spec: BEFORE returns entries with message_id < cursor. Zero
	 * results is legitimate (cursor was the oldest in the table). */
    for (guint i = 0; i < n2; i++) {
        HxHistoryEntry *e = g_ptr_array_index (second, i);
        g_assert_cmpuint (e->message_id, <, cursor);
    }

    g_ptr_array_unref (first);
    g_ptr_array_unref (second);
    close_session (fd, &htlc);
}

/* AFTER catch-up: fetch the newest msgid, send a fresh marker chat,
 * refetch with AFTER=that_id. Our new message must appear with a
 * larger message_id. This is the in-session reconnect flow that the
 * production client uses post-login to surface anything new since
 * disconnect (src/chat_history Phase 4). */
static void
test_chat_history_after_catchup (void)
{
    const hx_test_server *srv = pick_chat_history_server ();
    if (!srv) {
        g_test_skip ("no chat-history-capable server in matrix.");
        return;
    }

    struct htlc_conn htlc;
    int fd = integration_open_login_to_caps_or_skip (
        srv, &htlc, "After Tier-3", 412, HTLC_CAP_CHAT_HISTORY);
    if (fd < 0) {
        return;
    }

    /* Establish the cursor from whatever's currently newest. */
    guint32 t1 = integration_send_get_chat_history (
        fd, &htlc, HX_HISTORY_CHANNEL_PUBLIC, 0, 0, /*limit=*/5);
    g_assert_cmpuint (t1, !=, 0);
    g_assert_true (drain_until_task_with_trans (fd, &htlc, t1, 32));

    GPtrArray *prefix = g_ptr_array_new_with_free_func (
        (GDestroyNotify) hx_history_entry_free);
    gboolean has_more = FALSE;
    walk_history_reply (&htlc, prefix, &has_more);
    guint64 cursor = newest_msgid (prefix);
    g_ptr_array_unref (prefix);
    if (cursor == 0) {
        /* Brand-new channel with no history at all — fetching with
		 * AFTER=0 is the same as fetching with no cursor and would
		 * trip the "give me everything" path. Skip gracefully; the
		 * round_trip / limit tests cover the empty-channel happy
		 * path well enough. */
        g_test_skip ("channel has no persisted messages to anchor "
                     "an AFTER cursor against.");
        close_session (fd, &htlc);
        return;
    }

    /* Send the marker we want to appear in the AFTER batch. */
    char marker[32];
    make_marker (marker, sizeof (marker));
    gchar *line = g_strdup_printf ("catchup %s body", marker);
    g_assert_true (send_chat_line (fd, &htlc, line));
    g_free (line);
    g_usleep (250000);

    /* Catch-up fetch — everything newer than cursor. */
    guint32 t2 = integration_send_get_chat_history (
        fd, &htlc, HX_HISTORY_CHANNEL_PUBLIC, /*before=*/0,
        /*after=*/cursor, /*limit=*/50);
    g_assert_cmpuint (t2, !=, 0);
    g_assert_true (drain_until_task_with_trans (fd, &htlc, t2, 32));

    GPtrArray *catchup = g_ptr_array_new_with_free_func (
        (GDestroyNotify) hx_history_entry_free);
    has_more = FALSE;
    guint n = walk_history_reply (&htlc, catchup, &has_more);
    g_assert_cmpuint (n, >, 0);
    const HxHistoryEntry *mine = find_entry_by_marker (catchup, marker);
    g_assert_nonnull (mine);
    g_assert_cmpuint (mine->message_id, >, cursor);

    /* All entries must satisfy the AFTER predicate. */
    for (guint i = 0; i < n; i++) {
        HxHistoryEntry *e = g_ptr_array_index (catchup, i);
        g_assert_cmpuint (e->message_id, >, cursor);
    }

    g_ptr_array_unref (catchup);
    close_session (fd, &htlc);
}

/* ------------------------------------------------------------------ */
int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/integration/chat_history/cap_negotiation",
                     test_chat_history_cap_negotiation);
    g_test_add_func ("/integration/chat_history/round_trip",
                     test_chat_history_round_trip);
    g_test_add_func ("/integration/chat_history/limit",
                     test_chat_history_limit);
    g_test_add_func ("/integration/chat_history/has_more",
                     test_chat_history_has_more);
    g_test_add_func ("/integration/chat_history/before_pagination",
                     test_chat_history_before_pagination);
    g_test_add_func ("/integration/chat_history/after_catchup",
                     test_chat_history_after_catchup);

    return g_test_run ();
}
