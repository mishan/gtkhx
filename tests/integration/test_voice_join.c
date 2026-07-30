/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_voice_join.c — Tier 3 coverage for the
 * fogWraith voice-chat extension's JOIN round-trip (Phase 8.F).
 *
 * Wire contract under test:
 *   1. Client advertises HTLC_CAP_VOICE in LOGIN. Server echoes the
 *      bit back in the TASK reply's HTLS_DATA_CAPABILITIES. Captured
 *      by the harness into htlc->caps.
 *   2. Client sends HTLC_HDR_VOICE_JOIN (600) with HTLC_DATA_CHAT_ID
 *      = 0 (the public chat). Server replies with a TASK matching
 *      the trans, carrying the four spec-mandated chunks:
 *        - HTLS_DATA_CHAT_ID  echo
 *        - HTLS_DATA_VOICE_SDP    server's SDP offer (the server is
 *                                 ALWAYS the offerer per spec)
 *        - HTLS_DATA_VOICE_CODEC  active codec (typically "PCMU")
 *        - HTLS_DATA_VOICE_PARTICIPANTS  packed participants blob
 *      gtkhx_proto_parse_voice_reply (the same parser production
 *      rcv_task_voice_join uses) is the witness that the wire shape
 *      hasn't drifted.
 *   3. Client sends HTLC_HDR_VOICE_LEAVE (601) to clean up and
 *      expects a non-error empty-body TASK ack.
 *
 * Server gating: requires HX_TEST_CAP_VOICE, which today only Janus
 * advertises. Per the no-silent-skips policy, the test fails loudly
 * (g_test_fail_printf + return) when no voice-capable target is
 * configured in the matrix — missing Tier 3 coverage should look
 * red in CI, not green.
 *
 * Janus's voice extension is server-side opt-in (config.yaml:
 * EnableVoice: true + per-account VoiceChat: true). The bundled
 * tests/janus/Dockerfile config flips both on, so this test
 * exercises the full happy path against the standard container.
 *
 * Parallelism: Janus-only test → is_parallel: false in meson.build.
 * Same per-IP connect-rate caveat documented on the chacha20 sister
 * tests.
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

/* Pick the first voice-capable server in the matrix, or NULL if
 * none survived the GTKHX_TEST_SERVERS env filter. Same shape the
 * other capability-gated Tier 3 tests use. */
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

/* Send HTLC_HDR_VOICE_JOIN(cid) and return the trans hlpack stamped
 * onto the wire (capture BEFORE the send — hlpack writes htlc->trans
 * into the header then increments, so the pre-send value is what the
 * server echoes back in the TASK reply). The HTLC_DATA_CHAT_ID chunk
 * carries the room id as a 4-byte big-endian word. */
static guint32
send_voice_join (int fd, struct htlc_conn *htlc, guint32 cid)
{
    guint32 cid_be = g_htonl (cid);
    guint32 trans = htlc->trans;
    if (!integration_send_message (fd, htlc, HTLC_HDR_VOICE_JOIN, /*flag=*/0,
                                   /*hc=*/1, (int)HTLC_DATA_CHAT_ID,
                                   (int)sizeof (cid_be), &cid_be)) {
        return 0;
    }
    return trans;
}

/* Send HTLC_HDR_VOICE_LEAVE(cid) symmetric with send_voice_join. */
static guint32
send_voice_leave (int fd, struct htlc_conn *htlc, guint32 cid)
{
    guint32 cid_be = g_htonl (cid);
    guint32 trans = htlc->trans;
    if (!integration_send_message (fd, htlc, HTLC_HDR_VOICE_LEAVE, /*flag=*/0,
                                   /*hc=*/1, (int)HTLC_DATA_CHAT_ID,
                                   (int)sizeof (cid_be), &cid_be)) {
        return 0;
    }
    return trans;
}

/* Capability negotiation: advertising HTLC_CAP_VOICE in LOGIN earns
 * an echo in HTLS_DATA_CAPABILITIES from a voice-capable server. The
 * harness's drain captured the echo into htlc->caps; this test just
 * asserts the bit survived the trip. Same shape as
 * test_chat_history's cap_negotiation. */
static void
test_voice_cap_negotiation (void)
{
    const hx_test_server *srv = pick_voice_server ();
    if (!srv) {
        g_test_fail_printf ("no voice-capable server in matrix "
                            "(GTKHX_TEST_SERVERS filter excluded all).");
        return;
    }

    struct htlc_conn htlc;
    int fd = integration_open_login_to_caps_or_skip (
        srv, &htlc, "VoiceCap Tier-3", 412, HTLC_CAP_VOICE);
    if (fd < 0) {
        return;
    }

    g_assert_cmphex ((htlc.caps & HTLC_CAP_VOICE), ==, HTLC_CAP_VOICE);

    integration_release_htlc (&htlc);
    integration_close (fd);
}

/* JOIN/LEAVE round-trip against the public chat. Asserts:
 *   - cap echoed (precondition for any voice opcode)
 *   - JOIN TASK reply correlates by trans, non-error
 *   - the four spec-mandated chunks are present and well-formed
 *   - the codec the server chose is "PCMU" (the only codec the
 *     spec defines today)
 *   - LEAVE TASK reply correlates by trans, non-error */
