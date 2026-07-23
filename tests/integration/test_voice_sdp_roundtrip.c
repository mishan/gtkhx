/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_voice_sdp_roundtrip.c — Tier 3 coverage for
 * the SDP offer/answer exchange (Phase 8.F).
 *
 * Wire contract under test:
 *   1. VOICE_JOIN(0) → TASK reply carries the server's SDP offer.
 *   2. Client extracts the offer, builds a syntactically-valid
 *      answer that mirrors the offer's m=audio line + the spec's
 *      a=mid:send / per-participant labels, and sends
 *      HTLC_HDR_VOICE_SDP_ANSWER (603) with HTLC_DATA_CHAT_ID +
 *      HTLC_DATA_VOICE_SDP.
 *   3. Server replies with a TASK matching the trans. The flag bit
 *      may or may not carry an error: a server that successfully
 *      parses the answer's SDP and queues it for downstream WebRTC
 *      negotiation replies non-error; a server that rejects the
 *      answer at the SDP layer (because the synthetic answer here
 *      doesn't include real DTLS fingerprint / ICE creds — those
 *      need a live GStreamer pipeline) replies error+TASKERROR.
 *      EITHER is a valid wire round-trip — the regression we're
 *      catching is the server dropping the connection, never
 *      replying, or sending a frame that breaks the trans
 *      correlation.
 *
 * Why a synthetic answer: a fully-valid SDP answer requires real
 * DTLS-SRTP fingerprints, ICE candidates, etc. — all coming from a
 * GStreamer / webrtcbin pipeline which this Tier 3 test
 * deliberately doesn't run. The Phase 8 plan covers media-path
 * round-trips manually against a real client; this test pins the
 * control-channel wire shape only.
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

static guint32
send_voice_sdp_answer (int fd, struct htlc_conn *htlc, guint32 cid,
                       const char *sdp)
{
    guint32 cid_be = g_htonl(cid);
    guint32 trans = htlc->trans;
    if (!integration_send_message (
            fd, htlc, HTLC_HDR_VOICE_SDP_ANSWER, /*flag=*/0, /*hc=*/2,
            (int) HTLC_DATA_CHAT_ID, (int) sizeof (cid_be), &cid_be,
            (int) HTLC_DATA_VOICE_SDP, (int) strlen (sdp), (guint8 *) sdp)) {
        return 0;
    }
    return trans;
}

/* Test body. */
static void
test_voice_sdp_roundtrip (void)
{
    const hx_test_server *srv = pick_voice_server ();
    if (!srv) {
        g_test_fail_printf ("no voice-capable server in matrix.");
        return;
    }

    char nick[32];
    g_snprintf (nick, sizeof (nick), "VoiceSDP-%d-%04x", (int) getpid (),
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

    /* JOIN. */
    guint32 join_trans = send_voice_join (fd, &htlc, 0);
    g_assert_cmpuint (join_trans, !=, 0);
    g_assert_true (integration_drain_until_task_trans (fd, &htlc, join_trans,
                                                       64));
    g_assert_cmphex (hdr_flag (&htlc) & 1, ==, 0);

    /* Pull the offer out so we can spot-check it was real before
     * we send our answer. The bytes themselves we don't reuse —
     * the answer below is synthetic. */
    struct gtkhx_proto_voice_reply r;
    memset (&r, 0, sizeof (r));
    gtkhx_proto_parse_voice_reply (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, &r);
    g_assert_true (r.sdp_present);
    g_assert_cmpuint (r.sdp_len, >, 0);

    /* Synthetic SDP answer. Not a valid WebRTC answer (no real
     * DTLS fingerprint or ICE creds), but syntactically a parsable
     * SDP. The intent is to drive the server's parse path: a
     * crash or an unparseable wire shape would manifest as either
     * a dropped connection or a missing TASK reply. A clean
     * task-error (flag&1) on this is acceptable — the regression
     * we're catching is the absence of a reply. */
    const char *answer_sdp
        = "v=0\r\n"
          "o=- 0 0 IN IP4 127.0.0.1\r\n"
          "s=-\r\n"
          "t=0 0\r\n"
          "a=group:BUNDLE 0\r\n"
          "m=audio 9 UDP/TLS/RTP/SAVPF 0\r\n"
          "c=IN IP4 0.0.0.0\r\n"
          "a=rtcp:9 IN IP4 0.0.0.0\r\n"
          "a=mid:0\r\n"
          "a=sendrecv\r\n"
          "a=rtpmap:0 PCMU/8000\r\n"
          "a=ice-ufrag:gtkhx\r\n"
          "a=ice-pwd:gtkhxgtkhxgtkhxgtkhxgtkhxgt\r\n"
          "a=fingerprint:sha-256 "
          "00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:"
          "00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00\r\n"
          "a=setup:active\r\n";

    guint32 ans_trans = send_voice_sdp_answer (fd, &htlc, 0, answer_sdp);
    g_assert_cmpuint (ans_trans, !=, 0);
    g_assert_true (integration_drain_until_task_trans (fd, &htlc, ans_trans,
                                                       64));
    /* We deliberately do NOT assert flag&1 == 0 here. See header
     * preamble: either outcome (clean ack or graceful task-error) is
     * a valid wire round-trip; we're catching connection drops or
     * missing replies. */
    if (hdr_flag (&htlc) & 1) {
        /* Surface the error chunk in the test log for diagnostic
         * value — it's not a failure, but knowing WHY the server
         * rejected the synthetic answer helps when iterating on
         * future answer-shape tests. */
        char err[256] = { 0 };
        gsize err_len = 0;
        if (task_error_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, err, sizeof (err), &err_len)) {
            g_test_message ("VOICE_SDP_ANSWER rejected (expected, synthetic "
                            "answer lacks real DTLS+ICE): \"%s\"", err);
        }
    } else {
        g_test_message ("VOICE_SDP_ANSWER accepted by server.");
    }

    /* LEAVE round-trip to clean up. */
    guint32 leave_trans = send_voice_leave (fd, &htlc, 0);
    g_assert_cmpuint (leave_trans, !=, 0);
    g_assert_true (integration_drain_until_task_trans (fd, &htlc, leave_trans,
                                                       32));
    /* LEAVE must succeed — we joined, server has us in the room. */
    g_assert_cmphex (hdr_flag (&htlc) & 1, ==, 0);

    integration_release_htlc (&htlc);
    integration_close (fd);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/integration/voice/sdp_roundtrip",
                     test_voice_sdp_roundtrip);

    return g_test_run ();
}
