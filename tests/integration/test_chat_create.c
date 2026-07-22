/*
 * tests/integration/test_chat_create.c — private chat creation
 * and invitation flow.
 *
 * Sequence:
 *   1. Alice and Bob both login.
 *   2. Alice sends HTLC_HDR_CHAT_CREATE with HTLC_DATA_UID = Bob's
 *      uid. mhxd's rcv_chat_create allocates a new chat ref,
 *      replies to Alice with HTLS_HDR_TASK carrying chat_id +
 *      Alice's user-row info, and sends Bob HTLS_HDR_CHAT_INVITE
 *      with the chat_id + Alice's uid + name.
 *   3. Verify Alice's TASK reply has the new chat_id.
 *   4. Verify Bob's CHAT_INVITE has the same chat_id and Alice's
 *      uid + name.
 *
 * Exercises the Tier 2 hx_chat_invite_extract helper end-to-end.
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
test_chat_create_invites_target (void)
{
    /* Alice connects first. */
    struct htlc_conn htlc_a;
    int fd_a
        = integration_open_login_or_skip (&htlc_a, "PChatAlice Tier-3", 412);
    if (fd_a < 0) {
        return;
    }

    /* Bob second. */
    struct htlc_conn htlc_b;
    int fd_b = integration_open_login_or_skip (&htlc_b, "PChatBob Tier-3", 412);
    if (fd_b < 0) {
        integration_release_htlc (&htlc_a);
        integration_close (fd_a);
        return;
    }

    /* Alice creates a private chat, inviting Bob. */
    guint32 chat_id = 0;
    g_assert_true (integration_create_chat_with_uid (fd_a, &htlc_a, htlc_b.uid,
                                                     &chat_id, 64));
    /* Reply must not be a task-error. */
    g_assert_cmphex (hdr_flag (&htlc_a) & 1, ==, 0);

    /* On Bob's connection, drain looking for HTLS_HDR_CHAT_INVITE. */
    g_assert_true (integration_drain_until_chat_invite (fd_b, &htlc_b, 64));
    struct hx_chat_invite_msg im = { 0 };
    g_assert_true (hx_chat_invite_extract (hx_test_in(&htlc_b)->buf, hx_test_in(&htlc_b)->pos, &im));

    /* Bob's invite carries Alice's uid, the chat_id, and Alice's
	 * name. */
    g_assert_cmphex (im.uid, ==, htlc_a.uid);
    g_assert_cmphex (im.cid, ==, chat_id);
    g_assert_cmpstr (im.name, ==, "PChatAlice Tier-3");

    integration_release_htlc (&htlc_b);
    integration_close (fd_b);
    integration_release_htlc (&htlc_a);
    integration_close (fd_a);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/integration/chat_create/invites_target",
                     test_chat_create_invites_target);

    return g_test_run ();
}
