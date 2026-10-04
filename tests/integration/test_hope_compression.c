/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_hope_compression.c — HOPE compression, asked for
 * by name as the connect dialog's Compression row does, against the
 * servers that have it: GZIP under Blowfish on mhxd; ZSTD and LZ4 under
 * ChaCha20-Poly1305, and GZIP and ZSTD under Blowfish, on Janus. Each
 * checks the server took what was asked and that requests and their
 * replies cross both ways compressed. (LZ4 under Blowfish is not offered:
 * see docs/janus-bugs.md.)
 */

#include "config.h"
#include <glib.h>
#include "compat.h"
#include "hotline.h"
#include "protocol.h"
#include "proto_helpers.h"
#include "integration_harness.h"
#include "server_matrix.h"

/* integration_hope_compression's codes. */
#define GZIP 1
#define LZ4 2
#define ZSTD 3

/* Log in to the matrix's HOPE server named `server` with `cipher` and
 * `compress`, and check `want` was agreed and pings cross. */
static void
login_and_ping (const char *server, const char *cipher, const char *compress,
                guint32 want)
{
    GPtrArray *candidates = hx_test_servers_with (HX_TEST_CAP_HOPE);
    const hx_test_server *srv = NULL;
    for (guint i = 0; candidates && i < candidates->len && !srv; i++) {
        const hx_test_server *s = g_ptr_array_index (candidates, i);
        if (g_strcmp0 (s->name, server) == 0) {
            srv = s;
        }
    }
    if (candidates) {
        g_ptr_array_unref (candidates);
    }
    if (!srv) {
        g_test_fail_printf ("no %s with HOPE in the matrix", server);
        return;
    }

    struct htlc_conn htlc;
    integration_hope_session hope;
    int fd = integration_open_login_hope_or_skip (srv, &htlc, &hope, "guest",
                                                  "", "HopeCompress Tier-3",
                                                  412, cipher, compress);
    if (fd < 0) {
        return;
    }
    g_assert_cmpuint (integration_hope_compression (fd), ==, want);

    for (int i = 0; i < 16; i++) {
        guint32 ping_trans = htlc.trans;
        g_assert_true (integration_send_message_hope (
            fd, &htlc, &hope, HTLC_HDR_PING, /*flag=*/0, /*hc=*/0));
        gboolean got_reply = FALSE;
        for (int j = 0; j < 16 && !got_reply; j++) {
            if (!integration_recv_message_hope (fd, &htlc, &hope, 5000)) {
                break;
            }
            got_reply = hdr_type (&htlc) == HTLS_HDR_TASK
                        && hdr_trans (&htlc) == ping_trans;
        }
        g_assert_true (got_reply);
        g_assert_cmphex (hdr_flag (&htlc) & 1, ==, 0);
    }

    integration_release_htlc (&htlc);
    integration_hope_session_release (&hope);
    integration_close (fd);
}

static void
test_gzip_on_mhxd (void)
{
    login_and_ping ("mhxd", "BLOWFISH", "GZIP", GZIP);
}

static void
test_blowfish_gzip_on_janus (void)
{
    login_and_ping ("janus", "BLOWFISH", "GZIP", GZIP);
}

static void
test_blowfish_zstd_on_janus (void)
{
    login_and_ping ("janus", "BLOWFISH", "ZSTD", ZSTD);
}

static void
test_zstd_on_janus (void)
{
    login_and_ping ("janus", "CHACHA20-POLY1305", "ZSTD", ZSTD);
}

static void
test_lz4_on_janus (void)
{
    login_and_ping ("janus", "CHACHA20-POLY1305", "LZ4", LZ4);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/integration/hope_compression/gzip_on_mhxd",
                     test_gzip_on_mhxd);
    g_test_add_func ("/integration/hope_compression/zstd_on_janus",
                     test_zstd_on_janus);
    g_test_add_func ("/integration/hope_compression/lz4_on_janus",
                     test_lz4_on_janus);
    g_test_add_func ("/integration/hope_compression/blowfish_gzip_on_janus",
                     test_blowfish_gzip_on_janus);
    g_test_add_func ("/integration/hope_compression/blowfish_zstd_on_janus",
                     test_blowfish_zstd_on_janus);
    return g_test_run ();
}
