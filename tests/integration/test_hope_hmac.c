/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_hope_hmac.c — Tier 3 end-to-end test for
 * the HOPE-Secure-Login handshake against an HMAC-only HOPE server.
 *
 * Coverage:
 *
 *   1. integration_open_login_hope_or_skip drives the full HOPE
 *      Step 1 / Step 2 dance against any server advertising
 *      HX_TEST_CAP_HOPE — but with cipheralg=NULL, so the cipher
 *      negotiation falls back to "NONE" and no AEAD framing is
 *      activated. This exercises everything ChaCha20 needs minus
 *      the AEAD path: algorithm advertisement, Step 1 reply
 *      parsing, HMAC chain, Step 2 authenticated LOGIN, post-
 *      login drain to SELFINFO.
 *   2. After the handshake we send HTLC_HDR_PING (plain framing —
 *      no AEAD) and drain to the matching TASK reply, proving
 *      the post-HOPE connection stays usable for normal traffic.
 *
 * Why this exists separately from test_hope_chacha20:
 *
 *   The ChaCha20 test filters for HX_TEST_CAP_CHACHA20, which only
 *   Janus would satisfy — and the Janus container's account YAMLs
 *   don't currently ship with HOPE-compatible password hashes (see
 *   server_matrix.c's Janus entry for the gating). That leaves the
 *   live HOPE handshake without coverage. mhxd in the matrix
 *   advertises HX_TEST_CAP_HOPE and serves HMAC-based HOPE
 *   (--enable-hope at build time, see tests/mhxd/Dockerfile); this
 *   test fills the gap by exercising the handshake against mhxd.
 *
 *   When the Janus container is updated to ship HOPE-compatible
 *   hashes, test_hope_chacha20 will start running and pick up the
 *   AEAD-framed superset; this test stays useful as the
 *   "without-AEAD" sibling.
 *
 *   This catches the bug class that produced the 2cce1f9
 *   regression — Step 1 / Step 2 chunk-shape drift that only
 *   surfaces when actually round-tripping against a real server.
 */

#include "config.h"
#include <string.h>
#include <netinet/in.h>
#include <unistd.h>
#include <glib.h>
#include "compat.h"
#include "hotline.h"
#include "protocol.h"
#include "proto_helpers.h"
#include "integration_harness.h"
#include "server_matrix.h"

/* Pick the first HOPE-capable server in the matrix, or NULL if
 * none survived the GTKHX_TEST_SERVERS env filter. Filters on
 * HX_TEST_CAP_HOPE alone — we don't need (and explicitly avoid)
 * HX_TEST_CAP_CHACHA20 here, so we match mhxd. */
static const hx_test_server *
pick_hope_server (void)
{
    GPtrArray *candidates = hx_test_servers_with (HX_TEST_CAP_HOPE);
    const hx_test_server *srv = NULL;
    if (candidates && candidates->len) {
        srv = candidates->pdata[0];
    }
    if (candidates) {
        g_ptr_array_unref (candidates);
    }
    return srv;
}

static void
test_hope_hmac_login_and_ping (void)
{
#ifndef CONFIG_CIPHER
    g_test_skip ("built without CONFIG_CIPHER — HOPE handshake "
                 "disabled at build time");
    return;
#else
    const hx_test_server *srv = pick_hope_server ();
    if (!srv) {
        g_test_skip ("no HX_TEST_CAP_HOPE server in the matrix. "
                     "Default mhxd container at tests/mhxd/ advertises "
                     "this cap; check GTKHX_TEST_SERVERS env if you've "
                     "filtered it out.");
        return;
    }

    struct htlc_conn htlc;
    integration_hope_session hope;
    /* cipheralg=NULL → "don't advertise a cipher". Server picks
	 * NONE; harness leaves hope.aead_active = 0 and we'll
	 * round-trip the PING through plain framing. Same goes for
	 * compress (we don't need either layer to validate HMAC). */
    int fd = integration_open_login_hope_or_skip (
        srv, &htlc, &hope,
        /*username=*/"guest",
        /*password=*/"",
        /*display_name=*/"HopeHmac Tier-3",
        /*icon=*/412,
        /*cipheralg=*/NULL,
        /*compressalg=*/NULL);
    if (fd < 0) {
        /* Harness already called g_test_fail_printf with a
		 * diagnostic. Anything other than "Step 1 task-error" is a
		 * real protocol bug — if mhxd ever returns one for the
		 * default guest account, fix it in the test container
		 * rather than masking the failure here. */
        return;
    }

    /* We negotiated HMAC-HOPE, not AEAD. aead_active stays 0. */
    g_assert_false (hope.aead_active);

    /* Confirm the post-login channel is usable: send a plain PING,
	 * drain to the matching TASK reply, assert no error flag. The
	 * harness uses non-AEAD send/recv when hope.aead_active is 0
	 * — same code path as every non-HOPE Tier 3 test, just with a
	 * connection that was established via the HOPE handshake. */
    guint32 ping_trans = integration_send_ping (fd, &htlc);
    g_assert_cmpuint (ping_trans, !=, 0);
    g_assert_true (integration_drain_until_task_trans (fd, &htlc, ping_trans,
                                                       64));
    g_assert_cmphex (hdr_flag (&htlc) & 1, ==, 0);

    integration_release_htlc (&htlc);
    integration_hope_session_release (&hope);
    integration_close (fd);
#endif /* CONFIG_CIPHER */
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/integration/hope_hmac/login_and_ping",
                     test_hope_hmac_login_and_ping);
    return g_test_run ();
}
