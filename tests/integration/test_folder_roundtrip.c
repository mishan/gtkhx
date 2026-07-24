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
 * Links xfers_send.c + xfers_recv.c + the hxhfs crate + htxf_subchannel.c + the FFO
 * codec on top of integration_harness_lib. GTK-shell couplings are
 * stubbed (preview branch never runs; progress is a no-op).
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
#include "htxf_io.h"
#include "htxf_subchannel.h"
#include "preview.h"
#include "xfers_recv.h"
#include "xfers_send.h"
#include "integration_harness.h"


/* HxnetXferParams progress shape for the Rust hxnet_xfer_file_{recv,send}_one
 * paths (folder_recv_all / folder_send_all forward it per file). */
static void
noop_progress_bump (void *user_data, guint64 delta)
{
    (void)user_data;
    (void)delta;
}

/* Open the HTXF subchannel and wrap it as a FOLDER-typed hxnet channel
 * (the preamble carries HTXF_TYPE_FOLDER). Returns NULL on failure. */
static HtxfConn *
open_folder_channel (guint32 ref, guint64 total_size)
{
    guint8 pre[24];
    size_t plen = hx_htxf_subchannel_pack_preamble (
        pre, sizeof (pre), ref, total_size, HTXF_TYPE_FOLDER, 0, FALSE);
    if (!plen) {
        return NULL;
    }
    return integration_htxf_open_xfer (pre, plen, NULL, ref);
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

/* Encode a multi-component HTLC_DATA_DIR blob for comps[0..n): a u16 BE
 * count, then per component a reserved byte + u16 BE name length + name.
 * (integration_encode_hldir_one only does a single component.) */
static gsize
build_hldir (guint8 *out, const char *const *comps, int n)
{
    guint16 count_be = g_htons((guint16)n);
    memcpy (out, &count_be, 2);
    gsize pos = 2;
    for (int i = 0; i < n; i++) {
        gsize nl = strlen (comps[i]);
        guint16 nl_be = g_htons((guint16)nl);
        out[pos++] = 0;
        memcpy (out + pos, &nl_be, 2);
        pos += 2;
        memcpy (out + pos, comps[i], nl);
        pos += nl;
    }
    return pos;
}

/* FILE_GET a single file at DIR=comps[0..ncomp)/<fname> via the real
 * hxnet_xfer_file_recv_one; return its contents (caller frees) or NULL. */
static char *
get_file_direct (int ctrl, struct htlc_conn *htlc, const char *const *comps,
                 int ncomp, const char *fname, gsize *out_len)
{
    guint8 hldir[512];
    gsize hldir_len = build_hldir (hldir, comps, ncomp);
    guint32 our_trans = htlc->trans;
    if (!integration_send_message (
            ctrl, htlc, HTLC_HDR_FILE_GET, /*flag=*/0, /*hc=*/2,
            (int)HTLC_DATA_FILE_NAME, (int)strlen (fname), (guint8 *)fname,
            (int)HTLC_DATA_DIR, (int)hldir_len, hldir)) {
        return NULL;
    }
    if (!integration_drain_until_task_trans (ctrl, htlc, our_trans, 64)
        || (hdr_flag (htlc) & 1)) {
        return NULL;
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
    g_autofree char *tmpdir = g_dir_make_tmp ("gtkhx_frt_get_XXXXXX", NULL);
    if (!tmpdir) {
        hxnet_htxf_close (ch);
        return NULL;
    }
    struct htxf_conn htxf;
    memset (&htxf, 0, sizeof (htxf));
    htxf_io_init (&htxf);
    htxf.hx = ch;
    htxf.total_size = reply.size;
    g_snprintf (htxf.path, sizeof (htxf.path), "%s/got", tmpdir);

    char *content = NULL;
    struct HxnetXferParams params;
    memset (&params, 0, sizeof params);
    params.hx = htxf.hx;
    params.path = htxf.path;
    params.file_budget = reply.size;
    params.opt_preview = htxf.opt.preview;
    params.opt_folder = htxf.opt.folder;
    params.opt_large = htxf.opt.large;
    params.preview = htxf.preview;
    params.user_data = &htxf;
    params.progress = noop_progress_bump;
    if (hxnet_xfer_file_recv_one (&params) == 0) {
        if (!g_file_get_contents (htxf.path, &content, out_len, NULL)) {
            content = NULL;
        }
    }
    htxf_io_release (&htxf);
    unlink (htxf.path);
    g_rmdir (tmpdir);
    return content;
}

/* PUTFOLDER + drive folder_send_all to upload the local tree at `srcroot`
 * to Uploads/<folder>. Returns TRUE on a clean upload. */
static gboolean
upload_folder_tree (int fd, struct htlc_conn *htlc, const char *srcroot,
                    const char *folder)
{
    guint32 nfiles = 0;
    guint64 total = 0;
    tree_totals (srcroot, &nfiles, &total);

    guint8 hldir[64];
    gsize hldir_len = integration_encode_hldir_one (hldir, "Uploads");
    guint32 size_be = g_htonl((guint32)total);
    guint32 nfiles_be = g_htonl(nfiles);
    guint32 our_trans = htlc->trans;
    if (!integration_send_message (
            fd, htlc, HTLC_HDR_FILE_PUTFOLDER, /*flag=*/0, /*hc=*/4,
            (int)HTLC_DATA_FILE_NAME, (int)strlen (folder), (guint8 *)folder,
            (int)HTLC_DATA_DIR, (int)hldir_len, hldir,
            (int)HTLC_DATA_HTXF_SIZE, (int)sizeof (size_be), &size_be,
            (int)HTLC_DATA_FILE_NFILES, (int)sizeof (nfiles_be), &nfiles_be)) {
        return FALSE;
    }
    if (!integration_drain_until_task_trans (fd, htlc, our_trans, 64)
        || (hdr_flag (htlc) & 1)) {
        return FALSE;
    }
    guint32 xfer_ref = 0;
    dh_start (hx_test_in(htlc)->buf, hx_test_in(htlc)->pos)
    {
        if (_type == HTLS_DATA_HTXF_REF) {
            dh_getint (xfer_ref);
        }
    }
    dh_end ();
    if (!xfer_ref) {
        return FALSE;
    }
    HtxfConn *ch = open_folder_channel (xfer_ref, total);
    if (!ch) {
        return FALSE;
    }
    struct htxf_conn htxf;
    memset (&htxf, 0, sizeof (htxf));
    htxf_io_init (&htxf);
    htxf.hx = ch;
    htxf.opt.folder = 1;
    htxf.total_size = total;
    guint8 buf[2048];
    int rv = folder_send_all (&htxf, srcroot, buf, noop_progress_bump);
    htxf_io_release (&htxf);
    return rv == 0;
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
    hx_htxf_reply_extract (hx_test_in(htlc)->buf, hx_test_in(htlc)->pos, &reply);
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
    int rv = folder_recv_all (&htxf, dstroot, buf, noop_progress_bump);
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

    /* Build a local source tree: two files at the top level. (Nested
     * subdirs are covered by test_folder_nested_upload — mhxd's folder
     * *download* is non-recursive, so a full nested round-trip via
     * GETFOLDER can't retrieve subdir files; see that test.) */
    g_autofree char *srcroot = g_dir_make_tmp ("gtkhx_frt_src_XXXXXX", NULL);
    g_assert_nonnull (srcroot);
    const char *alpha_body = "alpha folder file\n";
    const char *beta_body = "beta second file body\n";
    g_autofree char *alpha = g_build_filename (srcroot, "alpha.txt", NULL);
    g_autofree char *beta = g_build_filename (srcroot, "beta.txt", NULL);
    g_assert_true (g_file_set_contents (alpha, alpha_body, -1, NULL));
    g_assert_true (g_file_set_contents (beta, beta_body, -1, NULL));

    g_autofree char *folder =
        g_strdup_printf ("tier3_frt_%08x", g_random_int ());
    /* Declared before any `goto out` so the g_autofree cleanups never run
     * over an uninitialised pointer. */
    g_autofree char *dstroot = NULL;
    g_autofree char *got_root = NULL;

    /* Upload the tree via the production folder_send_all. */
    if (!upload_folder_tree (fd, &htlc, srcroot, folder)) {
        g_test_fail_printf ("folder upload failed (guest UPLOAD_FOLDERS?)");
        goto out;
    }

    /* Download the tree back (retry: mhxd commits asynchronously). mhxd
     * sends path components relative to the requested folder, so — as in
     * production hx_get_folder — the receive base already includes the
     * folder name. */
    dstroot = g_dir_make_tmp ("gtkhx_frt_dst_XXXXXX", NULL);
    g_assert_nonnull (dstroot);
    got_root = g_build_filename (dstroot, folder, NULL);

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

/* Nested subdir: upload <root>/alpha.txt + <root>/nested/beta.txt via the
 * production folder_send_all, then verify BOTH landed on the server by
 * fetching each file directly (FILE_GET) — including the subdir file.
 *
 * We can't verify the subdir file via a folder DOWNLOAD: mhxd's
 * folder_send (server side) is non-recursive — folder_getpaths reads a
 * single directory level and folder_send hard-codes pathcount=1, so it
 * emits a marker for a subdir but never descends into it. That's a mhxd
 * limitation, not a client bug: folder_send_all correctly uploads the
 * recursive tree (mhxd's folder_recv is recursion-capable and stores it),
 * and a direct FILE_GET of the nested path retrieves it. */
static void
test_folder_nested_upload (void)
{
    struct htlc_conn htlc;
    int fd = integration_open_login_or_skip (&htlc, "FolderNest Tier-3", 412);
    if (fd < 0) {
        return;
    }

    g_autofree char *srcroot = g_dir_make_tmp ("gtkhx_frn_src_XXXXXX", NULL);
    g_assert_nonnull (srcroot);
    const char *alpha_body = "alpha top-level file\n";
    const char *beta_body = "beta nested file body\n";
    g_autofree char *alpha = g_build_filename (srcroot, "alpha.txt", NULL);
    g_autofree char *nested = g_build_filename (srcroot, "nested", NULL);
    g_assert_cmpint (g_mkdir (nested, 0755), ==, 0);
    g_autofree char *beta = g_build_filename (nested, "beta.txt", NULL);
    g_assert_true (g_file_set_contents (alpha, alpha_body, -1, NULL));
    g_assert_true (g_file_set_contents (beta, beta_body, -1, NULL));

    g_autofree char *folder =
        g_strdup_printf ("tier3_frn_%08x", g_random_int ());
    /* Declared before any `goto out` so the g_autofree cleanups are safe. */
    g_autofree char *got_alpha = NULL;
    g_autofree char *got_beta = NULL;

    if (!upload_folder_tree (fd, &htlc, srcroot, folder)) {
        g_test_fail_printf ("nested folder upload failed");
        goto out;
    }

    /* Verify both files by direct FILE_GET (retry for async commit):
     *   Uploads/<folder>/alpha.txt         (top level)
     *   Uploads/<folder>/nested/beta.txt   (subdir — the recursive case) */
    const char *top_dir[] = { "Uploads", folder };
    const char *nested_dir[] = { "Uploads", folder, "nested" };

    gsize alen = 0, blen = 0;
    for (int attempt = 0; attempt < 30 && !(got_alpha && got_beta); attempt++) {
        if (attempt) {
            g_usleep (100 * 1000);
        }
        if (!got_alpha) {
            got_alpha = get_file_direct (fd, &htlc, top_dir, 2, "alpha.txt",
                                         &alen);
        }
        if (!got_beta) {
            got_beta = get_file_direct (fd, &htlc, nested_dir, 3, "beta.txt",
                                        &blen);
        }
    }

    g_assert_nonnull (got_alpha);
    g_assert_cmpmem (got_alpha, alen, alpha_body, strlen (alpha_body));
    g_assert_nonnull (got_beta);
    g_assert_cmpmem (got_beta, blen, beta_body, strlen (beta_body));

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
    g_test_add_func ("/integration/folder_roundtrip/nested_upload",
                     test_folder_nested_upload);
    return g_test_run ();
}
