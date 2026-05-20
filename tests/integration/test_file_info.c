/*
 * tests/integration/test_file_info.c — HTLC_HDR_FILE_GETINFO round-trip.
 *
 * Request info on the seeded files/test.txt (the Dockerfile writes
 * "hello world\n" — 12 bytes — into that path). Verify the TASK
 * reply carries:
 *
 *   HTLS_DATA_FILE_NAME — matches what we asked for ("test.txt")
 *   HTLS_DATA_FILE_SIZE — equals 12 (the seeded payload length)
 *
 * mhxd's rcv_file_getinfo also emits FILE_DATE_CREATE / DATE_MODIFY
 * / TYPE / CREATOR chunks, but those are filesystem-detail dependent
 * (timestamps from the docker build, type/creator strings from the
 * server's heuristic) — pinning them down would couple the test to
 * mhxd's implementation choices. NAME and SIZE are the two we know
 * are reliable.
 *
 * Tolerant to a task-error reply: if the seed file isn't on the
 * server (someone running against a non-default mhxd), we log a
 * skip-style message and exit clean.
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

static void
test_file_info_seeded (void)
{
    struct htlc_conn htlc;
    int fd = integration_open_login_or_skip (&htlc, "FileInfo Tier-3", 412);
    if (fd < 0) {
        return;
    }

    const char *fname = "test.txt";
    guint32 our_trans = htlc.trans;

    g_assert_true (integration_send_message (
        fd, &htlc, HTLC_HDR_FILE_GETINFO, /*flag=*/0, /*hc=*/1,
        (int)HTLC_DATA_FILE_NAME, (int)strlen (fname), fname));

    gboolean got_reply = FALSE;
    for (int i = 0; i < 64 && !got_reply; i++) {
        g_assert_true (
            integration_recv_message (fd, &htlc, /*timeout_ms=*/3000));
        if (hdr_type (&htlc) != HTLS_HDR_TASK) {
            continue;
        }
        if (hdr_trans (&htlc) != our_trans) {
            continue;
        }
        got_reply = TRUE;
    }
    g_assert_true (got_reply);

    if (hdr_flag (&htlc) & 1) {
        char err[256];
        gsize err_len = 0;
        g_assert_true (task_error_extract (&htlc, err, sizeof (err), &err_len));
        g_test_message ("server returned task-error for "
                        "FILE_GETINFO test.txt: \"%s\" "
                        "(check Dockerfile seeds the file)",
                        err);
        integration_release_htlc (&htlc);
        integration_close (fd);
        return;
    }

    char *got_name = NULL;
    gsize got_name_len = 0;
    guint32 got_size = 0;
    gboolean got_size_chunk = FALSE;

    dh_start (&htlc)
    {
        switch (_type) {
        case HTLS_DATA_FILE_NAME:
            got_name = g_strndup ((const char *)dh->data, _len);
            got_name_len = _len;
            break;
        case HTLS_DATA_FILE_SIZE:
            if (_len == sizeof (guint32)) {
                guint32 v;
                memcpy (&v, dh->data, sizeof v);
                got_size = ntohl (v);
                got_size_chunk = TRUE;
            }
            break;
        }
    }
    dh_end ();

    g_assert_nonnull (got_name);
    g_assert_cmpuint (got_name_len, ==, strlen (fname));
    g_assert_cmpstr (got_name, ==, fname);
    g_assert_true (got_size_chunk);
    /* "hello world\n" — 12 bytes seeded by tests/mhxd/Dockerfile. */
    g_assert_cmpuint (got_size, ==, 12);

    g_free (got_name);
    integration_release_htlc (&htlc);
    integration_close (fd);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/integration/file_info/seeded", test_file_info_seeded);
    return g_test_run ();
}
