/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_folder_roundtrip.c — upload a local folder tree
 * to mhxd via the PRODUCTION send machine (xfers_send.c::folder_send_all)
 * and download it back through the PRODUCTION receive machine
 * (xfers_recv.c::folder_recv_all), asserting every file round-trips
 * byte-for-byte.
 *
 * This drives the real Hotline 1.5 FILE_NEXT/FILE_SEND folder protocol on
 * both sides — the nfi header + path-component framing, folder markers,
 * per-file size headers, and the FILP body via file_{send,recv}_one.
 *
 * Requires the guest account to hold UPLOAD_FOLDERS + DOWNLOAD_FOLDERS
 * (+ UPLOAD_FILES / UPLOAD_ANYWHERE), granted in the mhxd overlay's
 * guest UserData.
 *
 * Links xfers_send.c + xfers_recv.c + hfs.c + htxf_subchannel.c + the FFO
 * codec on top of integration_harness_lib. GTK-shell couplings are
 * stubbed (preview branch never runs; progress is a no-op).
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
#include "htxf_subchannel.h"
#include "preview.h"
#include "xfers_recv.h"
#include "xfers_send.h"
#include "integration_harness.h"

/* --- link stubs for the send/recv machines' GTK-shell couplings --- */
guint8 dir_char = '/';

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

static const char *
xfer_host (void)
{
    const char *h = g_getenv ("GTKHX_TEST_HOST");
    return (h && *h) ? h : "127.0.0.1";
}

/* Open the HTXF subchannel and wrap it as a FOLDER-typed hxnet channel
 * (the preamble carries HTXF_TYPE_FOLDER). Returns NULL on failure. */
static HtxfConn *
open_folder_channel (guint32 ref, guint64 total_size)
{
    int xfd = integration_connect_xfer ();
    if (xfd < 0) {
        return NULL;
    }
    guint8 pre[24];
    size_t plen = hx_htxf_subchannel_pack_preamble (
        pre, sizeof (pre), ref, total_size, HTXF_TYPE_FOLDER, 0, FALSE);
    if (!plen) {
        integration_close (xfd);
        return NULL;
    }
    HtxfConn *ch = hxnet_htxf_open (xfd, 0, (const guint8 *)xfer_host (),
                                    strlen (xfer_host ()), pre, plen, NULL,
                                    ref, NULL, NULL);
    if (!ch) {
        integration_close (xfd);
    }
    return ch;
}

/* Fixed FILP-wrapper overhead per file for the HTXF_SIZE aggregate: the
 * 133-byte header + the 16-byte MACR marker file_send_one always writes
 * (no comment, no rsrc fork). */
#define FILP_OVERHEAD (133 + 16)

/* Recursively count regular files and sum (FILP_OVERHEAD + size) for the
 * PUTFOLDER aggregate. */
static void
tree_totals (const char *dir, guint32 *nfiles, guint64 *total)
{
    GDir *d = g_dir_open (dir, 0, NULL);
    if (!d) {
        return;
    }
    const char *n;
    while ((n = g_dir_read_name (d))) {
        g_autofree char *full = g_build_filename (dir, n, NULL);
        GStatBuf sb;
        if (g_stat (full, &sb) < 0) {
            continue;
        }
        if (S_ISDIR (sb.st_mode)) {
            tree_totals (full, nfiles, total);
        } else if (S_ISREG (sb.st_mode)) {
            (*nfiles)++;
            *total += FILP_OVERHEAD + (guint64)sb.st_size;
        }
    }
    g_dir_close (d);
}

/* Assert file `rel` under `root` exists with exactly `body`. */
static void
assert_file_is (const char *root, const char *rel, const char *body)
{
    g_autofree char *p = g_build_filename (root, rel, NULL);
    g_autofree char *got = NULL;
    gsize glen = 0;
    g_autoptr (GError) e = NULL;
    if (!g_file_get_contents (p, &got, &glen, &e)) {
        g_test_fail_printf ("missing round-tripped file %s: %s", rel,
                            e ? e->message : "?");
        return;
    }
    g_assert_cmpmem (got, glen, body, strlen (body));
}

/* Download folder `name` from Uploads/ into `dstroot` via folder_recv_all.
 * Returns TRUE on a clean transfer. */
static gboolean
download_folder (int ctrl, struct htlc_conn *htlc, const char *name,
                 const char *dstroot)
{
    guint8 hldir[64];
    gsize hldir_len = integration_encode_hldir_one (hldir, "Uploads");
    guint32 our_trans = htlc->trans;

    if (!integration_send_message (
            ctrl, htlc, HTLC_HDR_FILE_GETFOLDER, /*flag=*/0, /*hc=*/2,
            (int)HTLC_DATA_FILE_NAME, (int)strlen (name), (guint8 *)name,
            (int)HTLC_DATA_DIR, (int)hldir_len, hldir)) {
        return FALSE;
    }
    if (!integration_drain_until_task_trans (ctrl, htlc, our_trans, 64)
        || (hdr_flag (htlc) & 1)) {
        return FALSE;
    }

    struct hx_htxf_reply reply = { 0 };
    hx_htxf_reply_extract (htlc, &reply);
    if (!reply.ref) {
        return FALSE;
    }

    HtxfConn *ch = open_folder_channel (reply.ref, reply.size);
    if (!ch) {
        return FALSE;
    }

    struct htxf_conn htxf;
    memset (&htxf, 0, sizeof (htxf));
    htxf_io_init (&htxf);
    htxf.hx = ch;
    htxf.opt.folder = 1;
    htxf.total_size = reply.size;

    guint8 buf[1024];
    int rv = folder_recv_all (&htxf, dstroot, buf, noop_progress);
    htxf_io_release (&htxf);
    return rv == 0;
}

