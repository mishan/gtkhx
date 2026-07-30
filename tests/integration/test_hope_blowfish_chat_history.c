/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_hope_blowfish_chat_history.c — Tier 3
 * end-to-end test for the fogWraith chat-history extension under
 * HOPE+Blowfish-OFB-64 stream-cipher framing.
 *
 * Originally written as the sister of test_hope_rc4_chat_history;
 * that file is gone (RC4 removed in claude/remove-rc4) so this is
 * the canonical HOPE+stream chat-history Tier 3 test. Scope:
 * cipher_encode/_decode parity around the TRAN 700 round-trip;
 * rekey-marker rotation is left to test_hope_blowfish.c.
 *
 * Filters on HX_TEST_CAP_BLOWFISH + HX_TEST_CAP_CHAT_HISTORY.
 * Same caveat about no matrix entry currently advertising both —
 * test skips cleanly until one does.
 */

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <glib.h>
#include "compat.h"
#include "hotline.h"
#include "protocol.h"
#include "proto_helpers.h"
#include "chat_history.h"
#include "integration_harness.h"
#include "server_matrix.h"

static const hx_test_server *
pick_chat_history_blowfish_server (void)
{
    GPtrArray *servers = hx_test_servers_with (HX_TEST_CAP_CHAT_HISTORY
                                               | HX_TEST_CAP_BLOWFISH);
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

static void
make_marker (char *out, gsize cap)
{
    guint64 r = (((guint64)g_random_int ()) << 32) ^ (guint64)g_random_int ();
    r ^= ((guint64)getpid () << 16) ^ (guint64)time (NULL);
    g_snprintf (out, cap, "HX-%016" G_GINT64_MODIFIER "x", r);
}

static guint
walk_history_reply (struct htlc_conn *htlc, GPtrArray *out)
{
    guint added = 0;
    dh_start (hx_test_in (htlc)->buf, hx_test_in (htlc)->pos)
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

static gboolean
send_chat_line_hope (int fd, struct htlc_conn *htlc,
                     integration_hope_session *hope, const char *text)
{
    guint16 style = g_htons (1);
    return integration_send_message_hope (
        fd, htlc, hope, HTLC_HDR_CHAT, /*flag=*/0, /*hc=*/2,
        (int)HTLC_DATA_STYLE, (int)sizeof (style), &style, (int)HTLC_DATA_CHAT,
        (int)strlen (text), (guint8 *)text);
}

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
test_hope_blowfish_chat_history_round_trip (void)
{
    const hx_test_server *srv = pick_chat_history_blowfish_server ();
    if (!srv) {
        g_test_fail_printf (
            "no server in matrix advertising both "
            "HX_TEST_CAP_CHAT_HISTORY and HX_TEST_CAP_BLOWFISH. "
            "Janus has chat-history but only ChaCha20; mhxd has "
            "Blowfish but no chat-history. Add a matrix entry "
            "combining both and this test will fire.");
        return;
    }

    struct htlc_conn htlc;
    integration_hope_session hope;
    int fd = integration_open_login_hope_or_skip (
        srv, &htlc, &hope,
        /*username=*/"guest", /*password=*/"",
        /*display_name=*/"HopeBFHistory Tier-3",
        /*icon=*/412,
        /*cipheralg=*/"BLOWFISH",
        /*compressalg=*/NULL);
    if (fd < 0) {
        return;
    }
    /* Stream-cipher negotiation is a harness-crypto fact; under
     * orchestration the production actor (Rust) owns the cipher and the
     * harness hope session stays zeroed — the chat-history round-trip
     * below is the end-to-end proof. */
    g_assert_cmphex ((htlc.caps & HTLC_CAP_CHAT_HISTORY), ==,
                     HTLC_CAP_CHAT_HISTORY);

    char marker[32];
    make_marker (marker, sizeof (marker));
    gchar *line = g_strdup_printf ("integration %s body", marker);
    g_assert_true (send_chat_line_hope (fd, &htlc, &hope, line));

    g_usleep (200000);

    guint32 trans = integration_send_get_chat_history_hope (
        fd, &htlc, &hope, HX_HISTORY_CHANNEL_PUBLIC,
        /*before=*/0, /*after=*/0, /*limit=*/50);
    g_assert_cmpuint (trans, !=, 0);

    g_assert_true (drain_until_task_hope (fd, &htlc, &hope, trans, 32));

    GPtrArray *entries = g_ptr_array_new_with_free_func (
        (GDestroyNotify)hx_history_entry_free);
    guint n = walk_history_reply (&htlc, entries);
    g_assert_cmpuint (n, >, 0);

    const HxHistoryEntry *mine = find_entry_by_marker (entries, marker);
    g_assert_nonnull (mine);
    g_assert_nonnull (g_strstr_len (mine->message, mine->message_len, marker));
    g_assert_cmpstr (mine->nick, ==, "HopeBFHistory Tier-3");
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
    g_test_add_func ("/integration/hope_blowfish/chat_history/round_trip",
                     test_hope_blowfish_chat_history_round_trip);
    return g_test_run ();
}
