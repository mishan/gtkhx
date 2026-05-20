/*
 * tests/integration/test_ping.c — verify mhxd accepts the
 * HTLC_HDR_PING keepalive opcode without erroring.
 *
 * GtkHx sends HTLC_HDR_PING (0x000001f4) every 60 s when connected
 * to a Hotline 1.5+ server. The Phase 5 fix gated this on
 * htlc->version >= 150 so legacy servers (hlserver.com responds at
 * version=0) wouldn't error-toast every minute.
 *
 * mhxd built with --enable-hxd handles PING. We're running mhxd
 * configured as Hotline 1.8.5 (version=185 in our Dockerfile
 * patch), so PING should round-trip cleanly: send PING, receive a
 * non-error TASK reply, no toast.
 *
 * If the server ever stops accepting PING — say a future mhxd
 * commit removes the handler — this test fails loudly.
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
test_ping_round_trip (void)
{
    struct htlc_conn htlc;
    int fd = integration_open_login_or_skip (&htlc, "Ping Tier-3", 412);
    if (fd < 0) {
        return;
    }

    /* Capture the trans we'll send PING with. hlpack assigns
	 * htlc->trans's current value to the message and increments
	 * after, so the sent PING uses whatever htlc.trans is right
	 * now. */
    guint32 ping_trans = htlc.trans;

    g_assert_true (integration_send_message (fd, &htlc, HTLC_HDR_PING,
                                             /*flag=*/0, /*hc=*/0));

    /* Drain past unsolicited server messages (banners, USER_CHANGE
	 * broadcasts from parallel test connections, etc.) until the
	 * TASK matching our PING trans lands. */
    g_assert_true (
        integration_drain_until_task_trans (fd, &htlc, ping_trans, 8));

    /* Flag bit 1 is the task-error marker — must NOT be set. */
    g_assert_cmphex (hdr_flag (&htlc) & 1, ==, 0);

    integration_release_htlc (&htlc);
    integration_close (fd);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/integration/ping/round_trip", test_ping_round_trip);

    return g_test_run ();
}
