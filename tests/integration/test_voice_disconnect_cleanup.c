/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_voice_disconnect_cleanup.c — Tier 3 coverage
 * for "disconnect = automatic leave" (Phase 8.F).
 *
 * Wire contract under test:
 *   1. Alice + Bob both log in with HTLC_CAP_VOICE.
 *   2. Both join voice room cid=0.
 *   3. Alice confirms Bob is present via a 605 VOICE_ROOM_STATUS
 *      whose participant count grew to >= 2.
 *   4. Bob closes his TCP control connection abruptly (no
 *      VOICE_LEAVE first).
 *   5. Alice's connection should receive a follow-up 605 frame
 *      whose participant count dropped back below the post-Bob
 *      total — the server's cleanup-on-disconnect fired.
 *
 * This is the spec's "server cleans up if the TCP control
 * connection drops" contract from §1 of docs/voice-chat-plan.md.
 *
 * Why match on participant-count delta, not Bob's specific uid:
 * see the same note in test_voice_participants.c — Janus's
 * SELFINFO doesn't carry the local USER_LIST chunk, so
 * htlc_b.uid stays 0 and a uid-based filter would never match.
 * The growth-then-shrink witness is equivalent to "Bob became
 * visible, then was cleaned up" without needing the unreliable
 * uid.
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

/* Pull the participant count out of whatever 605 / JOIN-reply frame
 * currently sits in htlc->in. Returns -1 if the frame doesn't
 * carry a participants blob. */
static int
participant_count (struct htlc_conn *htlc)
{
    struct gtkhx_proto_voice_reply r;
    memset (&r, 0, sizeof (r));
    gtkhx_proto_parse_voice_reply (htlc->in.buf, htlc->in.pos, &r);
    if (!r.participants_present) {
        return -1;
    }
    const guint8 *blob = NULL;
    gsize blob_len = 0;
    if (!gtkhx_proto_voice_reply_field (htlc->in.buf, htlc->in.pos, 3, &blob,
                                        &blob_len)) {
        return -1;
    }
    enum { MAX_P = 64 };
    struct gtkhx_proto_voice_participant ents[MAX_P];
    return (int) gtkhx_proto_parse_voice_participants (blob, blob_len, ents,
                                                       MAX_P);
}

/* Drain on `fd` looking for a 605 whose participant count is >=
 * `min_count`. Returns the count on success, -1 on timeout. */
static int
drain_for_room_status_min (int fd, struct htlc_conn *htlc, int min_count,
                           int max_messages)
{
    for (int i = 0; i < max_messages; i++) {
        if (!integration_recv_message (fd, htlc, /*timeout_ms=*/3000)) {
            return -1;
        }
        if (hdr_type (htlc) != HTLS_HDR_VOICE_ROOM_STATUS) {
            continue;
        }
        int n = participant_count (htlc);
        if (n >= min_count) {
            return n;
        }
    }
    return -1;
}

/* Drain on `fd` looking for a 605 whose participant count is <=
 * `max_count` (and >= 0 — a malformed frame doesn't count as
 * shrinkage). Returns the count on success, -1 on timeout.
 *
 * Per-recv timeout is generous (15 s) because Janus's cleanup
 * latency after an abrupt-close FIN is variable: the room-update
 * fan-out doesn't fire on the FIN read itself but on a subsequent
 * housekeeping tick. The growth witness (drain_for_room_status_min
 * below) tolerates the same lag at the join-side. */
static int
drain_for_room_status_max (int fd, struct htlc_conn *htlc, int max_count,
                           int max_messages)
{
    for (int i = 0; i < max_messages; i++) {
        if (!integration_recv_message (fd, htlc, /*timeout_ms=*/15000)) {
            return -1;
        }
        if (hdr_type (htlc) != HTLS_HDR_VOICE_ROOM_STATUS) {
            continue;
        }
        int n = participant_count (htlc);
        if (n >= 0 && n <= max_count) {
            return n;
        }
    }
    return -1;
}

