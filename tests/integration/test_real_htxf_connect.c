/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_real_htxf_connect.c — Tier 3 coverage for the
 * PRODUCTION HTXF subchannel connect, network.c::htxf_connect.
 *
 * Every other HTXF transfer test either reimplements the subchannel in
 * the harness (test_file_get, test_hope_chacha20_banner) or reads the
 * body via the harness (test_real_tls_file_get) — none of them actually
 * call htxf_connect. That left the production connect + preamble + AEAD
 * arm path with zero Tier 3 coverage, which is exactly the function the
 * HTXF→Rust H2 re-wire rewrites. This test closes that gap: it is the
 * worker-level regression net the re-wire flips against.
 *
 * Flow: drive the control channel through the harness for the
 * FILE_GET → HTXF_REF/SIZE handshake, then call the REAL htxf_connect
 * for the subchannel (which opens the socket, sends the 16-byte
 * preamble, and arms AEAD off the control channel's cipher state — here
 * none, since mhxd guest is plaintext, so the subchannel stays
 * passthrough), and drain the body through htxf_io_read. Substring-
 * check the seed bytes to prove the connect + drain round-tripped.
 *
 * Links production network.c (for htxf_connect) alongside the harness
 * control helpers — the harness's hlwrite_chunks / hx_htlc_close stubs
 * are __attribute__((weak)) so network.c's strong definitions win.
 */

#include "config.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <gio/gio.h>

#include "compat.h"
#include "hotline.h"
#include "protocol.h"
#include "proto_helpers.h"
#include "cipher.h"              /* CIPHER_MODE_AEAD */
#include "htxf_io.h"
#include "network.h"             /* htxf_connect */
#include "integration_harness.h"
#include "server_matrix.h"

/* Shared body-drain + substring check used by all three siblings.
 * Drains exactly xfer_size bytes through production htxf_io_read
 * (passthrough, AEAD, or TLS leg depending on how htxf was armed)
 * and asserts the seed bytes "hello world" appear. */
static void
drain_and_check (struct htxf_conn *htxf, guint64 xfer_size)
{
    guint8 *payload = g_malloc (xfer_size);
    gsize got = 0;
    while (got < xfer_size) {
        ssize_t r = htxf_io_read (htxf, payload + got, xfer_size - got);
        if (r <= 0) {
            g_test_message ("htxf_io_read returned %zd at got=%zu errno=%d (%s)",
                            r, got, errno, g_strerror (errno));
            break;
        }
        got += (gsize) r;
    }
    g_assert_cmpuint ((guint) got, ==, xfer_size);

    const char *needle = "hello world";
    gsize needle_len = strlen (needle);
    gboolean found = FALSE;
    for (gsize i = 0; i + needle_len <= xfer_size; i++) {
        if (memcmp (payload + i, needle, needle_len) == 0) {
            found = TRUE;
            break;
        }
    }
    g_assert_true (found);
    g_free (payload);
}

