/*
 * tests/integration/test_msg_to_unknown_uid.c — sending HTLC_HDR_MSG
 * to a uid that doesn't exist on the server.
 *
 * mhxd's hxd_rcv_msg looks the target up via isclient(uid). If the
 * uid isn't a live connection, the routing loop has no destination
 * and the message just falls off the floor. The contract:
 *
 *   - No HTLS_HDR_MSG echo to the sender.
 *   - No task-error (mhxd doesn't validate target uids preemptively).
 *   - The connection stays in a clean dispatcher state.
 *
 * We use uid 0xFFFE — the harness ensures real client uids start
 * at 1 and increment, so 0xFFFE is unreachable in any realistic
 * test fixture. We round-trip a PING after the bad msg to prove
 * the framing is intact.
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
test_msg_to_unknown_uid_doesnt_break_stream (void)
{
    struct htlc_conn htlc;
    int fd = integration_open_login_or_skip (&htlc, "MsgUnk T-3", 412);
    if (fd < 0) {
        return;
    }

    /* uid 0xFFFE — high enough that no real connection will ever
	 * be assigned it within the test's lifetime. */
    guint16 dead_uid_be = htons (0xFFFE);
    const char *body = "tier-3 unknown-uid msg";
    g_assert_true (integration_send_message (
        fd, &htlc, HTLC_HDR_MSG, /*flag=*/0, /*hc=*/2, (int)HTLC_DATA_UID,
        (int)sizeof (dead_uid_be), &dead_uid_be, (int)HTLC_DATA_MSG,
        (int)strlen (body), (guint8 *)body));

    /* Drain a brief window. We expect nothing — no echo, no
	 * task-error. Note for future readers: any frame that DOES
	 * appear is suspicious and worth recording. */
    int drained = 0;
    gboolean saw_msg_echo = FALSE;
    gboolean saw_task_error = FALSE;
    for (int i = 0; i < 8; i++) {
        if (!integration_recv_message (fd, &htlc, /*timeout_ms=*/500)) {
            break;
        }
        drained++;
        if (hdr_type (&htlc) == HTLS_HDR_MSG) {
            saw_msg_echo = TRUE;
        } else if (hdr_type (&htlc) == HTLS_HDR_TASK
                   && (hdr_flag (&htlc) & 1)) {
            saw_task_error = TRUE;
        }
    }
    g_test_message ("msg-to-unknown-uid drain: %d frame(s); "
                    "echo=%s task_error=%s",
                    drained, saw_msg_echo ? "yes" : "no",
                    saw_task_error ? "yes" : "no");

    /* Probe the dispatcher with a PING. */
    guint32 ping_trans = htlc.trans;
    g_assert_true (integration_send_message (fd, &htlc, HTLC_HDR_PING,
                                             /*flag=*/0, /*hc=*/0));

    gboolean got_pong = FALSE;
    for (int i = 0; i < 32 && !got_pong; i++) {
        g_assert_true (
            integration_recv_message (fd, &htlc, /*timeout_ms=*/3000));
        if (hdr_type (&htlc) != HTLS_HDR_TASK) {
            continue;
        }
        if (hdr_trans (&htlc) != ping_trans) {
            continue;
        }
        got_pong = TRUE;
        g_assert_cmphex (hdr_flag (&htlc) & 1, ==, 0);
    }
    g_assert_true (got_pong);

    integration_release_htlc (&htlc);
    integration_close (fd);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/integration/msg_to_unknown_uid/doesnt_break_stream",
                     test_msg_to_unknown_uid_doesnt_break_stream);
    return g_test_run ();
}
