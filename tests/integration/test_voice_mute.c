/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_voice_mute.c — Tier 3 coverage for the
 * VOICE_MUTE (606) round-trip (Phase 8.F).
 *
 * Wire contract under test:
 *   1. VOICE_JOIN(0).
 *   2. VOICE_MUTE(cid=0, muted=1) → TASK ack matching trans,
 *      non-error.
 *   3. VOICE_MUTE(cid=0, muted=0) → TASK ack, non-error.
 *   4. VOICE_LEAVE(0) → TASK ack, non-error.
 *
 * Mute is server-side enforced per the spec (the server gates
 * RTP forwarding from a muted participant), but the wire shape
 * we're catching regressions on is straightforward: an empty-body
 * TASK ack correlated by trans. PTT is just rapid mute/unmute from
 * the client's side, so coverage here pins down the round-trip
 * the PTT feature will build on.
 *
 * Server gating: HX_TEST_CAP_VOICE → Janus today.
 */

#include "config.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <glib.h>
#include "compat.h"
#include "hotline.h"
#include "protocol.h"
#include "proto_helpers.h"
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
    guint32 cid_be = g_htonl(cid);
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
    guint32 cid_be = g_htonl(cid);
    guint32 trans = htlc->trans;
    if (!integration_send_message (
            fd, htlc, HTLC_HDR_VOICE_LEAVE, /*flag=*/0, /*hc=*/1,
            (int) HTLC_DATA_CHAT_ID, (int) sizeof (cid_be), &cid_be)) {
        return 0;
    }
    return trans;
}

/* HTLC_HDR_VOICE_MUTE: CHAT_ID + VOICE_MUTED (u16 BE, 0 or 1). */
static guint32
send_voice_mute (int fd, struct htlc_conn *htlc, guint32 cid, guint16 muted)
{
    guint32 cid_be = g_htonl(cid);
    guint16 muted_be = g_htons(muted);
    guint32 trans = htlc->trans;
    if (!integration_send_message (
            fd, htlc, HTLC_HDR_VOICE_MUTE, /*flag=*/0, /*hc=*/2,
            (int) HTLC_DATA_CHAT_ID, (int) sizeof (cid_be), &cid_be,
            (int) HTLC_DATA_VOICE_MUTED, (int) sizeof (muted_be), &muted_be)) {
        return 0;
    }
    return trans;
}

static void
test_voice_mute_round_trip (void)
{
    const hx_test_server *srv = pick_voice_server ();
    if (!srv) {
        g_test_fail_printf ("no voice-capable server in matrix.");
        return;
    }

    char nick[32];
    g_snprintf (nick, sizeof (nick), "VoiceMute-%d-%04x", (int) getpid (),
                g_random_int () & 0xffff);

    struct htlc_conn htlc;
    int fd = integration_open_login_to_caps_or_skip (
        srv, &htlc, nick, 412, HTLC_CAP_VOICE);
    if (fd < 0) {
        return;
    }
    g_assert_true (integration_send_agreementagree_hope (
        fd, &htlc, /*hope=*/NULL, nick, 412));
    g_assert_cmphex ((htlc.caps & HTLC_CAP_VOICE), ==, HTLC_CAP_VOICE);

    guint32 join_trans = send_voice_join (fd, &htlc, 0);
    g_assert_cmpuint (join_trans, !=, 0);
    g_assert_true (integration_drain_until_task_trans (fd, &htlc, join_trans,
                                                       64));
    g_assert_cmphex (hdr_flag (&htlc) & 1, ==, 0);

    /* Mute. */
    guint32 m1_trans = send_voice_mute (fd, &htlc, 0, /*muted=*/1);
    g_assert_cmpuint (m1_trans, !=, 0);
    g_assert_true (integration_drain_until_task_trans (fd, &htlc, m1_trans,
                                                       32));
    g_assert_cmphex (hdr_flag (&htlc) & 1, ==, 0);

    /* Unmute. */
    guint32 m0_trans = send_voice_mute (fd, &htlc, 0, /*muted=*/0);
    g_assert_cmpuint (m0_trans, !=, 0);
    g_assert_true (integration_drain_until_task_trans (fd, &htlc, m0_trans,
                                                       32));
    g_assert_cmphex (hdr_flag (&htlc) & 1, ==, 0);

    guint32 leave_trans = send_voice_leave (fd, &htlc, 0);
    g_assert_cmpuint (leave_trans, !=, 0);
    g_assert_true (integration_drain_until_task_trans (fd, &htlc, leave_trans,
                                                       32));
    g_assert_cmphex (hdr_flag (&htlc) & 1, ==, 0);

    integration_release_htlc (&htlc);
    integration_close (fd);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/integration/voice/mute_round_trip",
                     test_voice_mute_round_trip);

    return g_test_run ();
}
