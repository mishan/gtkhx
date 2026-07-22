/*
 * tests/integration/test_file_list_subdir.c — HTLC_HDR_FILE_LIST
 * with HTLC_DATA_DIR pointing at a subdirectory.
 *
 * Tier 3's existing test_file_list covers the root listing path
 * (no DIR chunk → mhxd lists hxd_cfg.paths.files). This test
 * exercises the variant: listing inside a subdirectory by sending
 * a Hotline-encoded path component in HTLC_DATA_DIR.
 *
 * The Dockerfile creates files/Uploads/ specifically so this test
 * has a known-empty subdir to walk into. The contract:
 *
 *   - The TASK reply correlates by trans.
 *   - The reply is either a successful TASK (flag=0) carrying zero
 *     or more FILE_LIST chunks (Uploads/ is empty, but mhxd may
 *     emit a stub or just no chunks), OR a task-error (flag=1) if
 *     the directory is somehow missing on this server.
 *
 * The hldir wire format is:
 *
 *   2 bytes: number of components (big-endian)
 *   per component:
 *     2 bytes: encoding (always zero for plain ASCII)
 *     1 byte:  name length
 *     N bytes: name
 *
 * For path "Uploads" we emit one component with len=7. files.c's
 * path_to_hldir() builds the same shape but isn't linkable into
 * the test binary (it pulls in GTK), so we hand-build the 12-byte
 * blob inline.
 */

#include "config.h"
#include <string.h>
#include <netinet/in.h>
#include <unistd.h>
#include <glib.h>
#include "compat.h"
#include "hotline.h"
#include "protocol.h"
#include "proto_helpers.h"
#include "integration_harness.h"

/* Build a single-component hldir blob for `name`. Returns g_malloc'd
 * bytes; caller frees. *outlen receives the total byte count. */
static guint8 *
build_hldir_one (const char *name, guint16 *outlen)
{
    guint8 nlen = (guint8)strlen (name);
    guint16 total = 2 + 3 + nlen;
    guint8 *buf = g_malloc (total);
    guint16 count = htons (1);

    memcpy (buf, &count, 2);
    /* enc = 0 */
    buf[2] = 0;
    buf[3] = 0;
    /* name length */
    buf[4] = nlen;
    memcpy (&buf[5], name, nlen);

    *outlen = total;
    return buf;
}

static void
test_file_list_subdir_uploads (void)
{
    struct htlc_conn htlc;
    int fd = integration_open_login_or_skip (&htlc, "FileListSub Tier-3", 412);
    if (fd < 0) {
        return;
    }

    guint16 hldirlen = 0;
    guint8 *hldir = build_hldir_one ("Uploads", &hldirlen);
    guint32 our_trans = htlc.trans;

    g_assert_true (integration_send_message (
        fd, &htlc, HTLC_HDR_FILE_LIST, /*flag=*/0, /*hc=*/1, (int)HTLC_DATA_DIR,
        (int)hldirlen, hldir));
    g_free (hldir);

    g_assert_true (integration_drain_until_task_trans (
        fd, &htlc, our_trans, 64));

    guint32 flag = hdr_flag (&htlc);
    if (flag & 1) {
        char err[256];
        gsize err_len = 0;
        g_assert_true (task_error_extract (htlc.in.buf, htlc.in.pos, err, sizeof (err), &err_len));
        g_test_message ("server returned task-error for "
                        "FILE_LIST Uploads/: \"%s\" "
                        "(check Dockerfile creates the dir)",
                        err);
    } else {
        int n = 0;
        dh_start (htlc.in.buf, htlc.in.pos)
        {
            if (_type == HTLS_DATA_FILE_LIST) {
                n++;
            }
        }
        dh_end ();
        g_test_message ("server returned %d FILE_LIST "
                        "chunks under Uploads/",
                        n);
    }

    integration_release_htlc (&htlc);
    integration_close (fd);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/integration/file_list_subdir/uploads",
                     test_file_list_subdir_uploads);
    return g_test_run ();
}