static void
test_folder_round_trip (void)
{
    struct htlc_conn htlc;
    int fd = integration_open_login_or_skip (&htlc, "FolderRT Tier-3", 412);
    if (fd < 0) {
        return;
    }

    /* Build a local source tree: two files at the top level. (A nested
     * subdir — folder markers + pathcount>1 — is a separate follow-up;
     * mhxd's subdir round-trip has its own untested quirks.) */
    g_autofree char *srcroot = g_dir_make_tmp ("gtkhx_frt_src_XXXXXX", NULL);
    g_assert_nonnull (srcroot);
    const char *alpha_body = "alpha folder file\n";
    const char *beta_body = "beta second file body\n";
    g_autofree char *alpha = g_build_filename (srcroot, "alpha.txt", NULL);
    g_autofree char *beta = g_build_filename (srcroot, "beta.txt", NULL);
    g_assert_true (g_file_set_contents (alpha, alpha_body, -1, NULL));
    g_assert_true (g_file_set_contents (beta, beta_body, -1, NULL));

    guint32 nfiles = 0;
    guint64 total = 0;
    tree_totals (srcroot, &nfiles, &total);
    g_assert_cmpuint (nfiles, ==, 2);

    g_autofree char *folder =
        g_strdup_printf ("tier3_frt_%08x", g_random_int ());

    /* FILE_PUTFOLDER: NAME + DIR(Uploads) + HTXF_SIZE + NFILES. */
    guint8 hldir[64];
    gsize hldir_len = integration_encode_hldir_one (hldir, "Uploads");
    guint32 size_be = htonl ((guint32)total);
    guint32 nfiles_be = htonl (nfiles);
    guint32 our_trans = htlc.trans;
    g_assert_true (integration_send_message (
        fd, &htlc, HTLC_HDR_FILE_PUTFOLDER, /*flag=*/0, /*hc=*/4,
        (int)HTLC_DATA_FILE_NAME, (int)strlen (folder), (guint8 *)folder,
        (int)HTLC_DATA_DIR, (int)hldir_len, hldir,
        (int)HTLC_DATA_HTXF_SIZE, (int)sizeof (size_be), &size_be,
        (int)HTLC_DATA_FILE_NFILES, (int)sizeof (nfiles_be), &nfiles_be));

    g_assert_true (
        integration_drain_until_task_trans (fd, &htlc, our_trans, 64));
    if (hdr_flag (&htlc) & 1) {
        char err[256];
        gsize el = 0;
        task_error_extract (&htlc, err, sizeof (err), &el);
        g_test_fail_printf ("putfolder refused: \"%s\" (guest UPLOAD_FOLDERS?)",
                            err);
        goto out;
    }

    guint32 xfer_ref = 0;
    dh_start (&htlc)
    {
        if (_type == HTLS_DATA_HTXF_REF) {
            dh_getint (xfer_ref);
        }
    }
    dh_end ();
    g_assert_cmphex (xfer_ref, !=, 0);

    HtxfConn *ch = open_folder_channel (xfer_ref, total);
    g_assert_nonnull (ch);

    struct htxf_conn htxf;
    memset (&htxf, 0, sizeof (htxf));
    htxf_io_init (&htxf);
    htxf.hx = ch;
    htxf.opt.folder = 1;
    htxf.total_size = total;
    g_strlcpy (htxf.path, srcroot, sizeof (htxf.path));

    guint8 buf[2048];
    int rv = folder_send_all (&htxf, srcroot, buf, noop_progress);
    g_assert_cmpint (rv, ==, 0);
    htxf_io_release (&htxf);

    /* Download the tree back (retry: mhxd commits asynchronously). mhxd
     * sends path components relative to the requested folder, so — as in
     * production hx_get_folder — the receive base already includes the
     * folder name. */
    g_autofree char *dstroot = g_dir_make_tmp ("gtkhx_frt_dst_XXXXXX", NULL);
    g_assert_nonnull (dstroot);
    g_autofree char *got_root = g_build_filename (dstroot, folder, NULL);

    gboolean ok = FALSE;
    for (int attempt = 0; attempt < 30 && !ok; attempt++) {
        if (attempt) {
            g_usleep (100 * 1000);
        }
        if (download_folder (fd, &htlc, folder, got_root)) {
            g_autofree char *a = g_build_filename (got_root, "alpha.txt", NULL);
            ok = g_file_test (a, G_FILE_TEST_EXISTS);
        }
    }
    g_assert_true (ok);

    assert_file_is (got_root, "alpha.txt", alpha_body);
    assert_file_is (got_root, "beta.txt", beta_body);

out:
    integration_release_htlc (&htlc);
    integration_close (fd);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/integration/folder_roundtrip/tree",
                     test_folder_round_trip);
    return g_test_run ();
}
