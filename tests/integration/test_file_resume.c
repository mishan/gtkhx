/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_file_resume.c — resume a partially-downloaded file
 * from mhxd over the HTXF subchannel, exercising the download-resume path
 * end-to-end against a real server.
 *
 * The client half of a resume is: xfer_go detects a local file shorter than
 * the server's copy, stamps the resume offsets into a 74-byte RFLT record, and
 * sends it in the FILE_GET request; the receive machine then seeks to the
 * resume offset and writes only the tail. mhxd reads the two fork offsets at
 * the fixed RFLT positions [46] (DATA) and [62] (MACR) and replies with a
 * transfer size reduced by the resumed prefix, then streams only the remaining
 * bytes.
 *
 * The RFLT record built here mirrors the production builder
 * (hxhandlers::xfer::build_resume_rflt); the record's byte layout is pinned by
 * that crate's `resume_rflt_is_well_formed` unit test, and this test pins the
 * server contract (mhxd honours the offset).
 *
 * Flow:
 *   1. Login on the main port (5500), send HTLC_HDR_FILE_GET for test.txt with
 *      an RFLT chunk carrying data_pos = 6 (resume past "hello ").
 *   2. Open the subchannel, pre-seed the local file with the first 6 bytes,
 *      and run hxnet_xfer_file_recv_one with data_pos = 6.
 *   3. Assert the completed file is byte-for-byte "hello world\n". If the
 *      server had ignored the offset and streamed the whole file, the tail
 *      would land at byte 6 and the file would read "hello hello world\n" —
 *      so the exact-content check is the authoritative resume proof.
 *
 * The Dockerfile seeds files/test.txt with exactly "hello world\n".
 */

#include "config.h"
#include <string.h>
#include <unistd.h>
#include <glib.h>
#include <glib/gstdio.h>
#include "compat.h"
#include "hotline.h"
#include "protocol.h"
#include "proto_helpers.h"
#include "hxnet_htxf.h"
#include "preview.h"
#include "xfers_recv.h"
#include "integration_harness.h"

#define SEED "hello world\n"
#define SEED_LEN 12
#define RESUME_AT 6 /* download from byte 6: skip "hello " */

static void
noop_progress (void *user_data, guint64 delta)
{
    (void)user_data;
    (void)delta;
}

/* Build the 74-byte resume RFLT record, mirroring the production
 * hxhandlers::xfer::build_resume_rflt: RFLT magic at [0], version 1, fork count
 * 2, "DATA"/"MACR" fork tags at [42]/[58], and the two 32-bit big-endian resume
 * offsets at [46]/[62] (the fixed positions mhxd reads). */
static void
build_resume_rflt (guint32 data_pos, guint32 rsrc_pos, guint8 out[74])
{
    memset (out, 0, 74);
    memcpy (out, "RFLT", 4);
    out[5] = 1;  /* version 1 (u16 BE at [4..6]) */
    out[41] = 2; /* fork count 2 (u16 BE at [40..42]) */
    memcpy (out + 42, "DATA", 4);
    out[46] = (guint8)(data_pos >> 24);
    out[47] = (guint8)(data_pos >> 16);
    out[48] = (guint8)(data_pos >> 8);
    out[49] = (guint8)(data_pos);
    memcpy (out + 58, "MACR", 4);
    out[62] = (guint8)(rsrc_pos >> 24);
    out[63] = (guint8)(rsrc_pos >> 16);
    out[64] = (guint8)(rsrc_pos >> 8);
    out[65] = (guint8)(rsrc_pos);
}