static void
test_voice_disconnect_cleanup (void)
{
    const hx_test_server *srv = pick_voice_server ();
    if (!srv) {
        g_test_fail_printf ("no voice-capable server in matrix.");
        return;
    }

    char a_nick[32], b_nick[32];
    g_snprintf (a_nick, sizeof (a_nick), "VoiceDcA-%d-%04x", (int) getpid (),
                g_random_int () & 0xffff);
    g_snprintf (b_nick, sizeof (b_nick), "VoiceDcB-%d-%04x", (int) getpid (),
                g_random_int () & 0xffff);

    struct htlc_conn htlc_a;
    int fd_a = integration_open_login_to_caps_or_skip (srv, &htlc_a, a_nick,
                                                       412, HTLC_CAP_VOICE);
    if (fd_a < 0) {
        return;
    }
    g_assert_true (integration_send_agreementagree_hope (
        fd_a, &htlc_a, /*hope=*/NULL, a_nick, 412));

    struct htlc_conn htlc_b;
    int fd_b = integration_open_login_to_caps_or_skip (srv, &htlc_b, b_nick,
                                                       412, HTLC_CAP_VOICE);
    if (fd_b < 0) {
        integration_release_htlc (&htlc_a);
        integration_close (fd_a);
        return;
    }
    g_assert_true (integration_send_agreementagree_hope (
        fd_b, &htlc_b, /*hope=*/NULL, b_nick, 412));

    guint32 a_join = send_voice_join (fd_a, &htlc_a, 0);
    g_assert_cmpuint (a_join, !=, 0);
    g_assert_true (integration_drain_until_task_trans (fd_a, &htlc_a, a_join,
                                                       64));
    g_assert_cmphex (hdr_flag (&htlc_a) & 1, ==, 0);

    int baseline = participant_count (&htlc_a);
    g_assert_cmpint (baseline, >=, 1);
    g_test_message ("Alice JOIN reply participants=%d", baseline);

    guint32 b_join = send_voice_join (fd_b, &htlc_b, 0);
    g_assert_cmpuint (b_join, !=, 0);
    g_assert_true (integration_drain_until_task_trans (fd_b, &htlc_b, b_join,
                                                       64));
    g_assert_cmphex (hdr_flag (&htlc_b) & 1, ==, 0);

    /* Anchor: Alice should see Bob's join reflected in a fresh 605
     * before we test the disconnect cleanup. Without this we can't
     * tell post-Bob shrinkage apart from a stale pre-Bob frame. */
    int grew = drain_for_room_status_min (fd_a, &htlc_a, baseline + 1,
                                          /*max_messages=*/64);
    if (grew < 0) {
        g_test_fail_printf (
            "Alice did not see Bob's join reflected in a 605 update "
            "(baseline=%d).", baseline);
        goto cleanup;
    }
    g_test_message ("Alice saw 605 with participants=%d (post-Bob join).",
                    grew);

    /* Bob abrupt-closes — no VOICE_LEAVE, no clean shutdown.  The
     * server's cleanup-on-disconnect should fire and Alice should
     * see a 605 whose count dropped back to the pre-Bob baseline
     * (or lower — server might collapse a multi-step cleanup into
     * one update). */
    integration_release_htlc (&htlc_b);
    integration_close (fd_b);

    int shrank = drain_for_room_status_max (fd_a, &htlc_a, grew - 1,
                                            /*max_messages=*/64);
    if (shrank < 0) {
        g_test_fail_printf (
            "Alice did not see post-disconnect shrinkage in a 605 update "
            "(post-Bob count was %d).", grew);
    } else {
        g_test_message ("Alice saw 605 with participants=%d after Bob "
                        "abrupt-close.", shrank);
        g_assert_cmpint (shrank, <, grew);
    }

cleanup:
    {
        guint32 a_leave = send_voice_leave (fd_a, &htlc_a, 0);
        g_assert_cmpuint (a_leave, !=, 0);
        integration_drain_until_task_trans (fd_a, &htlc_a, a_leave, 32);
    }

    integration_release_htlc (&htlc_a);
    integration_close (fd_a);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/integration/voice/disconnect_cleanup",
                     test_voice_disconnect_cleanup);

    return g_test_run ();
}
