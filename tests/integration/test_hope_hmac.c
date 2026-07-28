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
 *      parsing, HMAC chain, Step 2 authenticated LOGIN
 *      (including the HTLC_DATA_CLIENTVERSION advertisement
 *      that flips on mhxd's can_ping access bit), post-login
 *      drain to SELFINFO. A clean return from the harness means
 *      every chunk in the Step 1 reply parsed, the HMAC chain
 *      produced output, the Step 2 LOGIN landed without a
 *      server-side task-error, and SELFINFO arrived.
 *   2. A plain-framing PING round-trip after the handshake
 *      confirms the post-HOPE connection is usable for normal
 *      traffic — and validates that CLIENTVERSION made it into
 *      STEP2 (without it, mhxd would task-error the PING; the
 *      pre-CLIENTVERSION version of this test had to skip the
 *      PING entirely for that reason).
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
 */

#include "config.h"
#include <string.h>
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
    const hx_test_server *srv = pick_hope_server ();
    if (!srv) {
        g_test_fail_printf ("no HX_TEST_CAP_HOPE server in the matrix. "
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

    /* Sanity check the harness populated htlc->uid from the
	 * post-Step-2 SELFINFO drain. A clean fd return from
	 * integration_open_login_hope_or_skip already implies SELFINFO
	 * was parsed without task-error, but checking htlc->uid != 0
	 * catches the (hypothetical) drift where the harness silently
	 * returns success on an empty SELFINFO chunk. */
    g_assert_cmphex (htlc.uid, !=, 0);

    /* Confirm the post-HOPE channel is usable: send a plain
	 * (non-AEAD) PING, drain to the matching TASK reply. mhxd
	 * gates HTLC_HDR_PING on the `can_ping` access bit, which
	 * it sets when the LOGIN included HTLC_DATA_CLIENTVERSION
	 * >= 150. STEP2 now emits that chunk (the Rust HOPE builder +
	 * harness send_hope_step2 + production rcv.c all set
	 * client_version=185), so this PING round-trip works.
	 * Drops to task-error if any of those three sites regresses. */
    guint32 ping_trans = integration_send_ping (fd, &htlc);
    g_assert_cmpuint (ping_trans, !=, 0);
    g_assert_true (integration_drain_until_task_trans (fd, &htlc, ping_trans,
                                                       64));
    g_assert_cmphex (hdr_flag (&htlc) & 1, ==, 0);

    integration_release_htlc (&htlc);
    integration_hope_session_release (&hope);
    integration_close (fd);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/integration/hope_hmac/login_and_ping",
                     test_hope_hmac_login_and_ping);
    return g_test_run ();
}
