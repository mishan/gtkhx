/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_file_get.c — download a file from mhxd over
 * the HTXF subchannel, driving the PRODUCTION receive state machine
 * (xfers_recv.c::file_recv_one), and assert the decoded on-disk file is
 * byte-for-byte correct.
 *
 * Unlike a raw substring-search over the streamed bytes, this exercises
 * the real FILP-frame decode: the info-block length, the FILP info
 * parse, the DATA fork-length split, and the fork write to disk. That is
 * the path a regression like "wrong length passed to the FILP parser"
 * lives in — a substring search over the raw stream can't see it.
 *
 * Flow:
 *   1. Login on the main port (5500), send HTLC_HDR_FILE_GET for
 *      test.txt, read the TASK reply's HTXF_REF + HTXF_SIZE.
 *   2. Open the subchannel (5501), send the 16-byte preamble, and wrap
 *      the fd as an hxnet HTXF channel (plaintext passthrough — mhxd
 *      guest is unencrypted).
 *   3. Point a minimal htxf_conn at a temp path and call the real
 *      file_recv_one with a no-op progress hook.
 *   4. Assert the temp file equals the seeded "hello world\n".
 *
 * The Dockerfile seeds files/test.txt with exactly "hello world\n".
 *
 * Links xfers_recv.c + the hxhfs crate (the receive machine + HFS
 * sidecar) on top of integration_harness_lib (which already bundles
 * htxf_io.c and the hxnet channel). file_recv_one's GTK-shell couplings
 * are stubbed below: the preview branch is never taken (opt.preview = 0)
 * and progress is a no-op, so hx_preview_* just needs to resolve at link
 * time.
 */

#include "config.h"
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <glib.h>
#include <glib/gstdio.h>
#include "compat.h"
#include "hotline.h"
#include "protocol.h"
#include "proto_helpers.h"
#include "htxf_io.h"
#include "preview.h"
#include "xfers_recv.h"
#include "integration_harness.h"

/* --- link stubs for file_recv_one's GTK-shell couplings --- *
 * The preview branch of file_recv_one references these three but never
 * runs here (opt.preview = 0). */
void
hx_preview_set_info (hx_preview *p, const char *type, const char *creator)
{
    (void)p;
    (void)type;
    (void)creator;
}
void
hx_preview_chunk (hx_preview *p, const char *buf, gsize len)
{
    (void)p;
    (void)buf;
    (void)len;
}
void
hx_preview_done (hx_preview *p)
{
    (void)p;
}

static void
noop_progress (struct htxf_conn *htxf)
{
    (void)htxf;
}

static void
test_file_get_round_trip (void)
{
    struct htlc_conn htlc;
    int fd = integration_open_login_or_skip (&htlc, "FileGet Tier-3", 412);
    if (fd < 0) {
        return;
    }

    /* FILE_GET files/test.txt from the server root (mhxd's
     * rcv_file_get accepts a bare FILE_NAME as "from ROOTDIR"). */
    const char *fname = "test.txt";
    guint32 our_trans = htlc.trans;
    g_assert_true (integration_send_message (
        fd, &htlc, HTLC_HDR_FILE_GET, /*flag=*/0, /*hc=*/1,
        (int)HTLC_DATA_FILE_NAME, (int)strlen (fname), (guint8 *)fname));

    g_assert_true (integration_drain_until_task_trans (
        fd, &htlc, our_trans, 64));

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
    g_assert_cmpuint (reply.size, <, 1024 * 1024); /* sanity cap */
    guint32 xfer_ref = reply.ref;
    guint64 xfer_size = reply.size;

    /* Open the subchannel and send the 16-byte preamble the way a real
     * download does. */
    int xfd = integration_connect_xfer ();
    if (xfd < 0) {
        g_test_fail_printf ("HTXF subchannel port (5501 by default) isn't "
                            "reachable. Publish -p 5501:5501 or set "
                            "GTKHX_TEST_XFER_PORT.");
        integration_release_htlc (&htlc);
        integration_close (fd);
        return;
    }
    /* The legacy 16-byte HTXF preamble carries a 32-bit size; xfer_size
     * is capped < 1 MiB above, so the narrowing cast is safe. */
    g_assert_true (
        integration_send_xfer_hdr (xfd, xfer_ref, (guint32)xfer_size));

    /* Wrap the connected fd as an hxnet HTXF channel (plaintext, no
     * preamble — we already sent it above). hxnet adopts xfd; the
     * channel is closed by htxf_io_release, so we must not close xfd. */
    const char *host = g_getenv ("GTKHX_TEST_HOST");
    if (!host || !*host) {
        host = "127.0.0.1";
    }
    HtxfConn *ch = hxnet_htxf_open (xfd, /*tls=*/0, (const guint8 *)host,
                                    strlen (host), /*preamble=*/NULL,
                                    /*preamble_len=*/0, /*hope_aead=*/NULL,
                                    xfer_ref, /*verify=*/NULL, /*ud=*/NULL);
    g_assert_nonnull (ch);

    /* A temp directory to receive into, so the decoded file (and any HFS
     * sidecar) is isolated and cleaned up. */
    g_autoptr (GError) tmperr = NULL;
    g_autofree char *tmpdir = g_dir_make_tmp ("gtkhx_fileget_XXXXXX", &tmperr);
    g_assert_no_error (tmperr);
    g_assert_nonnull (tmpdir);

    struct htxf_conn htxf;
    memset (&htxf, 0, sizeof (htxf));
    htxf_io_init (&htxf);
    htxf.hx = ch;
    htxf.total_size = xfer_size;
    g_snprintf (htxf.path, sizeof (htxf.path), "%s/out.txt", tmpdir);

    /* THE code under test: the production single-file receive machine. */
    guint8 buf[1024];
    int rv = file_recv_one (&htxf, xfer_size, buf, noop_progress);
    g_assert_cmpint (rv, ==, 0);

    /* The decoded data fork must be byte-for-byte the seeded content. */
    g_autofree char *content = NULL;
    gsize clen = 0;
    g_autoptr (GError) rderr = NULL;
    g_assert_true (g_file_get_contents (htxf.path, &content, &clen, &rderr));
    g_assert_no_error (rderr);
    g_assert_cmpuint (clen, ==, 12);
    g_assert_cmpmem (content, clen, "hello world\n", 12);

    /* htxf_io_release closes the channel (and adopts-and-drops xfd). */
    htxf_io_release (&htxf);

    /* Best-effort cleanup of the temp tree (data fork + any sidecar). */
    unlink (htxf.path);
    g_autofree char *finfo =
        g_build_filename (tmpdir, ".finderinfo", "out.txt", NULL);
    unlink (finfo);
    g_autofree char *finfo_dir = g_build_filename (tmpdir, ".finderinfo", NULL);
    g_rmdir (finfo_dir);
    g_rmdir (tmpdir);

    integration_release_htlc (&htlc);
    integration_close (fd);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/integration/file_get/round_trip",
                     test_file_get_round_trip);

    return g_test_run ();
}
