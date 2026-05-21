/*
 * tests/integration/test_chat_decline.c — Bob declines Alice's
 * private chat invite via HTLC_HDR_CHAT_DECLINE.
 *
 * mhxd's rcv_chat_decline (src/hxd/chat.c) is intentionally silent:
 * it just clears Bob's invited-but-not-joined flag on the chat and
 * returns. No reply to Bob, no broadcast to Alice. The test pins
 * down that contract — three failure modes worth catching:
 *
 *   - Server rejects DECLINE outright (would surface as a
 *     task-error reply on Bob's connection — we'd see one, fail).
 *   - Server fans out DECLINE as a broadcast (some forks have
 *     done this — Alice would see it; we'd fail).
 *   - Server kills the connection (an EOF on either side after
 *     the DECLINE — recv would fail / return short).
 *
 * Sequence:
 *   1. Alice and Bob both login.
 *   2. Alice CHAT_CREATE inviting Bob → chat_id.
 *   3. Bob drains for CHAT_INVITE.
 *   4. Bob CHAT_DECLINE.
 *   5. Both connections stay healthy for a follow-up PING. The
 *      ping reply correlates by trans, so an unsolicited message
 *      stuck mid-stream would fail us. (Use Alice's connection
 *      since she's the one that COULD have seen a stray broadcast
 *      after Bob's decline — this also proves no spam landed
 *      between the create and the ping.)
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
test_chat_decline_silent (void)
{
    struct htlc_conn htlc_a;
    int fd_a
        = integration_open_login_or_skip (&htlc_a, "DeclineAlice Tier-3", 412);
    if (fd_a < 0) {
        return;
    }

    struct htlc_conn htlc_b;
    int fd_b
        = integration_open_login_or_skip (&htlc_b, "DeclineBob Tier-3", 412);
    if (fd_b < 0) {
        integration_release_htlc (&htlc_a);
        integration_close (fd_a);
        return;
    }

    /* Alice creates a private chat naming Bob. */
    guint32 chat_id = 0;
    g_assert_true (integration_create_chat_with_uid (fd_a, &htlc_a, htlc_b.uid,
                                                     &chat_id, 64));

    /* Bob drains for the CHAT_INVITE so we know he's actually been
	 * invited at the protocol level (and hasn't seen anything stale
	 * left over from a previous test run). */
    g_assert_true (integration_drain_until_chat_invite (
        fd_b, &htlc_b, 64));

    /* Bob declines. Server should accept silently. */
    guint32 cid_be = htonl (chat_id);
    g_assert_true (integration_send_message (
        fd_b, &htlc_b, HTLC_HDR_CHAT_DECLINE, /*flag=*/0, /*hc=*/1,
        (int)HTLC_DATA_CHAT_ID, (int)sizeof (cid_be), &cid_be));

    /* Round-trip a PING on Alice's connection. The ping's TASK
	 * reply must correlate by trans; if the server slipped in a
	 * decline-broadcast (shouldn't happen) we'd see an unrelated
	 * frame first and the trans match would fail. */
    guint32 ping_trans = integration_send_ping (fd_a, &htlc_a);
    g_assert_cmpuint (ping_trans, !=, 0);

    g_assert_true (integration_drain_until_task_trans (
        fd_a, &htlc_a, ping_trans, 64));
    /* Ping mustn't error out either. */
    g_assert_cmphex (hdr_flag (&htlc_a) & 1, ==, 0);

    /* Same shape on Bob's connection: we expect his stream to be
	 * idle after the DECLINE, so a ping round-trip works cleanly. */
    guint32 bob_ping_trans = integration_send_ping (fd_b, &htlc_b);
    g_assert_cmpuint (bob_ping_trans, !=, 0);

    g_assert_true (integration_drain_until_task_trans (
        fd_b, &htlc_b, bob_ping_trans, 64));
    g_assert_cmphex (hdr_flag (&htlc_b) & 1, ==, 0);

    integration_release_htlc (&htlc_b);
    integration_close (fd_b);
    integration_release_htlc (&htlc_a);
    integration_close (fd_a);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/integration/chat_decline/silent",
                     test_chat_decline_silent);
    return g_test_run ();
}
