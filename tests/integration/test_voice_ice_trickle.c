/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_voice_ice_trickle.c — Tier 3 coverage for
 * the ICE candidate trickle (Phase 8.F).
 *
 * Wire contract under test:
 *   1. VOICE_JOIN(0).
 *   2. Client builds an ICE candidate JSON via
 *      gtkhx_proto_build_voice_ice_json (the same builder
 *      production calls) and sends HTLC_HDR_VOICE_ICE (604) with
 *      HTLC_DATA_CHAT_ID + HTLC_DATA_VOICE_ICE.
 *   3. Client sends an end-of-candidates marker (empty-string
 *      candidate via the spec's shorthand: empty VOICE_ICE chunk).
 *   4. Client round-trips HTLC_HDR_PING to confirm the stream
 *      stayed healthy. 604 is a server→client / client→server
 *      notification per spec (no TASK reply expected), so the
 *      witness for "the server didn't choke" is a PING TASK reply
 *      arriving after the ICE messages.
 *
 * The synthetic candidate uses 127.0.0.1:1 host-type RTP, which is
 * not a real WebRTC endpoint — same intent as the SDP-roundtrip
 * test: we're pinning the WIRE shape, not the WebRTC media path.
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

/* Send HTLC_HDR_VOICE_ICE with CHAT_ID + VOICE_ICE chunks. Spec
 * allows the VOICE_ICE chunk body to be empty as the
 * end-of-candidates marker. */
static gboolean
send_voice_ice (int fd, struct htlc_conn *htlc, guint32 cid,
                const guint8 *ice_json, gsize ice_len)
{
    guint32 cid_be = htonl (cid);
    /* When ice_len == 0 we still send the chunk header (EOC
     * shorthand). integration_send_message hands the byte buffer
     * straight to hlpack which lets a zero-length data chunk
     * through. */
    return integration_send_message (
        fd, htlc, HTLC_HDR_VOICE_ICE, /*flag=*/0, /*hc=*/2,
        (int) HTLC_DATA_CHAT_ID, (int) sizeof (cid_be), &cid_be,
        (int) HTLC_DATA_VOICE_ICE, (int) ice_len,
        (guint8 *) (ice_json ? ice_json : (const guint8 *) ""));
}

static void
test_voice_ice_trickle (void)
{
    const hx_test_server *srv = pick_voice_server ();
    if (!srv) {
        g_test_fail_printf ("no voice-capable server in matrix.");
        return;
    }

    char nick[32];
    g_snprintf (nick, sizeof (nick), "VoiceICE-%d-%04x", (int) getpid (),
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

    /* Build a synthetic host-type ICE candidate via the production
     * JSON builder. The shape matches what webrtcbin would emit
     * for a fresh ICE gathering pass; the address is loopback
     * because we're pinning wire format, not negotiating real
     * media. */
    const char *cand
        = "candidate:1 1 UDP 2130706431 127.0.0.1 1 typ host";
    const char *sdp_mid = "0";
    guint8 json_buf[512];
    size_t json_len = gtkhx_proto_build_voice_ice_json (
        (const guint8 *) cand, strlen (cand),
        (const guint8 *) sdp_mid, strlen (sdp_mid),
        /*sdp_mline_index=*/0, /*sdp_mline_index_present=*/true,
        /*username_fragment_ptr=*/NULL, /*username_fragment_len=*/0,
        json_buf, sizeof (json_buf));
    g_assert_cmpuint (json_len, >, 0);

    g_assert_true (send_voice_ice (fd, &htlc, 0, json_buf, json_len));

    /* End-of-candidates marker: empty VOICE_ICE chunk body per
     * spec. The handler in production accepts both the
     * empty-string JSON variant and the empty-chunk variant; we
     * exercise the latter here since it's the shorthand the spec
     * lists first. */
    g_assert_true (send_voice_ice (fd, &htlc, 0, NULL, 0));

    /* PING round-trip — proof the stream stayed sane after the
     * notifications. 604 has no TASK reply (it's a bidirectional
     * notification) so a healthy PING completing IS the
     * stream-healthy witness. The 8-message drain budget matches
     * what test_ping uses; voice notifications from the server
     * (a 602 SDP offer hasn't been triggered, a 605 might fire
     * before PING) need a small margin. */
    guint32 ping_trans = integration_send_ping (fd, &htlc);
    g_assert_cmpuint (ping_trans, !=, 0);
    g_assert_true (integration_drain_until_task_trans (fd, &htlc, ping_trans,
                                                       16));
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

    g_test_add_func ("/integration/voice/ice_trickle", test_voice_ice_trickle);

    return g_test_run ();
}