static void
test_htxf_connect_file_get_plaintext (void)
{
    struct htlc_conn htlc;
    int fd = integration_open_login_or_skip (&htlc, "HtxfConnect Tier-3", 416);
    if (fd < 0) {
        return;
    }

    const char *fname = "test.txt";
    guint32 our_trans = htlc.trans;
    g_assert_true (integration_send_message (
        fd, &htlc, HTLC_HDR_FILE_GET, /*flag=*/0, /*hc=*/1,
        (int) HTLC_DATA_FILE_NAME, (int) strlen (fname), (guint8 *) fname));

    g_assert_true (integration_drain_until_task_trans (fd, &htlc, our_trans, 64));

    if (hdr_flag (&htlc) & 1) {
        char err[256];
        gsize err_len = 0;
        if (task_error_extract (htlc.in.buf, htlc.in.pos, err, sizeof (err), &err_len)) {
            g_test_fail_printf ("file_get refused by server: \"%s\". Is "
                                "files/test.txt seeded in the container?",
                                err);
        } else {
            g_test_fail_printf ("file_get refused (no error chunk)");
        }
        integration_release_htlc (&htlc);
        integration_close (fd);
        return;
    }

    struct hx_htxf_reply reply = { 0 };
    hx_htxf_reply_extract (htlc.in.buf, htlc.in.pos, &reply);
    g_assert_cmphex (reply.ref, !=, 0);
    g_assert_cmpuint (reply.size, >, 0);
    g_assert_cmpuint (reply.size, <, 1024 * 1024);
    guint32 xfer_ref = reply.ref;
    guint64 xfer_size = reply.size;

    /* The subchannel host/port: same host as the control channel, the
     * HTXF port (5501 by default; mhxd serves transfers there). */
    const char *host = g_getenv ("GTKHX_TEST_HOST");
    if (!host || !*host) {
        host = "127.0.0.1";
    }
    const char *xfer_port_s = g_getenv ("GTKHX_TEST_XFER_PORT");
    guint16 xfer_port =
        (xfer_port_s && *xfer_port_s) ? (guint16) atoi (xfer_port_s) : 5501;

    /* Zero-initialised htxf_conn: htlc carries no AEAD cipher state
     * (mhxd guest is plaintext), so htxf_connect leaves the subchannel
     * in passthrough mode (aead_active = FALSE). */
    struct htxf_conn htxf;
    memset (&htxf, 0, sizeof (htxf));
    htxf_io_init (&htxf);
    htxf.htlc = &htlc;
    g_strlcpy (htxf.serverhost, host, sizeof (htxf.serverhost));
    htxf.serverport = xfer_port;
    htxf.ref = xfer_ref;
    htxf.total_size = xfer_size;

    /* THE function under test: production htxf_connect. */
    if (!htxf_connect (&htxf)) {
        g_test_fail_printf ("htxf_connect failed (HTXF port %u reachable? "
                            "publish -p %u:%u or set GTKHX_TEST_XFER_PORT).",
                            (unsigned) xfer_port, (unsigned) xfer_port,
                            (unsigned) xfer_port);
        integration_release_htlc (&htlc);
        integration_close (fd);
        return;
    }
    g_assert_false (htxf.aead_active);

    /* Drain the body through production htxf_io_read (passthrough leg). */
    drain_and_check (&htxf, xfer_size);

    htxf_io_release (&htxf);
    integration_release_htlc (&htlc);
    integration_close (fd);
}

/*
 * AEAD sibling: same production htxf_connect, but the control channel
 * negotiated HOPE-ChaCha20-Poly1305. htxf_connect hands the control
 * connection's retained HOPE material handle (htlc->hope_aead) to
 * hxnet_htxf_open, which derives the per-transfer ChaCha20-Poly1305
 * keys in-process (mixing in ref) and frames the body AEAD — the
 * session key never crosses back into C. Under orchestration the login
 * helper seeds htlc->hope_aead; on the legacy transport we build the
 * handle from the harness's own C handshake state. Body drains through
 * the AEAD leg of htxf_io_read; a correct "hello world" round-trip is
 * the proof AEAD armed.
 */
static const hx_test_server *
pick_aead_server (void)
{
    GPtrArray *servers = hx_test_servers_with (HX_TEST_CAP_CHACHA20);
    if (!servers) {
        return NULL;
    }
    const hx_test_server *srv =
        (servers->len > 0) ? g_ptr_array_index (servers, 0) : NULL;
    g_ptr_array_unref (servers);
    return srv;
}

