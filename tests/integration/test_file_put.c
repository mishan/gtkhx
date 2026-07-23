/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_file_put.c — upload a file to mhxd over the
 * HTXF subchannel driving the PRODUCTION send state machine
 * (xfers_send.c::file_send_one), then download it back through the
 * PRODUCTION receive machine (xfers_recv.c::file_recv_one) and assert
 * the round-tripped bytes are byte-for-byte what we uploaded.
 *
 * This exercises the real FILP encode: the header template, the
 * type/creator + timestamp fill, the DATA fork-length pack, and the fork
 * write over the AEAD-aware channel — the send-side twin of
 * test_file_get.
 *
 * Requires the guest account to hold UPLOAD_FILES + UPLOAD_ANYWHERE
 * (granted in tests/mhxd/conf/accounts/guest/UserData) so mhxd accepts
 * the upload to the seeded Uploads/ directory.
 *
 * Links xfers_send.c + xfers_recv.c + the hxhfs crate + the FFO codec on
 * top of integration_harness_lib. file_{send,recv}_one's GTK-shell
 * couplings are stubbed below (preview branch never runs; progress is a
 * no-op).
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
#include "xfers_send.h"
#include "integration_harness.h"

/* --- link stubs for the send/recv machines' GTK-shell couplings --- */

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

/* Download Uploads/<fname> via the production file_recv_one into a fresh
 * temp file and return its contents (caller frees). NULL on any failure
 * along the way. */
static char *
download_uploaded (int ctrl, struct htlc_conn *htlc, const char *fname,
                   gsize *out_len)
{
    guint8 hldir[64];
    gsize hldir_len = integration_encode_hldir_one (hldir, "Uploads");
    guint32 our_trans = htlc->trans;

    if (!integration_send_message (
            ctrl, htlc, HTLC_HDR_FILE_GET, /*flag=*/0, /*hc=*/2,
            (int)HTLC_DATA_FILE_NAME, (int)strlen (fname), (guint8 *)fname,
            (int)HTLC_DATA_DIR, (int)hldir_len, hldir)) {
        return NULL;
    }
    if (!integration_drain_until_task_trans (ctrl, htlc, our_trans, 64)
        || (hdr_flag (htlc) & 1)) {
        return NULL; /* not stored yet (retried by the caller) or refused */
    }

    struct hx_htxf_reply reply = { 0 };
    hx_htxf_reply_extract (hx_test_in(htlc)->buf, hx_test_in(htlc)->pos, &reply);
    if (!reply.ref || !reply.size) {
        return NULL;
    }

    HtxfConn *ch = integration_htxf_open_xfer_file (
        reply.ref, (guint32)reply.size, NULL, reply.ref);
    if (!ch) {
        return NULL;
    }

    g_autofree char *tmpdir = g_dir_make_tmp ("gtkhx_putget_XXXXXX", NULL);
    if (!tmpdir) {
        hxnet_htxf_close (ch);
        return NULL;
    }

    struct htxf_conn htxf;
    memset (&htxf, 0, sizeof (htxf));
    htxf_io_init (&htxf);
    htxf.hx = ch;
    htxf.total_size = reply.size;
    g_snprintf (htxf.path, sizeof (htxf.path), "%s/back.txt", tmpdir);

    guint8 buf[1024];
    char *content = NULL;
    if (file_recv_one (&htxf, reply.size, buf, noop_progress) == 0) {
        if (!g_file_get_contents (htxf.path, &content, out_len, NULL)) {
            content = NULL;
        }
    }
    htxf_io_release (&htxf);

    unlink (htxf.path);
    g_rmdir (tmpdir);
    return content;
}

