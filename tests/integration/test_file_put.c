/*
 * tests/integration/test_file_put.c — exercise HTLC_HDR_FILE_PUT's
 * setup phase.
 *
 * The full file-upload flow is:
 *
 *   1. Client sends HTLC_HDR_FILE_PUT with FILE_NAME + DIR +
 *      HTXF_SIZE.
 *   2. Server replies with HTLS_HDR_TASK carrying HTLS_DATA_HTXF_REF
 *      (and possibly an HTLS_DATA_RFLT for resume info).
 *   3. Client opens the HTXF subchannel, sends the 16-byte HTXF
 *      magic header, then streams a FILP-wrapped payload (115 byte
 *      header + DATA fork bytes + MACR fork header).
 *   4. Server writes the file to disk.
 *
 * This test covers steps 1-2 only — the request/reply round-trip
 * over the main channel. Step 3 needs a hand-rolled FILP wrapper
 * (~130 byte header with 8 fields hand-tuned per file) which is
 * better as a follow-up batch when we want to verify the full
 * data-on-disk round-trip.
 *
 * The setup phase tests:
 *
 *   - mhxd accepts the request and returns a non-error TASK reply
 *     with a non-zero HTXF_REF (success path), OR
 *   - mhxd rejects with a task-error (e.g. EPERM if the guest
 *     account lacks upload_anywhere AND the target dir doesn't
 *     contain 'UPLOAD'/'DROP BOX' in its name; see the access
 *     check at mhxd/src/hxd/files.c:1701).
 *
 * Either case proves the request/reply correlation works. The
 * test uploads to a path we expect to be allowed: 'Uploads' is
 * seeded in the Dockerfile, and the substring match in mhxd's
 * permission check accepts it case-insensitively.
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

/* Encode a single-component Hotline directory path. The wire
 * format is a 2-byte count + N (1-byte unknown + 2-byte name-len
 * + name) records. We encode just one component here. The output
 * format mirrors what mhxd's hldir_to_path expects on the receive
 * side.
 *
 * Returns the encoded length. The caller's `out` buffer must hold
 * at least 5 + strlen(name) bytes. */
static gsize
encode_hldir_one (guint8 *out, const char *name)
{
    gsize nlen = strlen (name);
    guint16 count_be = htons (1);
    guint16 nlen_be = htons ((guint16)nlen);

    memcpy (out + 0, &count_be, 2); /* component count */
    out[2] = 0;                     /* unknown / reserved */
    memcpy (out + 3, &nlen_be, 2);  /* name length */
    memcpy (out + 5, name, nlen);
    return 5 + nlen;
}

static void
test_file_put_request_reply (void)
{
    struct htlc_conn htlc;
    int fd = integration_open_login_or_skip (&htlc, "FilePut Tier-3", 412);
    if (fd < 0) {
        return;
    }

    /* Build the DIR chunk: single component "Uploads". */
    guint8 hldir[64];
    gsize hldir_len = encode_hldir_one (hldir, "Uploads");

    gchar *fname = g_strdup_printf ("tier3_put_%u.txt", (guint)g_random_int ());
    guint32 size_be = htonl (32); /* fake "we want to upload 32 bytes" */
    guint32 our_trans = htlc.trans;

    g_assert_true (integration_send_message (
        fd, &htlc, HTLC_HDR_FILE_PUT, /*flag=*/0, /*hc=*/3,
        (int)HTLC_DATA_FILE_NAME, (int)strlen (fname), (guint8 *)fname,
        (int)HTLC_DATA_DIR, (int)hldir_len, hldir, (int)HTLC_DATA_HTXF_SIZE,
        (int)sizeof (size_be), &size_be));

    /* Drain to TASK reply matching our trans. */
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
        /* Server rejected the request. Surface the message; the
		 * test still passes because the request/reply round-trip
		 * itself worked. Common cause: guest lacks
		 * upload_anywhere and the substring-match permission gate
		 * declined. */
        char err[256];
        gsize err_len = 0;
        if (task_error_extract (&htlc, err, sizeof (err), &err_len)) {
            g_test_message ("file_put refused: \"%s\"", err);
        } else {
            g_test_message ("file_put refused (no error chunk)");
        }
    } else {
        /* Successful setup. Pull HTXF_REF out — should be
		 * non-zero. */
        guint32 xfer_ref = 0;
        dh_start (&htlc)
        {
            if (_type == HTLS_DATA_HTXF_REF) {
                dh_getint (xfer_ref);
            }
        }
        dh_end ();
        g_assert_cmphex (xfer_ref, !=, 0);
        g_test_message ("file_put accepted; ref=0x%08x", (unsigned)xfer_ref);

        /* TODO future batch: connect to the HTXF subchannel,
		 * send a hand-rolled FILP-wrapped payload, verify mhxd
		 * writes the file. Skipping the upload phase here keeps
		 * the test scoped to the request/reply round-trip. */
    }

    g_free (fname);
    integration_release_htlc (&htlc);
    integration_close (fd);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/integration/file_put/request_reply",
                     test_file_put_request_reply);

    return g_test_run ();
}