static void
test_voice_join_leave_round_trip (void)
{
    const hx_test_server *srv = pick_voice_server ();
    if (!srv) {
        g_test_fail_printf ("no voice-capable server in matrix.");
        return;
    }

    /* Per-process nick so concurrent test processes don't clobber
     * each other's session state on the server's voice tracker. */
    char nick[32];
    g_snprintf (nick, sizeof (nick), "VoiceJoin-%d-%04x", (int)getpid (),
                g_random_int () & 0xffff);

    struct htlc_conn htlc;
    int fd = integration_open_login_to_caps_or_skip (srv, &htlc, nick, 412,
                                                     HTLC_CAP_VOICE);
    if (fd < 0) {
        return;
    }

    /* Cross the "officially joined" boundary. Janus voice JOIN
     * requires the user to be past AGREEMENTAGREE the same way
     * the colored-nickname update does — without it the server
     * stashes the request in a pre-join queue we never get a reply
     * out of. The helper falls through to plain send when hope is
     * NULL. */
    g_assert_true (integration_send_agreementagree_hope (
        fd, &htlc, /*hope=*/NULL, nick, 412));

    g_assert_cmphex ((htlc.caps & HTLC_CAP_VOICE), ==, HTLC_CAP_VOICE);

    /* Send VOICE_JOIN against the public chat. */
    guint32 join_trans = send_voice_join (fd, &htlc, /*cid=*/0);
    g_assert_cmpuint (join_trans, !=, 0);

    /* Drain past unsolicited broadcasts (USER_CHANGE from concurrent
     * tests, BANNER, possibly an unsolicited 605 ROOM_STATUS) until
     * the JOIN TASK reply lands. The 64-message budget matches what
     * the chat-create tests use against Janus. */
    g_assert_true (integration_drain_until_task_trans (fd, &htlc, join_trans,
                                                       /*max_messages=*/64));
    g_assert_cmphex (hdr_flag (&htlc) & 1, ==, 0);

    /* The reply payload is parsed by the same Rust parser production
     * runs in rcv.c::rcv_task_voice_join. Witnesses both the
     * presence-of-mandatory-chunks contract AND the byte-shape
     * round-trip. */
    struct gtkhx_proto_voice_reply r;
    memset (&r, 0, sizeof (r));
    /* gtkhx_proto_parse_voice_reply only returns false on NULL out
     * (see ffi.rs::gtkhx_proto_parse_voice_reply). The presence
     * flags below are the real malformed-frame signal. */
    gtkhx_proto_parse_voice_reply (hx_test_in (&htlc)->buf,
                                   hx_test_in (&htlc)->pos, &r);
    g_assert_cmpuint (r.cid, ==, 0);
    g_assert_true (r.sdp_present);
    g_assert_true (r.codec_present);
    g_assert_true (r.participants_present);
    g_assert_cmpuint (r.sdp_len, >, 0);
    g_assert_cmpuint (r.codec_len, >, 0);

    /* SDP summary sanity. The wire SDP must be well-formed enough
     * for the production summary parser to digest it. A server that
     * emitted a corrupt or truncated SDP would fail here before any
     * downstream WebRTC code got a chance to mis-handle it. */
    const guint8 *sdp_ptr = NULL;
    gsize sdp_len = 0;
    g_assert_true (gtkhx_proto_voice_reply_field (hx_test_in (&htlc)->buf,
                                                  hx_test_in (&htlc)->pos, 0,
                                                  &sdp_ptr, &sdp_len));
    g_assert_nonnull (sdp_ptr);
    g_assert_cmpuint (sdp_len, >, 0);

    struct gtkhx_proto_voice_sdp_summary sum;
    g_assert_true (
        gtkhx_proto_parse_voice_sdp_summary (sdp_ptr, sdp_len, &sum));
    /* PCMU is the only codec the spec defines; the parser is the
     * authority on whether the server complied. */
    g_assert_true (sum.has_pcmu);

    /* Codec name should be "PCMU" — the spec only defines that
     * one. Compare on a NUL-terminated copy (wire field is raw
     * bytes, no terminator). */
    const guint8 *codec_ptr = NULL;
    gsize codec_len = 0;
    g_assert_true (gtkhx_proto_voice_reply_field (hx_test_in (&htlc)->buf,
                                                  hx_test_in (&htlc)->pos, 2,
                                                  &codec_ptr, &codec_len));
    char codec_buf[16] = { 0 };
    gsize ccap = (codec_len < sizeof (codec_buf) - 1) ? codec_len
                                                      : sizeof (codec_buf) - 1;
    memcpy (codec_buf, codec_ptr, ccap);
    g_assert_cmpstr (codec_buf, ==, "PCMU");

    /* LEAVE round-trip. */
    guint32 leave_trans = send_voice_leave (fd, &htlc, /*cid=*/0);
    g_assert_cmpuint (leave_trans, !=, 0);
    g_assert_true (integration_drain_until_task_trans (fd, &htlc, leave_trans,
                                                       /*max_messages=*/32));
    g_assert_cmphex (hdr_flag (&htlc) & 1, ==, 0);

    integration_release_htlc (&htlc);
    integration_close (fd);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/integration/voice/cap_negotiation",
                     test_voice_cap_negotiation);
    g_test_add_func ("/integration/voice/join_leave_round_trip",
                     test_voice_join_leave_round_trip);

    return g_test_run ();
}