static void
test_file_put_round_trip (void)
{
    struct htlc_conn htlc;
    int fd = integration_open_login_or_skip (&htlc, "FilePut Tier-3", 412);
    if (fd < 0) {
        return;
    }

    /* Declared before any `goto out_src` so the g_autofree cleanup never
     * runs over an uninitialised pointer. */
    g_autofree char *back = NULL;
    gsize back_len = 0;

    /* Local source file with known content. */
    const char *body = "hello upload from file_send_one\n";
    gsize body_len = strlen (body);
    g_autofree char *srcdir = g_dir_make_tmp ("gtkhx_putsrc_XXXXXX", NULL);
    g_assert_nonnull (srcdir);
    g_autofree char *srcpath = g_build_filename (srcdir, "src.txt", NULL);
    g_assert_true (
        g_file_set_contents (srcpath, body, (gssize)body_len, NULL));

    /* Unique remote name so reruns don't collide / hit resume. */
    g_autofree char *fname =
        g_strdup_printf ("tier3_put_%08x.txt", g_random_int ());

    /* FFO upload total: 133-byte FILP header + data fork (no comment, no
     * rsrc). Matches rcv_task_file_put's HTXF_SIZE accounting; the
     * trailing 16-byte MACR marker file_send_one writes is not counted
     * (the server stops after this many bytes). */
    guint32 up_total = 133 + (guint32)body_len;

    guint8 hldir[64];
    gsize hldir_len = integration_encode_hldir_one (hldir, "Uploads");
    guint32 size_be = htonl (up_total);
    guint32 our_trans = htlc.trans;

    g_assert_true (integration_send_message (
        fd, &htlc, HTLC_HDR_FILE_PUT, /*flag=*/0, /*hc=*/3,
        (int)HTLC_DATA_FILE_NAME, (int)strlen (fname), (guint8 *)fname,
        (int)HTLC_DATA_DIR, (int)hldir_len, hldir,
        (int)HTLC_DATA_HTXF_SIZE, (int)sizeof (size_be), &size_be));

    g_assert_true (
        integration_drain_until_task_trans (fd, &htlc, our_trans, 64));

    if (hdr_flag (&htlc) & 1) {
        char err[256];
        gsize err_len = 0;
        if (task_error_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, err, sizeof (err), &err_len)) {
            g_test_fail_printf ("file_put refused by server: \"%s\". Does the "
                                "guest account have UPLOAD_FILES + "
                                "UPLOAD_ANYWHERE?",
                                err);
        } else {
            g_test_fail_printf ("file_put refused (no error chunk)");
        }
        goto out_src;
    }

    guint32 xfer_ref = 0;
    dh_start (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos)
    {
        if (_type == HTLS_DATA_HTXF_REF) {
            dh_getint (xfer_ref);
        }
    }
    dh_end ();
    g_assert_cmphex (xfer_ref, !=, 0);

    /* Open the subchannel, send the upload preamble, wrap the fd. */
    HtxfConn *ch =
        integration_htxf_open_xfer_file (xfer_ref, up_total, NULL, xfer_ref);
    if (!ch) {
        g_test_fail_printf ("HTXF subchannel port (5501) unreachable.");
        goto out_src;
    }

    struct htxf_conn htxf;
    memset (&htxf, 0, sizeof (htxf));
    htxf_io_init (&htxf);
    htxf.hx = ch;
    g_strlcpy (htxf.path, srcpath, sizeof (htxf.path));
    htxf.data_size = body_len;
    htxf.rsrc_size = 0;
    htxf.total_size = up_total;

    /* THE code under test: the production single-file send machine. */
    guint8 buf[512];
    int rv = file_send_one (&htxf, buf, noop_progress);
    g_assert_cmpint (rv, ==, 0);
    htxf_io_release (&htxf);

    /* Round-trip: download it back and assert byte-exact. mhxd commits
     * the uploaded file to disk asynchronously once the subchannel
     * closes, so the file may not be visible on the very next FILE_GET —
     * retry briefly until it appears. */
    for (int attempt = 0; attempt < 30 && !back; attempt++) {
        if (attempt) {
            g_usleep (100 * 1000); /* 100 ms */
        }
        back = download_uploaded (fd, &htlc, fname, &back_len);
    }
    g_assert_nonnull (back);
    g_assert_cmpuint (back_len, ==, body_len);
    g_assert_cmpmem (back, back_len, body, body_len);

out_src:
    g_unlink (srcpath);
    g_rmdir (srcdir);
    integration_release_htlc (&htlc);
    integration_close (fd);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/integration/file_put/round_trip",
                     test_file_put_round_trip);

    return g_test_run ();
}
