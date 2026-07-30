/*
 * tests/integration/test_folder_put.c — exercise the request /
 * reply phase of HTLC_HDR_FILE_PUTFOLDER (0xd5).
 *
 * The full folder-upload flow is:
 *
 *   1. Client sends HTLC_HDR_FILE_PUTFOLDER with FILE_NAME +
 *      DIR + HTXF_SIZE (+ optional FILE_NFILES, advisory).
 *   2. Server replies with HTLS_HDR_TASK carrying an
 *      HTLS_DATA_HTXF_REF (and possibly a QUEUE chunk).
 *   3. Client opens the HTXF subchannel, sends the 16-byte
 *      magic header.
 *   4. The SERVER drives the FILE_NEXT loop, writing FILE_NEXT
 *      to us. We respond with one nfi entry per local tree node
 *      (folder markers + file leaves), each leaf followed by a
 *      u32 BE per-file size and a FILP-wrapped payload streamed
 *      via file_send_one.
 *
 * This test covers steps 1-2 only — the request/reply round-trip
 * over the main channel. Step 4 needs hand-rolled FILP wrappers
 * for each file leaf (115 byte preamble + INFO/comment block +
 * DATA fork + optional MACR), same depth of plumbing the
 * existing test_file_put.c deferred for the solo PUT case.
 * Running the full upload is a follow-up batch when we want to
 * verify the data-on-disk round-trip.
 *
 * The setup phase test pins:
 *
 *   - mhxd accepts HTLC_HDR_FILE_PUTFOLDER (the opcode is wired
 *     in its dispatcher).
 *   - The TASK reply correlates by trans and either carries an
 *     HTXF_REF (success path) or a task-error (permission
 *     refused — the guest account requires the
 *     UPLOAD_FOLDERS bit, which mhxd's default config grants on
 *     directories named with 'UPLOAD' / 'DROP BOX' substrings,
 *     same logic as solo file uploads).
 *
 * Either case proves the request/reply path is healthy.
 */

#include "config.h"
#include <string.h>
#include <unistd.h>
#include <glib.h>
#include "compat.h"
#include "hotline.h"
#include "protocol.h"
#include "proto_helpers.h"
#include "integration_harness.h"

/* Single-component HTLC_DATA_DIR encoder lives in the harness as
 * integration_encode_hldir_one (shared with test_file_put.c). */

static void
test_folder_put_request_reply (void)
{
    struct htlc_conn htlc;
    int fd = integration_open_login_or_skip (&htlc, "FolderPut Tier-3", 412);
    if (fd < 0) {
        return;
    }

    /* Upload-target lives under Uploads/<random>. mhxd's
     * default config gates folder uploads on the same substring
     * permission check as solo uploads — directories whose names
     * (or some parent's name) contain 'UPLOAD' or 'DROP BOX' are
     * acceptable destinations for guest. We anchor at Uploads/. */
    guint8 hldir[64];
    gsize hldir_len = integration_encode_hldir_one (hldir, "Uploads");

    gchar *fname
        = g_strdup_printf ("tier3_putfolder_%u", (guint)g_random_int ());
    guint32 size_be = g_htonl (256); /* aggregate size hint */
    guint32 nfiles_be = g_htonl (2); /* aggregate count hint */
    guint32 our_trans = htlc.trans;

    g_assert_true (integration_send_message (
        fd, &htlc, HTLC_HDR_FILE_PUTFOLDER, /*flag=*/0, /*hc=*/4,
        (int)HTLC_DATA_FILE_NAME, (int)strlen (fname), (guint8 *)fname,
        (int)HTLC_DATA_DIR, (int)hldir_len, hldir, (int)HTLC_DATA_HTXF_SIZE,
        (int)sizeof (size_be), &size_be, (int)HTLC_DATA_FILE_NFILES,
        (int)sizeof (nfiles_be), &nfiles_be));

    /* Drain to the TASK reply matching our trans. */
    g_assert_true (
        integration_drain_until_task_trans (fd, &htlc, our_trans, 64));

    if (hdr_flag (&htlc) & 1) {
        /* Server refused. Most common reason: the guest account
         * isn't granted UPLOAD_FOLDERS, or mhxd's substring
         * permission gate didn't accept 'Uploads' as a valid
         * folder-upload target. Surface the message but treat
         * the round-trip as successful — the request reached the
         * dispatcher and got an error reply, which is all the
         * setup-phase test needs to prove. */
        char err[256];
        gsize err_len = 0;
        if (task_error_extract (hx_test_in (&htlc)->buf,
                                hx_test_in (&htlc)->pos, err, sizeof (err),
                                &err_len)) {
            g_test_message ("PUTFOLDER refused: \"%s\" (request/reply "
                            "round-trip itself worked)",
                            err);
        } else {
            g_test_message ("PUTFOLDER refused (no error chunk)");
        }
    } else {
        /* Success path. Pull HTXF_REF — should be non-zero. */
        guint32 xfer_ref = 0;
        dh_start (hx_test_in (&htlc)->buf, hx_test_in (&htlc)->pos)
        {
            if (_type == HTLS_DATA_HTXF_REF) {
                dh_getint (xfer_ref);
            }
        }
        dh_end ();
        g_assert_cmphex (xfer_ref, !=, 0);
        g_test_message ("PUTFOLDER accepted; ref=0x%08x", (unsigned)xfer_ref);

        /* TODO future batch: connect to the HTXF subchannel,
         * drive the FILE_NEXT loop (server drives, we send
         * nfi entries + FILP-wrapped payloads), verify mhxd
         * created the folder + landed both files. Skipping for
         * the same reason test_file_put skips the post-setup
         * upload phase — needs hand-rolled FILP wrappers. */
    }

    g_free (fname);
    integration_release_htlc (&htlc);
    integration_close (fd);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/integration/folder_put/request_reply",
                     test_folder_put_request_reply);

    return g_test_run ();
}
