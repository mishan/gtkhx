/*
 * tests/integration/test_chat_part.c — Bob leaves a private chat
 * via HTLC_HDR_CHAT_PART after joining.
 *
 * Builds on the chat_join flow (batch 8): Alice creates a chat,
 * Bob joins, then Bob parts. Alice's connection must receive
 * HTLS_HDR_CHAT_USER_PART tagged with Bob's uid + the chat_id.
 *
 * mhxd's rcv_chat_part (mhxd/src/hxd/chat.c) clears Bob's joined
 * flag, then walks the remaining chat members and broadcasts
 * HTLS_HDR_CHAT_USER_PART with the chat_id and Bob's uid. If the
 * chat empties out (no users remain), mhxd deletes the chat
 * entirely — Alice's still in it, so deletion doesn't fire here.
 *
 * Failure modes worth catching:
 *   - Bob's PART silently drops on the server (we'd never see
 *     CHAT_USER_PART on Alice's connection).
 *   - The broadcast goes back to Bob too (ours doesn't — only
 *     to OTHER joined members, per the loop in rcv_chat_part).
 *     We don't drain Bob's connection so we won't catch that
 *     directly, but it'd surface as a mismatched pong-trans on
 *     a follow-up ping if Bob's stream had stale frames.
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
test_chat_part_broadcasts (void)
{
    struct htlc_conn htlc_a;
    int fd_a
        = integration_open_login_or_skip (&htlc_a, "PartAlice Tier-3", 412);
    if (fd_a < 0) {
        return;
    }

    struct htlc_conn htlc_b;
    int fd_b = integration_open_login_or_skip (&htlc_b, "PartBob Tier-3", 412);
    if (fd_b < 0) {
        integration_release_htlc (&htlc_a);
        integration_close (fd_a);
        return;
    }

    /* Alice creates a private chat naming Bob → chat_id. */
    guint32 chat_id = 0;
    g_assert_true (integration_create_chat_with_uid (fd_a, &htlc_a, htlc_b.uid,
                                                     &chat_id, 64));

    /* Bob drains for the invite. */
    g_assert_true (integration_drain_until_chat_invite (
        fd_b, &htlc_b, 64));

    /* Bob joins. We need Bob in the chat before he can part it. */
    g_assert_true (integration_join_chat (fd_b, &htlc_b, chat_id, 64));
    guint32 cid_be = g_htonl(chat_id);

    /* Drain Alice's CHAT_USER_CHANGE for Bob (the join broadcast)
	 * so the remaining test only sees the part broadcast. */
    g_assert_true (integration_drain_until_type (
        fd_a, &htlc_a, HTLS_HDR_CHAT_USER_CHANGE, 64));

    /* Bob parts. mhxd doesn't reply directly to the parter — the
	 * broadcast goes only to OTHER joined members. */
    g_assert_true (integration_send_message (
        fd_b, &htlc_b, HTLC_HDR_CHAT_PART, /*flag=*/0, /*hc=*/1,
        (int)HTLC_DATA_CHAT_ID, (int)sizeof (cid_be), &cid_be));

    /* Alice receives HTLS_HDR_CHAT_USER_PART for Bob — same cid,
	 * Bob's uid. */
    g_assert_true (integration_drain_until_chat_user_event (
        fd_a, &htlc_a, HTLS_HDR_CHAT_USER_PART, chat_id, htlc_b.uid, 64));

    integration_release_htlc (&htlc_b);
    integration_close (fd_b);
    integration_release_htlc (&htlc_a);
    integration_close (fd_a);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/integration/chat_part/broadcasts",
                     test_chat_part_broadcasts);
    return g_test_run ();
}
