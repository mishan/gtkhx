/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_real_tls_login.c — Phase 1 close-out
 * coverage: drive a full login + chat round-trip over the TLS-
 * wrapped HTLS control channel against a real server (Janus today).
 *
 * Sister test to test_real_tls.c, which only exercises the connect-
 * state-machine progression (CONNECTING -> TCP_CONNECTED ->
 * HANDSHAKE_DONE) via production hx_connect. This binary goes the
 * extra mile that docs/tls-scoping.md §9 Phase 1 calls out: actually
 * speak the protocol over the wrapped socket end-to-end.
 *
 * Subtests:
 *
 *   /real_tls_login/login_round_trip
 *     - TLS connect to Janus's tls_port
 *     - HTLC_MAGIC <-> HTLS_MAGIC magic exchange
 *     - HTLC_HDR_LOGIN as guest
 *     - drain to HTLS_HDR_USER_SELFINFO (proves the SELFINFO header
 *       arrived intact through TLS-decrypt — anything less and the
 *       drain returns TASK-with-error or times out)
 *     - clean disconnect
 *
 *   /real_tls_login/chat_round_trip
 *     - everything in login_round_trip, plus
 *     - send HTLC_HDR_CHAT with a unique marker string
 *     - drain for any HTLS_HDR_CHAT broadcast carrying that marker
 *     - assert the body bytes round-trip intact through TLS-encrypt
 *       on send + TLS-decrypt on receive (proves the TLS framing
 *       handles a non-trivial payload, not just the 12+8 byte magic
 *       exchange)
 *
 * Body-text matching uses a random marker rather than filtering on
 * htlc->uid because Janus's SELFINFO doesn't ship the USER_LIST
 * chunk that hx_selfinfo_parse reads uid from — Janus's 1.9-style
 * flow puts session metadata in the TASK login reply instead. The
 * marker-based filter sidesteps the "where does my uid come from"
 * question entirely; if the marker bytes survive the round-trip
 * under TLS that's the thing we care about.
 *
 * Skips loudly (g_test_fail_printf, not g_test_skip) when no TLS-
 * capable matrix entry is configured — per the no-silent-skips
 * feedback, a missing matrix entry should show up as a fail in CI,
 * not a green pass.
 */

#include "config.h"

#include <string.h>
#include <glib.h>
#include <gio/gio.h>

#include "compat.h"
#include "hotline.h"
#include "protocol.h"
#include "proto_helpers.h"
#include "server_matrix.h"
#include "integration_harness.h"
#include "integration_tls.h"

/* Drain budget for chat broadcasts. Same value the fd-based
 * test_chat_roundtrip uses — sub-second cross-talk from concurrent
 * Tier 3 tests can fill the receive buffer with USER_CHANGEs, so 64
 * leaves headroom past the chat broadcast we're waiting for. */
#define TLS_CHAT_DRAIN_BUDGET 64

static const hx_test_server *
pick_tls_server (void)
{
    GPtrArray *cand = hx_test_servers_with (HX_TEST_CAP_TLS);
    if (!cand) {
        return NULL;
    }
    const hx_test_server *picked = NULL;
    for (guint i = 0; i < cand->len; i++) {
        const hx_test_server *s = g_ptr_array_index (cand, i);
        if (s->tls_port != 0) {
            picked = s;
            break;
        }
    }
    g_ptr_array_unref (cand);
    return picked;
}

static void
test_login_round_trip (void)
{
    const hx_test_server *srv = pick_tls_server ();
    if (!srv) {
        g_test_fail_printf (
            "no TLS-capable server in matrix; need HX_TEST_CAP_TLS + "
            "tls_port. Run with GTKHX_TEST_SERVERS=janus or start "
            "the Janus container with TLS ports mapped.");
        return;
    }

    struct htlc_conn htlc;
    GIOStream *io = integration_open_login_tls_or_skip (
        srv, &htlc, "TLS-Login Tier-3", 412);
    if (!io) {
        /* integration_open_login_tls_or_skip already called
         * g_test_fail_printf with the specific failure mode. */
        return;
    }

    /* Reaching here means the TLS wrap carried the full handshake
     * + LOGIN + SELFINFO sequence without dropping or scrambling
     * any framing. The drain inside integration_open_login_tls_or_
     * skip returned HTLS_HDR_USER_SELFINFO; if the TLS layer had
     * mangled bytes the magic exchange would've failed earlier,
     * and if LOGIN had been corrupted the server would've replied
     * with a task-error (which the drain reports as a failure).
     * Pin the contract by sanity-checking htlc->in has a parsed
     * header buffer of at least the wire-header size. */
    g_assert_cmpuint (htlc.in.len, >=, SIZEOF_HL_HDR);

    integration_release_htlc (&htlc);
    integration_close_stream (io);
}

static void
test_chat_round_trip (void)
{
    const hx_test_server *srv = pick_tls_server ();
    if (!srv) {
        g_test_fail_printf (
            "no TLS-capable server in matrix; need HX_TEST_CAP_TLS + "
            "tls_port.");
        return;
    }

    struct htlc_conn htlc;
    GIOStream *io = integration_open_login_tls_or_skip (
        srv, &htlc, "TLS-Chat Tier-3", 412);
    if (!io) {
        return;
    }

    /* Random marker token so parallel runs / repeat invocations
     * don't false-positive on each other's broadcasts. The non-
     * ASCII suffix doubles as a TLS-framing stress payload —
     * snowman + Japanese covers a few multi-byte UTF-8 ranges. */
    char marker[64];
    g_snprintf (marker, sizeof (marker),
                "TLS-marker-%08x \xe2\x98\x83 \xe6\x97\xa5\xe6\x9c\xac",
                g_random_int ());

    g_assert_true (integration_send_chat_stream (io, &htlc, marker));

    /* Drain for any chat broadcast carrying our marker. Don't
     * filter on uid — Janus's SELFINFO doesn't populate htlc->uid
     * (see the file preamble for the why) and the marker is unique
     * enough that no other concurrent test can race us. The drain
     * loop also tolerates interleaved USER_CHANGE / banner / agreement
     * frames the server may send between our send and the echo. */
    struct hx_chat_msg cm;
    gboolean found = FALSE;
    for (int i = 0; i < TLS_CHAT_DRAIN_BUDGET; i++) {
        if (!integration_recv_message_stream (io, &htlc,
                                              /*timeout_ms=*/3000)) {
            break;
        }
        if (hdr_type (&htlc) != HTLS_HDR_CHAT) {
            continue;
        }
        if (!hx_chat_extract (&htlc, &cm)) {
            continue;
        }
        if (g_strstr_len (cm.text, cm.text_len, marker) != NULL) {
            found = TRUE;
            break;
        }
    }
    g_assert_true (found);

    /* cid 0 = main public chat — the only chat we ever joined. */
    g_assert_cmphex (cm.cid, ==, 0);

    integration_release_htlc (&htlc);
    integration_close_stream (io);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/real_tls_login/login_round_trip",
                     test_login_round_trip);
    g_test_add_func ("/real_tls_login/chat_round_trip",
                     test_chat_round_trip);

    return g_test_run ();
}