static void
test_htxf_connect_file_get_aead (void)
{
    const hx_test_server *srv = pick_aead_server ();
    if (!srv) {
        g_test_fail_printf (
            "no HX_TEST_CAP_CHACHA20 server in matrix; run with "
            "GTKHX_TEST_SERVERS=janus and the Janus container up.");
        return;
    }

    struct htlc_conn htlc;
    integration_hope_session hope;
    int fd = integration_open_login_hope_or_skip (
        srv, &htlc, &hope, /*username=*/"guest", /*password=*/"",
        /*display_name=*/"HtxfConnect-AEAD Tier-3", /*icon=*/416,
        /*cipheralg=*/"CHACHA20-POLY1305", /*compressalg=*/NULL);
    if (fd < 0) {
        return;
    }
    /* AEAD negotiation is a harness-crypto fact only on the legacy
     * transport. Under orchestration the production actor (Rust) owns
     * the cipher and the harness hope session stays zeroed — the
     * encrypted body round-trip via htxf_io_read below is the
     * end-to-end proof. (Same gating as test_hope_chacha20_banner.) */

    /* FILE_GET test.txt over the AEAD control channel. */
    const char *fname = "test.txt";
    guint32 our_trans = htlc.trans;
    g_assert_true (integration_send_message_hope (
        fd, &htlc, &hope, HTLC_HDR_FILE_GET, /*flag=*/0, /*hc=*/1,
        (int) HTLC_DATA_FILE_NAME, (int) strlen (fname), (guint8 *) fname));

    struct hx_htxf_reply reply = { 0 };
    gboolean got_reply = FALSE;
    for (int i = 0; i < 16 && !got_reply; i++) {
        if (!integration_recv_message_hope (fd, &htlc, &hope,
                                            /*timeout_ms=*/3000)) {
            break;
        }
        if (hdr_type (&htlc) != HTLS_HDR_TASK
            || hdr_trans (&htlc) != our_trans) {
            continue;
        }
        got_reply = TRUE;
        if (hdr_flag (&htlc) & 1) {
            char err[256];
            gsize err_len = 0;
            if (task_error_extract (htlc.in.buf, htlc.in.pos, err, sizeof (err), &err_len)) {
                g_test_fail_printf ("file_get refused: \"%s\"", err);
            } else {
                g_test_fail_printf ("file_get refused (no error chunk)");
            }
            integration_release_htlc (&htlc);
            integration_close (fd);
            return;
        }
        hx_htxf_reply_extract (htlc.in.buf, htlc.in.pos, &reply);
    }
    g_assert_true (got_reply);
    g_assert_cmphex (reply.ref, !=, 0);
    g_assert_cmpuint (reply.size, >, 0);
    g_assert_cmpuint (reply.size, <, 1024 * 1024);

    /* htxf_connect derives the per-transfer ChaCha20-Poly1305 keys
     * INSIDE hxnet_htxf_open from the control connection's retained HOPE
     * material (htlc->hope_aead, an opaque handle) plus this transfer's
     * ref — the session key never crosses back into C. The orchestrated
     * login already seeded htlc.hope_aead from the production actor
     * (mirroring rcv_task_login); it's an owned handle that
     * integration_release_htlc frees at teardown. */
    g_assert_nonnull (htlc.hope_aead);

    struct htxf_conn htxf;
    memset (&htxf, 0, sizeof (htxf));
    htxf_io_init (&htxf);
    htxf.htlc = &htlc;
    g_strlcpy (htxf.serverhost, srv->host, sizeof (htxf.serverhost));
    htxf.serverport = srv->xfer_port;
    htxf.ref = reply.ref;
    htxf.total_size = reply.size;

    if (!htxf_connect (&htxf)) {
        g_test_fail_printf ("htxf_connect failed (HTXF port %u on %s "
                            "reachable?)",
                            (unsigned) srv->xfer_port, srv->host);
        integration_release_htlc (&htlc);
        integration_close (fd);
        return;
    }

    /* htxf_connect no longer flips htxf->aead_active — AEAD framing now
     * lives entirely inside the hxnet channel (keyed off the hope_aead
     * handle). The proof that AEAD armed is the body round-trip: if the
     * subchannel were running plaintext passthrough, the AEAD-framed
     * bytes on the wire wouldn't decode to "hello world". */
    drain_and_check (&htxf, reply.size);

    htxf_io_release (&htxf);
    integration_release_htlc (&htlc);
    integration_close (fd);
}

/*
 * TLS sibling: control channel over TLS (Janus 5610), subchannel over
 * TLS too (Janus 5611). htxf_connect mirrors htlc->tls onto the
 * subchannel — so setting htlc.tls = 1 makes it open a TLS-wrapped
 * subchannel via hx_sync_connect_to_host. The self-signed cert is
 * accepted via GTKHX_TLS_AUTO_ACCEPT (set by the harness around this
 * test). No cipher state, so the body stays plaintext-inside-TLS and
 * drains through the passthrough leg of htxf_io_read.
 */
