/*
 * tests/integration/test_unauthorized_opcode.c — sending a
 * privileged opcode (FILE_MKDIR) as guest is rejected by the
 * mhxd dispatcher with a TASK task-error reply.
 *
 * mhxd's main rcv loop (mhxd/src/hxd/rcv.c around line 380+)
 * gates each privileged HTLC_HDR_* on the corresponding access
 * bit BEFORE setting htlc->rcv. When the bit is unset, htlc->rcv
 * stays NULL and the post-switch tail at rcv.c:580 emits
 *
 *   snd_errorstr(htlc, "Transaction rejected. (Unknown or non-authorised)");
 *
 * — i.e. an HTLS_HDR_TASK with the error bit set, correlated to
 * our trans. FILE_MKDIR is gated on htlc->access.create_folders.
 *
 * The default upstream guest UserData has all eight byte-0
 * access bits set, which would make FILE_MKDIR pass through and
 * give a misleading 'guest can mkdir' result. Our Dockerfile
 * patches guest's binary UserData at byte 4 with a 0x60 mask —
 * keeping download_files (bit 5) and upload_files (bit 6),
 * clearing everything else including create_folders (bit 2). See
 * the comment block in tests/mhxd/Dockerfile next to the dd patch.
 *
 * Test contract:
 *   1. Login as guest.
 *   2. Send HTLC_HDR_FILE_MKDIR for some name.
 *   3. Drain to TASK with our trans. Assert flag bit 1 is set
 *      (rejected) and that NO non-error TASK reply for our trans
 *      ever arrives — the directory must not actually be created.
 *   4. Round-trip a PING; the pong-trans must match. Proves the
 *      dispatcher's rejection path left the stream in a clean
 *      state.
 *
 * Catches a regression where the dispatcher accidentally lets a
 * privileged opcode through when the access bit is clear.
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
test_unauthorized_mkdir_silently_dropped (void)
{
    struct htlc_conn htlc;
    int fd = integration_open_login_or_skip (&htlc, "UnauthMkdir T-3", 412);
    if (fd < 0) {
        return;
    }

    /* MKDIR-trans we'll watch for AND not expect to see. */
    guint32 mkdir_trans = htlc.trans;

    const char *new_dir = "tier3_unauth_test_dir";
    g_assert_true (integration_send_message (
        fd, &htlc, HTLC_HDR_FILE_MKDIR, /*flag=*/0, /*hc=*/1,
        (int)HTLC_DATA_FILE_NAME, (int)strlen (new_dir), (guint8 *)new_dir));

    /* Drain to the TASK reply matching our trans. With guest's
	 * create_folders bit cleared, the dispatcher's post-switch
	 * tail emits HTLS_HDR_TASK with flag=1 carrying a
	 * "Transaction rejected" TASKERROR chunk. */
    gboolean got_reject = FALSE;
    gboolean got_success = FALSE;
    for (int i = 0; i < 32 && !got_reject && !got_success; i++) {
        g_assert_true (
            integration_recv_message (fd, &htlc, /*timeout_ms=*/3000));
        if (hdr_type (&htlc) != HTLS_HDR_TASK) {
            continue;
        }
        if (hdr_trans (&htlc) != mkdir_trans) {
            continue;
        }
        if (hdr_flag (&htlc) & 1) {
            got_reject = TRUE;
            char err[256];
            gsize err_len = 0;
            if (task_error_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, err, sizeof (err), &err_len)) {
                g_test_message ("server rejected mkdir: \"%s\"", err);
            }
        } else {
            got_success = TRUE;
        }
    }
    g_assert_false (got_success);
    g_assert_true (got_reject);

    /* Probe with PING — the dispatcher should still be in a clean
	 * state, accepting the next request and replying normally. */
    guint32 ping_trans = integration_send_ping (fd, &htlc);
    g_assert_cmpuint (ping_trans, !=, 0);

    g_assert_true (
        integration_drain_until_task_trans (fd, &htlc, ping_trans, 32));
    g_assert_cmphex (hdr_flag (&htlc) & 1, ==, 0);

    integration_release_htlc (&htlc);
    integration_close (fd);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/integration/unauthorized_opcode/mkdir_silently_dropped",
                     test_unauthorized_mkdir_silently_dropped);
    return g_test_run ();
}
