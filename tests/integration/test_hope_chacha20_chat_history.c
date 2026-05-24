/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_hope_chacha20_chat_history.c — Tier 3
 * end-to-end test for the fogWraith chat-history extension under
 * HOPE-Secure-Login + ChaCha20-Poly1305 AEAD framing.
 *
 * Exercises the same TRAN 700 / DATA_HISTORY_ENTRY round-trip the
 * plain test_chat_history.c covers, but with every byte on the wire
 * sealed/opened through cipher_aead_seal / cipher_aead_open after
 * the HOPE Step 2 handshake completes. Catches regressions where:
 *
 *   - the HTLC_HDR_GET_CHAT_HISTORY send path skips AEAD framing
 *     (cleartext write into an AEAD-only stream → server can't
 *     decrypt, response never arrives, test times out)
 *   - the HTLS_HDR_TASK reply with HISTORY_ENTRY chunks doesn't
 *     decode correctly under AEAD-aware recv
 *   - chat sends used to seed history bypass AEAD
 *   - the chat-history cap bit doesn't make it through the HOPE
 *     Step 2 LOGIN's capability advertisement
 *
 * Today only Janus advertises HX_TEST_CAP_CHAT_HISTORY AND
 * HX_TEST_CAP_CHACHA20, so the test filters the matrix on both and
 * skips silently when neither is in scope.
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

/* hlwrite stub mirrors test_chat_history.c — chat_history.c is
 * linked into the integration_harness lib and any code path that
 * unexpectedly reaches into production's async-write entry should
 * fail loudly. */
void
hlwrite (struct htlc_conn *htlc, guint32 type, guint32 flag, int hc, ...)
{
    (void) htlc;
    (void) type;
    (void) flag;
    (void) hc;
    g_assert_not_reached ();
}

static const hx_test_server *
pick_chat_history_chacha20_server (void)
{
    GPtrArray *servers = hx_test_servers_with (HX_TEST_CAP_CHAT_HISTORY
                                               | HX_TEST_CAP_CHACHA20);
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

/* Process-unique marker — same shape as test_chat_history.c so the
 * cross-talk argument matches. */
static void
make_marker (char *out, gsize cap)
{
    guint64 r = (((guint64) g_random_int ()) << 32) ^ (guint64) g_random_int ();
    r ^= ((guint64) getpid () << 16) ^ (guint64) time (NULL);
    g_snprintf (out, cap, "HX-%016" G_GINT64_MODIFIER "x", r);
}

/* Walk a HISTORY task reply for HISTORY_ENTRY chunks. Caller frees
 * via hx_history_entry_free in the array's free_func. */
static guint
walk_history_reply (struct htlc_conn *htlc, GPtrArray *out)
{
    guint added = 0;
    dh_start (htlc)
    {
        if (_type == HTLS_DATA_HISTORY_ENTRY) {
            HxHistoryEntry *e = hx_history_entry_parse (dh->data, _len);
            if (e) {
                g_ptr_array_add (out, e);
                added++;
            }
        }
    }
    dh_end ();
    return added;
}

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

/* HOPE-aware chat send. Builds the same 2-chunk HTLC_HDR_CHAT
 * production sends (STYLE + CHAT) and routes through
 * integration_send_message_hope so it goes through cipher_aead_seal
 * when AEAD is active. */
static gboolean
send_chat_line_hope (int fd, struct htlc_conn *htlc,
                     integration_hope_session *hope, const char *text)
{
    guint16 style = htons (1);
    return integration_send_message_hope (
        fd, htlc, hope, HTLC_HDR_CHAT, /*flag=*/0, /*hc=*/2,
        (int) HTLC_DATA_STYLE, (int) sizeof (style), &style,
        (int) HTLC_DATA_CHAT, (int) strlen (text), (guint8 *) text);
}

/* Drain to the TASK reply matching our trans, AEAD-aware. */
static gboolean
drain_until_task_hope (int fd, struct htlc_conn *htlc,
                       integration_hope_session *hope, guint32 wanted_trans,
                       int max_messages)
{
    for (int i = 0; i < max_messages; i++) {
        if (!integration_recv_message_hope (fd, htlc, hope,
                                            /*timeout_ms=*/4000)) {
            return FALSE;
        }
        if (hdr_type (htlc) == HTLS_HDR_TASK
            && hdr_trans (htlc) == wanted_trans) {
            return TRUE;
        }
    }
    return FALSE;
}

static void
test_hope_chacha20_chat_history_round_trip (void)
{
    const hx_test_server *srv = pick_chat_history_chacha20_server ();
    if (!srv) {
        g_test_fail_printf ("no server in matrix advertising both "
                     "HX_TEST_CAP_CHAT_HISTORY and HX_TEST_CAP_CHACHA20. "
                     "Janus (tests/janus/) advertises both — bring it up "
                     "with `docker run -p 5510:5500 -p 5511:5501 "
                     "gtkhx-janus`.");
        return;
    }

    struct htlc_conn htlc;
    integration_hope_session hope;
    int fd = integration_open_login_hope_or_skip (
        srv, &htlc, &hope,
        /*username=*/"guest", /*password=*/"",
        /*display_name=*/"HopeChaChaHistory Tier-3",
        /*icon=*/412,
        /*cipheralg=*/"CHACHA20-POLY1305",
        /*compressalg=*/NULL);
    if (fd < 0) {
        return;
    }
    g_assert_true (hope.aead_active);

    /* HOPE Step 2 advertises HTLC_CAP_CHAT_HISTORY (see
     * integration_open_login_hope_or_skip), so the server's echo
     * should round-trip into htlc->caps. */
    g_assert_cmphex ((htlc.caps & HTLC_CAP_CHAT_HISTORY), ==,
                     HTLC_CAP_CHAT_HISTORY);

    /* Seed a marker line through the AEAD-encrypted control channel,
     * then immediately fetch via TRAN 700. */
    char marker[32];
    make_marker (marker, sizeof (marker));
    gchar *line = g_strdup_printf ("integration %s body", marker);
    g_assert_true (send_chat_line_hope (fd, &htlc, &hope, line));

    /* Same 200ms beat as test_chat_history.c — give Janus's SQLite
     * INSERT time to commit before the SELECT. */
    g_usleep (200000);

    guint32 trans = integration_send_get_chat_history_hope (
        fd, &htlc, &hope, HX_HISTORY_CHANNEL_PUBLIC,
        /*before=*/0, /*after=*/0, /*limit=*/50);
    g_assert_cmpuint (trans, !=, 0);

    g_assert_true (drain_until_task_hope (fd, &htlc, &hope, trans, 32));

    GPtrArray *entries = g_ptr_array_new_with_free_func (
        (GDestroyNotify) hx_history_entry_free);
    guint n = walk_history_reply (&htlc, entries);
    g_assert_cmpuint (n, >, 0);

    const HxHistoryEntry *mine = find_entry_by_marker (entries, marker);
    g_assert_nonnull (mine);
    g_assert_nonnull (g_strstr_len (mine->message, mine->message_len, marker));
    g_assert_cmpstr (mine->nick, ==, "HopeChaChaHistory Tier-3");
    g_assert_cmpuint (mine->message_id, >, 0);

    g_ptr_array_unref (entries);
    g_free (line);
    integration_release_htlc (&htlc);
    integration_hope_session_release (&hope);
    integration_close (fd);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/integration/hope_chacha20/chat_history/round_trip",
                     test_hope_chacha20_chat_history_round_trip);
    return g_test_run ();
}