static const hx_test_server *
pick_tls_xfer_server (void)
{
    GPtrArray *cand = hx_test_servers_with (HX_TEST_CAP_TLS);
    if (!cand) {
        return NULL;
    }
    const hx_test_server *picked = NULL;
    for (guint i = 0; i < cand->len; i++) {
        const hx_test_server *s = g_ptr_array_index (cand, i);
        if (s->tls_port != 0 && s->tls_xfer_port != 0) {
            picked = s;
            break;
        }
    }
    g_ptr_array_unref (cand);
    return picked;
}

static void
test_htxf_connect_file_get_tls (void)
{
    const hx_test_server *srv = pick_tls_xfer_server ();
    if (!srv) {
        g_test_fail_printf (
            "no TLS-capable server with tls_port + tls_xfer_port in "
            "matrix; run with GTKHX_TEST_SERVERS=janus and the Janus "
            "container with -p 5610:5600 -p 5611:5601.");
        return;
    }

    /* The self-signed Janus cert is accepted via GTKHX_TLS_AUTO_ACCEPT,
     * set once in main() before any threads spawn (g_setenv after
     * thread creation is not thread-safe). */
    struct htlc_conn htlc;
    int ctrl = integration_open_login_tls_or_skip (
        srv, &htlc, "HtxfConnect-TLS Tier-3", 416);
    if (ctrl < 0) {
        return;
    }

    const char *fname = "test.txt";
    guint32 our_trans = htlc.trans;
    g_assert_true (integration_send_message (
        ctrl, &htlc, HTLC_HDR_FILE_GET, /*flag=*/0, /*hc=*/1,
        (int) HTLC_DATA_FILE_NAME, (int) strlen (fname), (guint8 *) fname));

    g_assert_true (integration_drain_until_task_trans (
        ctrl, &htlc, our_trans, 64));

    if (hdr_flag (&htlc) & 1) {
        char err[256];
        gsize err_len = 0;
        if (task_error_extract (htlc.in.buf, htlc.in.pos, err, sizeof (err), &err_len)) {
            g_test_fail_printf ("file_get refused: \"%s\"", err);
        } else {
            g_test_fail_printf ("file_get refused (no error chunk)");
        }
        integration_release_htlc (&htlc);
        integration_close (ctrl);
        return;
    }

    struct hx_htxf_reply reply = { 0 };
    hx_htxf_reply_extract (htlc.in.buf, htlc.in.pos, &reply);
    g_assert_cmphex (reply.ref, !=, 0);
    g_assert_cmpuint (reply.size, >, 0);
    g_assert_cmpuint (reply.size, <, 1024 * 1024);

    /* Drive the subchannel over TLS: htlc.tls = 1 makes htxf_connect
     * connect to tls_xfer_port with a rustls wrap. */
    htlc.tls = 1;

    struct htxf_conn htxf;
    memset (&htxf, 0, sizeof (htxf));
    htxf_io_init (&htxf);
    htxf.htlc = &htlc;
    g_strlcpy (htxf.serverhost, srv->host, sizeof (htxf.serverhost));
    htxf.serverport = srv->tls_xfer_port;
    htxf.ref = reply.ref;
    htxf.total_size = reply.size;

    if (!htxf_connect (&htxf)) {
        g_test_fail_printf ("htxf_connect (TLS) failed (TLS HTXF port %u "
                            "on %s reachable?)",
                            (unsigned) srv->tls_xfer_port, srv->host);
        integration_release_htlc (&htlc);
        integration_close (ctrl);
        return;
    }
    g_assert_false (htxf.aead_active);

    drain_and_check (&htxf, reply.size);

    htxf_io_release (&htxf);
    integration_release_htlc (&htlc);
    integration_close (ctrl);
}

int
main (int argc, char **argv)
{
    /* Accept the self-signed Janus cert on the TLS subchannel. Set
     * before g_test_init / any thread spawns — g_setenv after thread
     * creation is not thread-safe. Inert for the plaintext + AEAD
     * subtests (their subchannels never go through TLS). */
    g_setenv ("GTKHX_TLS_AUTO_ACCEPT", "1", TRUE);

    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/integration/htxf_connect/file_get_plaintext",
                     test_htxf_connect_file_get_plaintext);
    g_test_add_func ("/integration/htxf_connect/file_get_aead",
                     test_htxf_connect_file_get_aead);
    g_test_add_func ("/integration/htxf_connect/file_get_tls",
                     test_htxf_connect_file_get_tls);
    return g_test_run ();
}
