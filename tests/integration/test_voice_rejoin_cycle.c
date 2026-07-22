/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_voice_rejoin_cycle.c — Tier 3 coverage for
 * the LEAVE → JOIN rejoin cycle within a single TCP control
 * connection (Phase 8.F follow-up).
 *
 * Wire contract under test:
 *   1. Client logs in with HTLC_CAP_VOICE.
 *   2. VOICE_JOIN(0) → reply carries an SDP offer (offer #1).
 *   3. Synthetic VOICE_SDP_ANSWER → reply ack/task-error.
 *   4. VOICE_LEAVE(0) → reply must succeed.
 *   5. VOICE_JOIN(0) again on the same fd → reply carries an SDP
 *      offer (offer #2). Offer #2 MUST be distinct from offer #1:
 *      different `o=` line (session-id and/or version), so the
 *      server isn't recycling stale negotiation state.
 *
 * The runtime side of this scenario lives on
 * claude/voice-renegotiate-fix's Fix #4 (`Action::TearDown` walks
 * the pipeline back to `Null` and rebuilds a fresh webrtcbin via
 * `build_pipeline_bits` so the next `JoinRequested` doesn't try to
 * renegotiate against the prior session's ICE / DTLS credentials).
 * For that fix to be useful at all the SERVER must give the client
 * a fresh offer on rejoin — if Janus cached and recycled the prior
 * offer, the client's rebuilt webrtcbin would receive a stale SDP
 * pointing at the old server-side credentials and the new ICE
 * agent's binding would still mismatch. This test pins "fresh
 * offer per JOIN" as the server-side contract the runtime fix
 * depends on.
 *
 * Server gating: HX_TEST_CAP_VOICE → Janus today. is_parallel:false
 * matches the rest of the voice tests.
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

static const char ANSWER_SDP[]
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

static guint32
send_voice_sdp_answer (int fd, struct htlc_conn *htlc, guint32 cid,
                       const char *sdp)
{
    guint32 cid_be = htonl (cid);
    guint32 trans = htlc->trans;
    if (!integration_send_message (
            fd, htlc, HTLC_HDR_VOICE_SDP_ANSWER, /*flag=*/0, /*hc=*/2,
            (int) HTLC_DATA_CHAT_ID, (int) sizeof (cid_be), &cid_be,
            (int) HTLC_DATA_VOICE_SDP, (int) strlen (sdp), (guint8 *) sdp)) {
        return 0;
    }
    return trans;
}

/* Extract the `o=` (origin) line from the SDP currently parked in
 * htlc->in. Writes a NUL-terminated copy of the (CRLF-stripped) line
 * into `out` (capped at `out_cap`), returning TRUE on success. The
 * `o=` line encodes session-id + version per RFC 4566 — server-side
 * recycling of an offer would reuse the same session-id, so
 * comparing this line between the two joins is the cheapest
 * "different offer?" witness available without a real SDP parser. */
static gboolean
extract_sdp_origin (struct htlc_conn *htlc, char *out, gsize out_cap)
{
    const guint8 *sdp_ptr = NULL;
    gsize sdp_len = 0;
    if (!gtkhx_proto_voice_reply_field (hx_test_in(htlc)->buf, hx_test_in(htlc)->pos,
                                        /*field=SDP*/ 0, &sdp_ptr, &sdp_len)) {
        return FALSE;
    }
    const char *line_start = (const char *) sdp_ptr;
    const char *end = (const char *) sdp_ptr + sdp_len;
    while (line_start < end) {
        const char *eol = memchr (line_start, '\n', end - line_start);
        const char *line_end = eol ? eol : end;
        const char *real_end = line_end;
        if (real_end > line_start && real_end[-1] == '\r') {
            real_end--;
        }
        if ((real_end - line_start) >= 2 && line_start[0] == 'o'
            && line_start[1] == '=') {
            gsize n = (gsize) (real_end - line_start);
            if (n + 1 > out_cap) {
                n = out_cap - 1;
            }
            memcpy (out, line_start, n);
            out[n] = '\0';
            return TRUE;
        }
        if (!eol) {
            break;
        }
        line_start = eol + 1;
    }
    return FALSE;
}

static void
test_voice_rejoin_cycle (void)
{
    const hx_test_server *srv = pick_voice_server ();
    if (!srv) {
        g_test_fail_printf ("no voice-capable server in matrix.");
        return;
    }

    char nick[32];
    g_snprintf (nick, sizeof (nick), "VoiceReJ-%d-%04x", (int) getpid (),
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

    /* JOIN #1 — capture offer #1's o= line. */
    guint32 join1_trans = send_voice_join (fd, &htlc, 0);
    g_assert_cmpuint (join1_trans, !=, 0);
    g_assert_true (integration_drain_until_task_trans (fd, &htlc, join1_trans,
                                                       64));
    g_assert_cmphex (hdr_flag (&htlc) & 1, ==, 0);
    {
        struct gtkhx_proto_voice_reply r;
        memset (&r, 0, sizeof (r));
        gtkhx_proto_parse_voice_reply (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, &r);
        g_assert_true (r.sdp_present);
        g_assert_cmpuint (r.sdp_len, >, 0);
    }
    char origin1[256] = { 0 };
    if (!extract_sdp_origin (&htlc, origin1, sizeof (origin1))) {
        g_test_fail_printf ("first JOIN's reply had no parseable o= line.");
        goto cleanup;
    }
    g_test_message ("First JOIN reply o=: %s", origin1);

    /* Synthetic answer — task-error is acceptable. */
    guint32 ans1_trans = send_voice_sdp_answer (fd, &htlc, 0, ANSWER_SDP);
    g_assert_cmpuint (ans1_trans, !=, 0);
    g_assert_true (integration_drain_until_task_trans (fd, &htlc, ans1_trans,
                                                       64));

    /* LEAVE — must be a clean ack (we joined, server has us). */
    guint32 leave1_trans = send_voice_leave (fd, &htlc, 0);
    g_assert_cmpuint (leave1_trans, !=, 0);
    g_assert_true (integration_drain_until_task_trans (fd, &htlc, leave1_trans,
                                                       32));
    g_assert_cmphex (hdr_flag (&htlc) & 1, ==, 0);

    /* JOIN #2 on the SAME fd — capture offer #2 and check it's
     * genuinely fresh. */
    guint32 join2_trans = send_voice_join (fd, &htlc, 0);
    g_assert_cmpuint (join2_trans, !=, 0);
    g_assert_cmpuint (join2_trans, !=, join1_trans);
    g_assert_true (integration_drain_until_task_trans (fd, &htlc, join2_trans,
                                                       64));
    g_assert_cmphex (hdr_flag (&htlc) & 1, ==, 0);
    {
        struct gtkhx_proto_voice_reply r;
        memset (&r, 0, sizeof (r));
        gtkhx_proto_parse_voice_reply (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, &r);
        g_assert_true (r.sdp_present);
        g_assert_cmpuint (r.sdp_len, >, 0);
    }
    char origin2[256] = { 0 };
    if (!extract_sdp_origin (&htlc, origin2, sizeof (origin2))) {
        g_test_fail_printf ("second JOIN's reply had no parseable o= line.");
        goto cleanup;
    }
    g_test_message ("Second JOIN reply o=: %s", origin2);

    /* Core assertion: the server's offers must differ between the
     * two joins. Same `o=` line would mean Janus recycled the prior
     * session's negotiation — the runtime's Fix #4 rebuild path
     * would then build a clean webrtcbin only to negotiate against
     * the SAME server-side credentials, which is the wedge-after-
     * rejoin shape we're guarding against. */
    if (strcmp (origin1, origin2) == 0) {
        g_test_fail_printf (
            "JOIN #1 and JOIN #2 returned identical SDP origins: \"%s\". "
            "The server appears to be recycling negotiation state across "
            "rejoins — that breaks the runtime's TearDown / rebuild path "
            "(Fix #4 on claude/voice-renegotiate-fix). Janus should ship a "
            "fresh o= session-id/version on every JOIN.",
            origin1);
        goto cleanup;
    }

    /* Round-trip JOIN #2's answer so the LEAVE below isn't issued
     * against a half-finished negotiation. */
    guint32 ans2_trans = send_voice_sdp_answer (fd, &htlc, 0, ANSWER_SDP);
    g_assert_cmpuint (ans2_trans, !=, 0);
    g_assert_true (integration_drain_until_task_trans (fd, &htlc, ans2_trans,
                                                       64));

cleanup:
    {
        guint32 t = send_voice_leave (fd, &htlc, 0);
        if (t != 0) {
            integration_drain_until_task_trans (fd, &htlc, t, 32);
        }
    }
    integration_release_htlc (&htlc);
    integration_close (fd);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/integration/voice/rejoin_cycle",
                     test_voice_rejoin_cycle);

    return g_test_run ();
}
