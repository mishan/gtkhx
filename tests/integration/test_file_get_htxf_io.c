/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_file_get_htxf_io.c — Tier 3 coverage for the
 * PLAINTEXT (no-AEAD) HTXF subchannel byte pump, driven through the
 * PRODUCTION htxf_io_read path.
 *
 * Why this exists: the other plaintext transfer tests (test_file_get,
 * test_folder_get, test_banner) reimplement the subchannel read in the
 * harness (integration_recv), so they don't exercise production's
 * htxf_io.c. The AEAD subchannel byte pump is production-tested by
 * test_hope_chacha20_banner (htxf_io_read over AEAD frames), but the
 * plaintext passthrough leg of htxf_io_read had no production-path
 * coverage at all — every htxf_io test armed AEAD first.
 *
 * That gap matters because the HTXF→Rust migration (H2,
 * docs/htxf-rust-migration-scoping.md) replaces this byte pump with the
 * Rust HtxfChannel: this test pins the current plaintext-download
 * behaviour as the parity oracle the re-wire must reproduce, and is the
 * regression guard for the plaintext mode (mhxd / any non-HOPE server).
 *
 * Flow (mirrors test_file_get's control channel, but reads the body
 * through htxf_io_read with aead_active = FALSE):
 *   1. Log in to mhxd, HTLC_HDR_FILE_GET "test.txt".
 *   2. Pull HTXF_REF + HTXF_SIZE from the TASK reply.
 *   3. Open the subchannel, send the 16-byte HTXF preamble.
 *   4. Read `size` body bytes via PRODUCTION htxf_io_read over a
 *      GIOStream, with a zero-initialised (plaintext) htxf_conn.
 *   5. Substring-check the seed bytes ("hello world").
 */

#include "config.h"

#include <errno.h>
#include <string.h>
#include <glib.h>
#include <gio/gio.h>

#include "compat.h"
#include "hotline.h"
#include "protocol.h"
#include "proto_helpers.h"
#include "htxf_io.h"
#include "integration_harness.h"

static void
test_file_get_htxf_io_plaintext (void)
{
    struct htlc_conn htlc;
    int fd = integration_open_login_or_skip (&htlc, "FileGetHtxfIo Tier-3", 414);
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
        if (task_error_extract (&htlc, err, sizeof (err), &err_len)) {
            g_test_fail_printf ("file_get refused by server: \"%s\". "
                                "Is files/test.txt seeded in the container?",
                                err);
        } else {
            g_test_fail_printf ("file_get refused (no error chunk)");
        }
        integration_release_htlc (&htlc);
        integration_close (fd);
        return;
    }

    struct hx_htxf_reply reply = { 0 };
    hx_htxf_reply_extract (&htlc, &reply);
    g_assert_cmphex (reply.ref, !=, 0);
    g_assert_cmpuint (reply.size, >, 0);
    g_assert_cmpuint (reply.size, <, 1024 * 1024);
    guint32 xfer_ref = reply.ref, xfer_size = reply.size;

    int xfd = integration_connect_xfer ();
    if (xfd < 0) {
        g_test_fail_printf ("HTXF subchannel port (5501 by default) isn't "
                            "reachable; publish it (-p 5501:5501) or set "
                            "GTKHX_TEST_XFER_PORT.");
        integration_release_htlc (&htlc);
        integration_close (fd);
        return;
    }

    /* 16-byte HTXF preamble, plaintext (matches production
     * network.c::htxf_connect — the preamble always travels in the
     * clear, before any cipher state exists). */
    g_assert_true (integration_send_xfer_hdr (xfd, xfer_ref, xfer_size));

    /* Wrap the raw fd in a GSocketConnection so we can hand a GIOStream
     * to htxf_io_read (the GSocket adopts the fd). */
    GError *err = NULL;
    GSocket *xfer_sock = g_socket_new_from_fd (xfd, &err);
    g_assert_no_error (err);
    g_assert_nonnull (xfer_sock);
    GSocketConnection *xfer_conn =
        g_socket_connection_factory_create_connection (xfer_sock);
    g_object_unref (xfer_sock);
    g_assert_nonnull (xfer_conn);
    GIOStream *xfer_io = G_IO_STREAM (xfer_conn);

    /* PLAINTEXT htxf_conn: htxf_io_init leaves aead_active = FALSE, so
     * htxf_io_read takes its passthrough leg (a plain
     * g_input_stream_read), NOT aead_read. This is the production code
     * the Rust HtxfChannel's new_plain() path replaces. */
    struct htxf_conn xfer;
    memset (&xfer, 0, sizeof (xfer));
    htxf_io_init (&xfer);
    g_assert_false (xfer.aead_active);

    guint8 *payload = g_malloc (xfer_size);
    gsize got = 0;
    while (got < xfer_size) {
        ssize_t r = htxf_io_read (&xfer, xfer_io, payload + got, xfer_size - got);
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
    g_object_unref (xfer_conn);
    htxf_io_release (&xfer);
    integration_release_htlc (&htlc);
    integration_close (fd);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/integration/file_get_htxf_io/plaintext_passthrough",
                     test_file_get_htxf_io_plaintext);
    return g_test_run ();
}
