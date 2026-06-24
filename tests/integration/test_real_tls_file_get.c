/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_real_tls_file_get.c — Tier 3 coverage for
 * docs/tls-scoping.md Phase 2: HTXF file transfer over TLS.
 *
 * Mirror of tests/integration/test_file_get.c but with both the
 * control channel AND the HTXF subchannel wrapped in TLS. Drives
 * the full transfer flow end-to-end against Janus's TLS ports
 * (5610 / 5611 host-mapped) to prove:
 *
 *   - hx_sync_connect_to_host's new tls parameter wires the
 *     subchannel through GTlsClientConnection cleanly.
 *   - The 16-byte plaintext HTXF preamble survives the TLS wrap
 *     (the server matches the subchannel to the queued transfer
 *     by ref before any cipher state is available — see
 *     network.c::htxf_connect's preamble comment).
 *   - The streamed file body (FILP-wrapped seed bytes) round-
 *     trips through TLS encrypt/decrypt without mangling.
 *
 * Uses the GIOStream-based harness in integration_tls.{c,h} for
 * both connections. Pulls the seed-bytes substring check straight
 * from the plaintext sibling — Janus seeds files/test.txt with
 * "hello world\n" the same way mhxd does.
 */

#include "config.h"

#include <string.h>
#include <glib.h>
#include <gio/gio.h>
#include <netinet/in.h>

#include "compat.h"
#include "hotline.h"
#include "protocol.h"
#include "proto_helpers.h"
#include "server_matrix.h"
#include "integration_harness.h"
#include "integration_tls.h"
#include "htxf_io.h"
#include <errno.h>

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
        if (s->tls_port != 0 && s->tls_xfer_port != 0) {
            picked = s;
            break;
        }
    }
    g_ptr_array_unref (cand);
    return picked;
}

static void
test_file_get_round_trip_tls (void)
{
    const hx_test_server *srv = pick_tls_server ();
    if (!srv) {
        g_test_fail_printf (
            "no TLS-capable server in matrix; need HX_TEST_CAP_TLS + "
            "tls_port + tls_xfer_port. Run with GTKHX_TEST_SERVERS=janus "
            "and the Janus container with -p 5610:5600 -p 5611:5601.");
        return;
    }

    struct htlc_conn htlc;
    GIOStream *ctrl = integration_open_login_tls_or_skip (
        srv, &htlc, "TLS-FileGet Tier-3", 412);
    if (!ctrl) {
        return;
    }

    /* Ask for files/test.txt — same shape as the plaintext
     * file_get test. Janus seeds it with "hello world\n". */
    const char *fname = "test.txt";
    guint32 our_trans = htlc.trans;
    g_assert_true (integration_send_message_stream (
        ctrl, &htlc, HTLC_HDR_FILE_GET, /*flag=*/0, /*hc=*/1,
        (int) HTLC_DATA_FILE_NAME, (int) strlen (fname),
        (guint8 *) fname));

    g_assert_true (integration_drain_until_task_trans_stream (
        ctrl, &htlc, our_trans, 64));

    if (hdr_flag (&htlc) & 1) {
        char err[256];
        gsize err_len = 0;
        if (task_error_extract (&htlc, err, sizeof (err), &err_len)) {
            g_test_fail_printf ("file_get refused by server: \"%s\". "
                                "Is files/test.txt seeded in the Janus "
                                "container?", err);
        } else {
            g_test_fail_printf ("file_get refused (no error chunk)");
        }
        integration_release_htlc (&htlc);
        integration_close_stream (ctrl);
        return;
    }

    /* Pull HTXF_REF + HTXF_SIZE out via the shared walker. */
    struct hx_htxf_reply reply = { 0 };
    hx_htxf_reply_extract (&htlc, &reply);
    g_assert_cmphex (reply.ref, !=, 0);
    g_assert_cmpuint (reply.size, >, 0);
    g_assert_cmpuint (reply.size, <, 1024 * 1024);
    guint32 xfer_ref = reply.ref, xfer_size = reply.size;

    /* Open the TLS-wrapped HTXF subchannel. Same accept-everything
     * cert stub as the control channel. */
    GIOStream *xfer = hx_test_server_connect_xfer_tls (srv);
    if (!xfer) {
        g_test_fail_printf (
            "TLS HTXF subchannel port (%u) isn't reachable; make sure "
            "the Janus container has it mapped (-p %u:5601).",
            (unsigned) srv->tls_xfer_port,
            (unsigned) srv->tls_xfer_port);
        integration_release_htlc (&htlc);
        integration_close_stream (ctrl);
        return;
    }

    /* Send the 16-byte HTXF preamble. The preamble is plaintext
     * in the legacy spec — over TLS it still rides the encrypted
     * stream, but logically it's the same bytes the server reads
     * to match the subchannel to the queued transfer. */
    guint8 hdr_buf[SIZEOF_HTXF_HDR];
    hl_htxf_hdr_pack (hdr_buf, xfer_ref, xfer_size, HTXF_TYPE_FILE, 0);
    g_assert_true (integration_send_stream (xfer, hdr_buf, sizeof (hdr_buf)));

    /* Stream the full body off the TLS subchannel through PRODUCTION
     * htxf_io_read (not the harness read), so the TLS byte pump is
     * production-tested — the combo the HTXF→Rust re-wire implements as
     * HtxfChannel over connect_tls()'s rustls stream. Janus TLS file-get
     * is plaintext-login-over-TLS, so the subchannel carries no AEAD:
     * aead_active stays FALSE and htxf_io_read takes its passthrough leg
     * over the (TLS-encrypted) GIOStream. */
    struct htxf_conn xfer_conn;
    memset (&xfer_conn, 0, sizeof (xfer_conn));
    htxf_io_init (&xfer_conn);
    g_assert_false (xfer_conn.aead_active);

    guint8 *payload = g_malloc (xfer_size);
    gsize got = 0;
    while (got < xfer_size) {
        ssize_t r = htxf_io_read (&xfer_conn, xfer, payload + got, xfer_size - got);
        if (r <= 0) {
            g_test_message ("htxf_io_read returned %zd at got=%zu errno=%d (%s)",
                            r, got, errno, g_strerror (errno));
            break;
        }
        got += (gsize) r;
    }
    g_assert_cmpuint ((guint) got, ==, xfer_size);

    /* Substring-search for the seed bytes — the FILP wrapper has
     * fork headers + offsets in front of the actual file content,
     * so we don't try to parse the format. Finding "hello world"
     * intact proves the byte stream round-tripped through TLS
     * encrypt-on-server + decrypt-on-client without scrambling. */
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
    htxf_io_release (&xfer_conn);
    integration_close_stream (xfer);
    integration_release_htlc (&htlc);
    integration_close_stream (ctrl);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/real_tls_file_get/round_trip",
                     test_file_get_round_trip_tls);

    return g_test_run ();
}
