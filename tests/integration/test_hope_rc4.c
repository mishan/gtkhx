/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_hope_rc4.c — Tier 3 end-to-end test for the
 * HOPE-Secure-Login + RC4 stream-cipher path.
 *
 * Same shape as test_hope_blowfish.c — see that file's preamble for
 * the rationale (rekey-marker statistics, why N=12 pings, what
 * "stream-cipher round-trip" actually proves). The only differences
 * here are:
 *
 *   - cipheralg = "RC4"
 *   - HX_TEST_CAP_RC4 instead of HX_TEST_CAP_BLOWFISH on the matrix
 *     filter
 *
 * Keeping the two as separate binaries (instead of one parameterised
 * test) so individual failures bisect cleanly: a green test_hope_rc4
 * and a red test_hope_blowfish narrows the bug to Blowfish-OFB-64
 * without further diagnosis. Both ciphers share the same harness
 * code path — only cipher.c's per-direction state machine differs
 * underneath.
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

static const hx_test_server *
pick_rc4_server (void)
{
    GPtrArray *candidates = hx_test_servers_with (HX_TEST_CAP_RC4);
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
test_hope_rc4_login_and_ping (void)
{
    const hx_test_server *srv = pick_rc4_server ();
    if (!srv) {
        g_test_skip ("no HX_TEST_CAP_RC4 server in the matrix. "
                     "mhxd advertises this cap; bring it up with "
                     "`docker run -p 5500:5500 -p 5501:5501 "
                     "gtkhx-mhxd-test`.");
        return;
    }

    struct htlc_conn htlc;
    integration_hope_session hope;
    int fd = integration_open_login_hope_or_skip (
        srv, &htlc, &hope,
        /*username=*/"guest",
        /*password=*/"",
        /*display_name=*/"HopeRC4 Tier-3",
        /*icon=*/412,
        /*cipheralg=*/"RC4",
        /*compressalg=*/NULL);
    if (fd < 0) {
        return;
    }

    g_assert_true (hope.stream_active);
    g_assert_false (hope.aead_active);

    /* See test_hope_blowfish.c for the loop count + assertion
     * rationale (32 sends ≈ 0.004% miss rate for at least one
     * rekey firing; explicit count assertion catches a regression
     * that suppressed all rotations). */
    for (int i = 0; i < 32; i++) {
        guint32 ping_trans = htlc.trans;
        g_assert_true (integration_send_message_hope (
            fd, &htlc, &hope, HTLC_HDR_PING, /*flag=*/0, /*hc=*/0));

        gboolean got_reply = FALSE;
        for (int j = 0; j < 16 && !got_reply; j++) {
            if (!integration_recv_message_hope (fd, &htlc, &hope,
                                                /*timeout_ms=*/5000)) {
                break;
            }
            if (hdr_type (&htlc) != HTLS_HDR_TASK) {
                continue;
            }
            if (hdr_trans (&htlc) != ping_trans) {
                continue;
            }
            got_reply = TRUE;
        }
        g_assert_true (got_reply);
        g_assert_cmphex (hdr_flag (&htlc) & 1, ==, 0);
    }

    g_assert_cmpuint (hope.decode_rekey_count, >, 0);

    integration_release_htlc (&htlc);
    integration_hope_session_release (&hope);
    integration_close (fd);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/integration/hope_rc4/login_and_ping",
                     test_hope_rc4_login_and_ping);

    return g_test_run ();
}
