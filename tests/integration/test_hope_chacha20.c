/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_hope_chacha20.c — Tier 3 end-to-end test for
 * the HOPE-Secure-Login + ChaCha20-Poly1305 AEAD path.
 *
 * Coverage:
 *
 *   1. integration_open_login_hope_or_skip drives the full HOPE
 *      handshake (TCP + magic, Step 1 LOGIN with algorithm
 *      negotiation, Step 1 reply parsing, HMAC chain, Step 2
 *      authenticated LOGIN, AEAD key derivation, post-login drain
 *      to SELFINFO).
 *   2. After the handshake we send HTLC_HDR_PING under the AEAD
 *      framing and drain to the matching TASK reply — proving the
 *      seal/open codec round-trips against a real server, not just
 *      our own decoder.
 *
 * The test filters the matrix for HX_TEST_CAP_CHACHA20. Today only
 * Janus advertises that capability; when no chacha20-capable server
 * is configured (e.g. CI runs with GTKHX_TEST_SERVERS=mhxd), the
 * test calls g_test_skip and exits cleanly.
 *
 * Pre-refactor this test was infeasible: the harness had no HOPE
 * primitives and the production HOPE flow was wedged inside
 * rcv_task_login (GTK / Adwaita / GtkhxSession). The src/hope.c
 * extraction + src/login_packet.c sharing + harness AEAD wrappers
 * landed in earlier commits on this branch specifically to unblock
 * this test.
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

/* Pick the first ChaCha20-capable server in the matrix, or NULL if
 * none survived the GTKHX_TEST_SERVERS env filter. */
static const hx_test_server *
pick_chacha20_server (void)
{
    GPtrArray *candidates = hx_test_servers_with (HX_TEST_CAP_CHACHA20);
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
test_hope_chacha20_login_and_ping (void)
{
#ifndef CONFIG_CIPHER
    g_test_skip ("built without CONFIG_CIPHER");
    return;
#else
    const hx_test_server *srv = pick_chacha20_server ();
    if (!srv) {
        g_test_skip ("no HX_TEST_CAP_CHACHA20 server in the matrix. "
                     "The Janus container at tests/janus/ advertises this "
                     "cap by default — bring it up with "
                     "`docker run -p 5510:5500 -p 5511:5501 gtkhx-janus`. "
                     "If GTKHX_TEST_SERVERS is filtering Janus out, "
                     "include it in the list.");
        return;
    }

    struct htlc_conn htlc;
    integration_hope_session hope;
    /* Janus's bundled guest account ships with an empty password
	 * (bcrypt-of-empty in the YAML, no HOPEPassword: field). At
	 * HOPE login time the server computes HMAC(key="", session_key)
	 * directly and compares — no stored HOPEPassword needed. This
	 * is the path production GtkHx uses against hotline.vespernet
	 * .net's guest account with cipher=CHACHA20-POLY1305, verified
	 * by tracing a live connection. The PATCH-the-password path
	 * via Janus's admin API writes a HOPEPassword: field but
	 * doesn't validate at login (likely a Janus issue) — see the
	 * preamble of tests/janus/seed-hope-passwords.sh for the full
	 * write-up. */
    int fd = integration_open_login_hope_or_skip (
        srv, &htlc, &hope,
        /*username=*/"guest",
        /*password=*/"",
        /*display_name=*/"HopeChaCha Tier-3",
        /*icon=*/412,
        /*cipheralg=*/"CHACHA20-POLY1305",
        /*compressalg=*/NULL);
    if (fd < 0) {
        /* Harness already called g_test_fail_printf with a
		 * diagnostic. Anything we'd want to special-case here
		 * (e.g. a known container limitation) would deserve a
		 * skip-instead-of-fail conversion, but with the
		 * Dockerfile-side HOPE seeding in place there's no such
		 * known limitation left. Just return. */
        return;
    }

    /* If we got here, the HOPE state machine completed without
     * task-error, AEAD was negotiated, and the harness has switched
     * to framed I/O. Verify the negotiation result. */
    g_assert_true (hope.aead_active);

    /* Now exercise the AEAD wire end-to-end: send a PING through
     * the encryptor, drain to the TASK reply through the decryptor.
     * Janus's PING handler echoes the trans field; any AEAD framing
     * bug would surface as either an open() failure (g_critical),
     * a wrong trans, or a hung wait. */
    guint32 ping_trans = htlc.trans;
    g_assert_true (integration_send_message_hope (
        fd, &htlc, &hope, HTLC_HDR_PING, /*flag=*/0, /*hc=*/0));

    /* drain_until_task_trans pulls plain frames; for AEAD we use a
	 * mini loop that calls integration_recv_message_hope. */
    gboolean got_reply = FALSE;
    for (int i = 0; i < 16 && !got_reply; i++) {
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

    integration_release_htlc (&htlc);
    integration_hope_session_release (&hope);
    integration_close (fd);
#endif /* CONFIG_CIPHER */
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/integration/hope_chacha20/login_and_ping",
                     test_hope_chacha20_login_and_ping);

    return g_test_run ();
}
