/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_voice_implicit_leave.c — Tier 3 coverage
 * for the spec's "joining B implicitly leaves A" rule (Phase 8.F).
 *
 * Per fogWraith Capabilities-Voice.md, a client may only be in one
 * voice room at a time. Sending VOICE_JOIN(B) while already in room A
 * implicitly leaves A — the server tears down the previous session
 * and replies to the new JOIN with a fresh SDP offer for B.
 *
 * Wire contract under test (single-room variant — the public chat
 * is the only room the test suite has reliable access to, so we
 * exercise the duplicate-JOIN path on cid=0 and check the server
 * either:
 *   (a) re-issues a fresh SDP offer for the second JOIN (success
 *       path — server treated the second JOIN as an implicit
 *       leave + rejoin), or
 *   (b) replies with a clean task-error explaining the duplicate
 *       (also acceptable — some server implementations of the
 *       implicit-leave rule require the rooms to differ).
 *
 * EITHER outcome is a passing wire round-trip. The regression we're
 * catching is the server dropping the connection, hanging, or
 * sending an uncorrelated frame.
 *
 * The follow-up VOICE_LEAVE(0) must succeed cleanly, witnessing
 * that the server still treats us as a participant after the
 * duplicate-JOIN dance.
 *
 * Server gating: HX_TEST_CAP_VOICE → Janus today.
 */

#include "config.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <glib.h>
#include "compat.h"
#include "hotline.h"
#include "protocol.h"
#include "proto_helpers.h"
#include "hotline_proto.h"
#include "integration_harness.h"
#include "server_matrix.h"

static const hx_test_server *
pick_voice_server (void)
{
    GPtrArray *servers = hx_test_servers_with (HX_TEST_CAP_VOICE);
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

static guint32
send_voice_join (int fd, struct htlc_conn *htlc, guint32 cid)
{
    guint32 cid_be = htonl (cid);
    guint32 trans = htlc->trans;
    if (!integration_send_message (
            fd, htlc, HTLC_HDR_VOICE_JOIN, /*flag=*/0, /*hc=*/1,
            (int) HTLC_DATA_CHAT_ID, (int) sizeof (cid_be), &cid_be)) {
        return 0;
    }
    return trans;
}

static guint32
send_voice_leave (int fd, struct htlc_conn *htlc, guint32 cid)
{
    guint32 cid_be = htonl (cid);
    guint32 trans = htlc->trans;
    if (!integration_send_message (
            fd, htlc, HTLC_HDR_VOICE_LEAVE, /*flag=*/0, /*hc=*/1,
            (int) HTLC_DATA_CHAT_ID, (int) sizeof (cid_be), &cid_be)) {
        return 0;
    }
    return trans;
}

static void
test_voice_implicit_leave (void)
{
    const hx_test_server *srv = pick_voice_server ();
    if (!srv) {
        g_test_fail_printf ("no voice-capable server in matrix.");
        return;
    }

    char nick[32];
    g_snprintf (nick, sizeof (nick), "VoiceImpl-%d-%04x", (int) getpid (),
                g_random_int () & 0xffff);

    struct htlc_conn htlc;
    int fd = integration_open_login_to_caps_or_skip (
        srv, &htlc, nick, 412, HTLC_CAP_VOICE);
    if (fd < 0) {
        return;
    }
    g_assert_true (integration_send_agreementagree_hope (
        fd, &htlc, /*hope=*/NULL, nick, 412));

    /* First JOIN — must succeed. */
    guint32 j1 = send_voice_join (fd, &htlc, 0);
    g_assert_cmpuint (j1, !=, 0);
    g_assert_true (integration_drain_until_task_trans (fd, &htlc, j1, 64));
    g_assert_cmphex (hdr_flag (&htlc) & 1, ==, 0);

    /* Second JOIN against the same room — outcome may vary, but
     * the wire round-trip must complete. */
    guint32 j2 = send_voice_join (fd, &htlc, 0);
    g_assert_cmpuint (j2, !=, 0);
    g_assert_true (integration_drain_until_task_trans (fd, &htlc, j2, 64));
    if (hdr_flag (&htlc) & 1) {
        /* Server rejected the duplicate JOIN — acceptable. Capture
         * the error message in the test log. */
        char err[256] = { 0 };
        gsize err_len = 0;
        if (task_error_extract (&htlc, err, sizeof (err), &err_len)) {
            g_test_message ("duplicate VOICE_JOIN(0) rejected: \"%s\"", err);
        }
    } else {
        /* Server treated as implicit leave + rejoin and gave us a
         * fresh SDP offer. Validate the reply shape. */
        struct gtkhx_proto_voice_reply r;
        memset (&r, 0, sizeof (r));
        gtkhx_proto_parse_voice_reply (htlc.in.buf, htlc.in.pos, &r);
        g_assert_true (r.sdp_present);
        g_assert_true (r.codec_present);
        g_assert_true (r.participants_present);
        g_test_message ("duplicate VOICE_JOIN(0) accepted as implicit "
                        "leave+rejoin, fresh SDP offer received.");
    }

    /* LEAVE must succeed regardless of which branch the duplicate
     * JOIN took. If the server tore down our session via the
     * implicit leave path, the second JOIN's success above put us
     * back in the room; if the duplicate JOIN failed, we're still
     * in the room from the first JOIN. Either way LEAVE is valid. */
    guint32 l1 = send_voice_leave (fd, &htlc, 0);
    g_assert_cmpuint (l1, !=, 0);
    g_assert_true (integration_drain_until_task_trans (fd, &htlc, l1, 32));
    g_assert_cmphex (hdr_flag (&htlc) & 1, ==, 0);

    integration_release_htlc (&htlc);
    integration_close (fd);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/integration/voice/implicit_leave",
                     test_voice_implicit_leave);

    return g_test_run ();
}
