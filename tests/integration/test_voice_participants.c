/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_voice_participants.c — Tier 3 coverage for
 * the VOICE_ROOM_STATUS (605) participant broadcast (Phase 8.F).
 *
 * Wire contract under test:
 *   1. Alice + Bob both log in with HTLC_CAP_VOICE.
 *   2. Alice VOICE_JOIN(0). Alice's JOIN reply carries an initial
 *      participants blob listing {Alice}. Snapshot the uids that
 *      reply named.
 *   3. Bob VOICE_JOIN(0).
 *   4. Alice's connection should receive an HTLS_HDR_VOICE_ROOM_STATUS
 *      (605) frame whose VOICE_PARTICIPANTS blob is strictly larger
 *      than the snapshot — at least one new uid appeared, i.e. Bob
 *      is now visible to Alice.
 *
 * Why match on participant-count growth, not Bob's specific uid:
 * the harness's open_login_to_caps_or_skip uses
 * hx_selfinfo_parse to pull the local uid out of SELFINFO, but
 * Janus's SELFINFO doesn't include the USER_LIST chunk that
 * carries it — so htlc_b.uid stays 0, and filtering 605 broadcasts
 * by uid==0 never matches. The growth-based witness is equivalent
 * for the contract under test ("a new participant became visible
 * to Alice") AND robust to that uid-discovery gap. The same
 * approach falls out for test_voice_disconnect_cleanup.
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

/* Pull the participant count out of whatever 605 / JOIN-reply frame
 * currently sits in htlc->in. Returns the entry count or -1 if the
 * frame doesn't carry a participants blob. */
static int
participant_count (struct htlc_conn *htlc)
{
    struct gtkhx_proto_voice_reply r;
    memset (&r, 0, sizeof (r));
    gtkhx_proto_parse_voice_reply (hx_test_in(htlc)->buf, hx_test_in(htlc)->pos, &r);
    if (!r.participants_present) {
        return -1;
    }
    const guint8 *blob = NULL;
    gsize blob_len = 0;
    if (!gtkhx_proto_voice_reply_field (hx_test_in(htlc)->buf, hx_test_in(htlc)->pos, 3, &blob,
                                        &blob_len)) {
        return -1;
    }
    /* Each participant entry is 6 bytes (user_id u16 + flags u16 +
     * codec_id u16, per gtkhx_proto_voice_participant). The Rust
     * parser's count IS blob_len / 6, but we re-derive via the
     * typed walk so the test is robust to a future entry-size
     * change. */
    enum { MAX_P = 64 };
    struct gtkhx_proto_voice_participant ents[MAX_P];
    return (int) gtkhx_proto_parse_voice_participants (blob, blob_len, ents,
                                                       MAX_P);
}

/* Drain on `fd` until a 605 ROOM_STATUS arrives whose participant
 * count is >= `min_count`. Returns the matched count on success,
 * -1 on timeout. */
static int
drain_for_room_status_with_min (int fd, struct htlc_conn *htlc, int min_count,
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

static void
test_voice_participants_broadcast (void)
{
    const hx_test_server *srv = pick_voice_server ();
    if (!srv) {
        g_test_fail_printf ("no voice-capable server in matrix.");
        return;
    }

    char a_nick[32], b_nick[32];
    g_snprintf (a_nick, sizeof (a_nick), "VoicePartA-%d-%04x", (int) getpid (),
                g_random_int () & 0xffff);
    g_snprintf (b_nick, sizeof (b_nick), "VoicePartB-%d-%04x", (int) getpid (),
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

    /* Alice joins first. Her JOIN TASK reply carries the initial
     * participants blob (just her — Bob hasn't joined yet). */
    guint32 a_join = send_voice_join (fd_a, &htlc_a, 0);
    g_assert_cmpuint (a_join, !=, 0);
    g_assert_true (integration_drain_until_task_trans (fd_a, &htlc_a, a_join,
                                                       64));
    g_assert_cmphex (hdr_flag (&htlc_a) & 1, ==, 0);

    int baseline = participant_count (&htlc_a);
    g_assert_cmpint (baseline, >=, 1);
    g_test_message ("Alice JOIN reply participants=%d", baseline);

    /* Bob joins. Bob's TASK reply also lands non-error. */
    guint32 b_join = send_voice_join (fd_b, &htlc_b, 0);
    g_assert_cmpuint (b_join, !=, 0);
    g_assert_true (integration_drain_until_task_trans (fd_b, &htlc_b, b_join,
                                                       64));
    g_assert_cmphex (hdr_flag (&htlc_b) & 1, ==, 0);

    /* Alice should now receive a 605 update with count > baseline.
     * Janus emits the broadcast quickly after Bob's JOIN reply
     * lands. */
    int grown = drain_for_room_status_with_min (fd_a, &htlc_a, baseline + 1,
                                                /*max_messages=*/64);
    if (grown < 0) {
        g_test_fail_printf (
            "Alice did not receive a VOICE_ROOM_STATUS (605) with "
            "participant_count > %d after Bob joined.", baseline);
    } else {
        g_test_message ("Alice saw 605 with participants=%d (baseline=%d).",
                        grown, baseline);
        g_assert_cmpint (grown, >, baseline);
    }

    /* Clean up. */
    guint32 a_leave = send_voice_leave (fd_a, &htlc_a, 0);
    g_assert_cmpuint (a_leave, !=, 0);
    integration_drain_until_task_trans (fd_a, &htlc_a, a_leave, 32);

    guint32 b_leave = send_voice_leave (fd_b, &htlc_b, 0);
    g_assert_cmpuint (b_leave, !=, 0);
    integration_drain_until_task_trans (fd_b, &htlc_b, b_leave, 32);

    integration_release_htlc (&htlc_b);
    integration_close (fd_b);
    integration_release_htlc (&htlc_a);
    integration_close (fd_a);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/integration/voice/participants_broadcast",
                     test_voice_participants_broadcast);

    return g_test_run ();
}
