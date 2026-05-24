/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_hope_rc4_chat_history.c — Tier 3 end-to-end
 * test for the fogWraith chat-history extension under HOPE+RC4
 * stream-cipher framing.
 *
 * Sister to test_hope_chacha20_chat_history (AEAD-framed) and
 * test_hope_rc4 (stream login + PING). What this one proves:
 * cipher_encode on the outgoing TRAN 700 plus cipher_decode on the
 * incoming HISTORY_ENTRY frames stay in sync with a stream cipher in
 * play. The legacy 3/16-probability rekey-marker rotation is exercised
 * separately by test_hope_rc4.c (32 pings under the same cipher
 * dispatcher, with decode_rekey_count > 0 asserted) — this test
 * deliberately keeps a minimal send sequence and does not assert on
 * the rekey path.
 *
 * Matrix filter requires HX_TEST_CAP_CHAT_HISTORY AND HX_TEST_CAP_RC4.
 * No matrix entry today advertises both — Janus has chat-history but
 * only ChaCha20; mhxd has RC4 but no chat-history. The test exists so
 * future matrix additions (Mobius with chat-history + a stream
 * cipher, or a mhxd-with-chat-history patch) get coverage
 * automatically. Until then it skips cleanly with a diagnostic.
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
pick_chat_history_rc4_server (void)
{
    GPtrArray *servers = hx_test_servers_with (HX_TEST_CAP_CHAT_HISTORY
                                               | HX_TEST_CAP_RC4);
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
    guint64 r = (((guint64) g_random_int ()) << 32) ^ (guint64) g_random_int ();
    r ^= ((guint64) getpid () << 16) ^ (guint64) time (NULL);
    g_snprintf (out, cap, "HX-%016" G_GINT64_MODIFIER "x", r);
}

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
test_hope_rc4_chat_history_round_trip (void)
{
    const hx_test_server *srv = pick_chat_history_rc4_server ();
    if (!srv) {
        g_test_fail_printf ("no server in matrix advertising both "
                     "HX_TEST_CAP_CHAT_HISTORY and HX_TEST_CAP_RC4. "
                     "Janus has chat-history but only ChaCha20; mhxd "
                     "has RC4 but no chat-history. Add a matrix entry "
                     "combining both and this test will fire.");
        return;
    }

    struct htlc_conn htlc;
    integration_hope_session hope;
    int fd = integration_open_login_hope_or_skip (
        srv, &htlc, &hope,
        /*username=*/"guest", /*password=*/"",
        /*display_name=*/"HopeRC4History Tier-3",
        /*icon=*/412,
        /*cipheralg=*/"RC4",
        /*compressalg=*/NULL);
    if (fd < 0) {
        return;
    }
    g_assert_true (hope.stream_active);
    g_assert_false (hope.aead_active);
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
        (GDestroyNotify) hx_history_entry_free);
    guint n = walk_history_reply (&htlc, entries);
    g_assert_cmpuint (n, >, 0);

    const HxHistoryEntry *mine = find_entry_by_marker (entries, marker);
    g_assert_nonnull (mine);
    g_assert_nonnull (g_strstr_len (mine->message, mine->message_len, marker));
    g_assert_cmpstr (mine->nick, ==, "HopeRC4History Tier-3");
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
    g_test_add_func ("/integration/hope_rc4/chat_history/round_trip",
                     test_hope_rc4_chat_history_round_trip);
    return g_test_run ();
}
