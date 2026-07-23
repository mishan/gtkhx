/*
 * tests/integration/test_file_list.c — exercise HTLC_HDR_FILE_LIST.
 *
 * Send the request with hc=0 (no HTLC_DATA_DIR chunk → mhxd lists
 * the root files directory at hxd_cfg.paths.files). Wait for the
 * task reply matching our trans, walk the chunks, and verify the
 * round-trip succeeded.
 *
 * This test is deliberately tolerant about WHAT shows up in the
 * list — mhxd's run/hxd/files/ directory may be empty in a fresh
 * container. The contract we pin down is:
 *
 *   - Request and reply correlate via trans.
 *   - The reply is either a successful TASK (flag=0) carrying zero
 *     or more FILE_LIST chunks, OR a task-error (flag=1) if the
 *     files/ directory doesn't exist (mhxd surfaces ENOENT).
 *
 * Either case proves the protocol round-trip works; the failure
 * we'd actually be flagging is "no reply at all."
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

static void
test_file_list_round_trip (void)
{
    struct htlc_conn htlc;
    int fd = integration_open_login_or_skip (&htlc, "FileList Tier-3", 412);
    if (fd < 0) {
        return;
    }

    guint32 our_trans = htlc.trans;
    g_assert_true (integration_send_message (fd, &htlc, HTLC_HDR_FILE_LIST,
                                             /*flag=*/0, /*hc=*/0));

    /* Drain looking for the TASK reply matching our trans. */
    g_assert_true (integration_drain_until_task_trans (
        fd, &htlc, our_trans, 64));

    /* Either a clean reply with zero or more FILE_LIST chunks, or
	 * a task-error (e.g. ENOENT if files/ doesn't exist on the
	 * server). Both prove the round-trip works. */
    guint32 flag = hdr_flag (&htlc);
    if (flag & 1) {
        /* task-error — confirm there's a TASKERROR chunk. The
		 * actual message text is server-implementation-detail. */
        char err[256];
        gsize err_len = 0;
        g_assert_true (task_error_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, err, sizeof (err), &err_len));
        g_test_message ("server returned task-error for FILE_LIST: "
                        "\"%s\" (this is fine if files/ is empty)",
                        err);
    } else {
        /* Success path — count the FILE_LIST chunks for the test
		 * log. We don't assert a specific count because the
		 * contents of mhxd's files/ are container-deployment-
		 * specific. */
        int n = 0;
        dh_start (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos)
        {
            if (_type == HTLS_DATA_FILE_LIST) {
                n++;
            }
        }
        dh_end ();
        g_test_message ("server returned %d FILE_LIST chunks", n);
    }

    integration_release_htlc (&htlc);
    integration_close (fd);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/integration/file_list/round_trip",
                     test_file_list_round_trip);

    return g_test_run ();
}
