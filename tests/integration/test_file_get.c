/*
 * tests/integration/test_file_get.c — download a file from mhxd
 * over the HTXF subchannel.
 *
 * Hotline file transfers run on a separate TCP port (main port +
 * 1, so 5501 by default). The flow is:
 *
 *   1. Login on the main port (5500).
 *   2. Send HTLC_HDR_FILE_GET with HTLC_DATA_FILE_NAME = "test.txt".
 *   3. Receive HTLS_HDR_TASK with HTLS_DATA_HTXF_REF (32-bit
 *      transfer reference) and HTLS_DATA_HTXF_SIZE (total bytes
 *      the server will stream — file size + ~133 byte FILP wrapper
 *      header overhead).
 *   4. Open a NEW TCP connection to the subchannel port (5501).
 *   5. Send the 16-byte HTXF magic header (magic + ref + size +
 *      zero).
 *   6. Read `total_size` bytes from the subchannel.
 *   7. Verify our seeded content ("hello world") appears in the
 *      stream — somewhere after the FILP wrapper header.
 *
 * The Dockerfile seeds files/test.txt with "hello world\\n" so the
 * test has a known target.
 *
 * The flat-file format mhxd wraps the content in is documented in
 * various places ("Hotline Flat File Format") with INFO/DATA/MACR
 * fork records. We don't parse it — we just substring-search the
 * streamed bytes for our seed content. That's enough to prove the
 * subchannel handshake + data delivery work end-to-end.
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
test_file_get_round_trip (void)
{
    struct htlc_conn htlc;
    int fd = integration_open_login_or_skip (&htlc, "FileGet Tier-3", 412);
    if (fd < 0) {
        return;
    }

    /* Send HTLC_HDR_FILE_GET asking for files/test.txt at the
	 * server root. We send only the FILE_NAME chunk; mhxd's
	 * rcv_file_get accepts that as "from ROOTDIR / FILE_NAME". */
    const char *fname = "test.txt";
    guint32 our_trans = htlc.trans;
    g_assert_true (integration_send_message (
        fd, &htlc, HTLC_HDR_FILE_GET, /*flag=*/0, /*hc=*/1,
        (int)HTLC_DATA_FILE_NAME, (int)strlen (fname), (guint8 *)fname));

    /* Drain to the TASK reply. */
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

    /* Pull HTXF_REF and HTXF_SIZE out of the reply. */
    guint32 xfer_ref = 0, xfer_size = 0;
    dh_start (&htlc)
    {
        switch (_type) {
        case HTLS_DATA_HTXF_REF:
            dh_getint (xfer_ref);
            break;
        case HTLS_DATA_HTXF_SIZE:
            dh_getint (xfer_size);
            break;
        }
    }
    dh_end ();
    g_assert_cmphex (xfer_ref, !=, 0);
    g_assert_cmpuint (xfer_size, >, 0);
    g_assert_cmpuint (xfer_size, <, 1024 * 1024); /* sanity cap */

    /* Open the subchannel (port 5501 by default). The docker run
	 * command needs to publish 5501; if it's unreachable, skip
	 * with a helpful pointer rather than failing the suite. */
    int xfd = integration_connect_xfer ();
    if (xfd < 0) {
        g_test_skip ("HTXF subchannel port (5501 by default) "
                     "isn't reachable. Make sure your `docker run` "
                     "publishes it: -p 5501:5501. Or set "
                     "GTKHX_TEST_XFER_PORT to a different port.");
        integration_release_htlc (&htlc);
        integration_close (fd);
        return;
    }

    /* Send the HTXF header (magic + ref + size + zero). */
    g_assert_true (integration_send_xfer_hdr (xfd, xfer_ref, xfer_size));

    /* Read the full payload. */
    guint8 *payload = g_malloc (xfer_size);
    g_assert_true (integration_recv (xfd, payload, xfer_size));

    /* Find our seed content. mhxd wraps the file in a FILP-style
	 * flat-file format with headers and fork records, so the
	 * actual "hello world" bytes are somewhere after the header
	 * preamble. Substring-search rather than parsing the format. */
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
    integration_close (xfd);

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