static void
test_file_resume_round_trip (void)
{
    struct htlc_conn htlc;
    int fd = integration_open_login_or_skip (&htlc, "FileResume Tier-3", 419);
    if (fd < 0) {
        return;
    }

    /* FILE_GET files/test.txt with a resume RFLT (data_pos = 6, rsrc_pos = 0).
     * mhxd allows only one in-flight transfer per user ("1 at a time"), so we
     * can't hold a baseline full transfer open for a size comparison — the byte
     * round-trip below is the authoritative resume proof instead. */
    const char *fname = "test.txt";
    guint8 rflt[74];
    build_resume_rflt (RESUME_AT, 0, rflt);

    guint32 our_trans = htlc.trans;
    g_assert_true (integration_send_message (
        fd, &htlc, HTLC_HDR_FILE_GET, /*flag=*/0, /*hc=*/2,
        (int)HTLC_DATA_FILE_NAME, (int)strlen (fname), (guint8 *)fname,
        (int)HTLC_DATA_RFLT, 74, rflt));

    g_assert_true (
        integration_drain_until_task_trans (fd, &htlc, our_trans, 64));

    if (hdr_flag (&htlc) & 1) {
        char err[256];
        gsize err_len = 0;
        if (task_error_extract (hx_test_in (&htlc)->buf,
                                hx_test_in (&htlc)->pos, err, sizeof (err),
                                &err_len)) {
            g_test_fail_printf ("file_get(resume) refused by server: \"%s\". "
                                "Is files/test.txt seeded in the container?",
                                err);
        } else {
            g_test_fail_printf ("file_get(resume) refused (no error chunk)");
        }
        integration_release_htlc (&htlc);
        integration_close (fd);
        return;
    }

    struct hx_htxf_reply reply = { 0 };
    hx_htxf_reply_extract (hx_test_in (&htlc)->buf, hx_test_in (&htlc)->pos,
                           &reply);
    g_assert_cmphex (reply.ref, !=, 0);
    g_assert_cmpuint (reply.size, >, 0);
    guint32 xfer_ref = reply.ref;
    guint64 xfer_size = reply.size;

    HtxfConn *ch = integration_htxf_open_xfer_file (
        xfer_ref, (guint32)xfer_size, NULL, xfer_ref);
    if (!ch) {
        g_test_fail_printf ("HTXF subchannel port (5501 by default) isn't "
                            "reachable. Publish -p 5501:5501 or set "
                            "GTKHX_TEST_XFER_PORT.");
        integration_release_htlc (&htlc);
        integration_close (fd);
        return;
    }

    g_autoptr (GError) tmperr = NULL;
    g_autofree char *tmpdir
        = g_dir_make_tmp ("gtkhx_fileresume_XXXXXX", &tmperr);
    g_assert_no_error (tmperr);
    g_assert_nonnull (tmpdir);

    struct htxf_conn htxf;
    memset (&htxf, 0, sizeof (htxf));
    htxf.hx = ch;
    htxf.total_size = xfer_size;
    htxf.data_pos = RESUME_AT;
    g_snprintf (htxf.path, sizeof (htxf.path), "%s/out.txt", tmpdir);

    /* Pre-seed the local file with the already-downloaded prefix ("hello "),
     * the way a real resume picks up an interrupted download. The receive
     * machine opens without truncating and seeks to data_pos, so this prefix
     * must survive into the completed file. */
    g_autoptr (GError) seederr = NULL;
    g_assert_true (g_file_set_contents (htxf.path, SEED, RESUME_AT, &seederr));
    g_assert_no_error (seederr);

    /* THE code under test: the production single-file receive machine, resuming
     * at data_pos. */
    struct HxnetXferParams params;
    memset (&params, 0, sizeof params);
    params.hx = htxf.hx;
    params.path = htxf.path;
    params.file_budget = xfer_size;
    params.data_pos = htxf.data_pos;
    params.rsrc_pos = htxf.rsrc_pos;
    params.user_data = &htxf;
    params.progress = noop_progress;
    int rv = hxnet_xfer_file_recv_one (&params);
    g_assert_cmpint (rv, ==, 0);

    /* Resume proof: the completed file is exactly the seed. Had the server
     * ignored data_pos and streamed the whole file, the tail would land at
     * byte 6 and this would read "hello hello world\n" (18 bytes). */
    g_autofree char *content = NULL;
    gsize clen = 0;
    g_autoptr (GError) rderr = NULL;
    g_assert_true (g_file_get_contents (htxf.path, &content, &clen, &rderr));
    g_assert_no_error (rderr);
    g_assert_cmpuint (clen, ==, SEED_LEN);
    g_assert_cmpmem (content, clen, SEED, SEED_LEN);

    hxnet_htxf_close ((HtxfConn *)htxf.hx);

    unlink (htxf.path);
    g_autofree char *finfo
        = g_build_filename (tmpdir, ".finderinfo", "out.txt", NULL);
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

    g_test_add_func ("/integration/file_resume/round_trip",
                     test_file_resume_round_trip);

    return g_test_run ();
}
