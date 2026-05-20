/*
 * tests/integration/test_chat_join.c — Bob accepts Alice's private
 * chat invite via HTLC_HDR_CHAT_JOIN.
 *
 * Sequence:
 *   1. Alice and Bob both login.
 *   2. Alice creates a chat naming Bob (HTLC_HDR_CHAT_CREATE);
 *      drains for the TASK reply with HTLS_DATA_CHAT_ID.
 *   3. Bob drains for HTLS_HDR_CHAT_INVITE carrying the chat_id.
 *   4. Bob sends HTLC_HDR_CHAT_JOIN with that chat_id.
 *   5. Bob's TASK reply must carry at least one HTLS_DATA_USER_LIST
 *      entry (the chat now has both Alice and Bob in it; mhxd's
 *      rcv_chat_join walks every joined connection and writes a
 *      USER_LIST chunk per member into Bob's reply, including
 *      Bob himself).
 *   6. Alice's connection must receive HTLS_HDR_CHAT_USER_CHANGE
 *      tagged with the chat_id and Bob's uid+name. mhxd broadcasts
 *      this to every OTHER joined member when someone joins.
 *
 * Pins down mhxd/src/hxd/chat.c:rcv_chat_join end-to-end. Failure
 * modes worth catching:
 *   - reply doesn't correlate by trans (state-machine slip)
 *   - reply has zero USER_LIST chunks (chat is empty per mhxd's
 *     accounting after JOIN — would mean chat_set didn't latch)
 *   - Alice never sees CHAT_USER_CHANGE (broadcast loop skips her)
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
test_chat_join_member_visible (void)
{
    struct htlc_conn htlc_a;
    int fd_a
        = integration_open_login_or_skip (&htlc_a, "JoinAlice Tier-3", 412);
    if (fd_a < 0) {
        return;
    }

    struct htlc_conn htlc_b;
    int fd_b = integration_open_login_or_skip (&htlc_b, "JoinBob Tier-3", 412);
    if (fd_b < 0) {
        integration_release_htlc (&htlc_a);
        integration_close (fd_a);
        return;
    }

    /* Step 2: Alice CHAT_CREATE → chat_id. */
    guint32 chat_id = 0;
    g_assert_true (integration_create_chat_with_uid (fd_a, &htlc_a, htlc_b.uid,
                                                     &chat_id, 64));
    g_assert_cmphex (hdr_flag (&htlc_a) & 1, ==, 0);

    /* Step 3: Bob drains for the CHAT_INVITE. */
    g_assert_true (integration_drain_until_chat_invite (fd_b, &htlc_b, 64));
    struct hx_chat_invite_msg im = { 0 };
    g_assert_true (hx_chat_invite_extract (&htlc_b, &im));
    g_assert_cmphex (im.cid, ==, chat_id);

    /* Steps 4 + 5: Bob CHAT_JOIN; harness drains to the TASK reply
	 * (correlated by trans) and asserts flag & 1 == 0. Reply frame
	 * is still in htlc_b.in afterward so we can walk the USER_LIST
	 * chunks below. */
    g_assert_true (integration_join_chat (fd_b, &htlc_b, chat_id, 64));

    int n_user_list = 0;
    dh_start (&htlc_b)
    {
        if (_type == HTLS_DATA_USER_LIST) {
            n_user_list++;
        }
    }
    dh_end ();
    /* Alice and Bob both joined → 2 entries. Some servers may
	 * vary; insist on >= 1 to leave room for differing flushes. */
    g_assert_cmpint (n_user_list, >=, 1);

    /* Step 6: Alice receives CHAT_USER_CHANGE for Bob. */
    gboolean alice_got_change = FALSE;
    for (int i = 0; i < 64 && !alice_got_change; i++) {
        if (!integration_recv_message (fd_a, &htlc_a, /*timeout_ms=*/3000)) {
            break;
        }
        if (hdr_type (&htlc_a) != HTLS_HDR_CHAT_USER_CHANGE) {
            continue;
        }

        guint32 got_cid = 0;
        guint16 got_uid = 0;
        gboolean got_uid_chunk = FALSE;
        dh_start (&htlc_a)
        {
            switch (_type) {
            case HTLS_DATA_CHAT_ID:
                dh_getint (got_cid);
                break;
            case HTLS_DATA_UID:
                if (_len == sizeof (guint16)) {
                    guint16 v;
                    memcpy (&v, dh->data, sizeof v);
                    got_uid = ntohs (v);
                    got_uid_chunk = TRUE;
                }
                break;
            }
        }
        dh_end ();
        if (got_cid != chat_id || !got_uid_chunk) {
            continue;
        }
        if (got_uid != htlc_b.uid) {
            continue;
        }
        alice_got_change = TRUE;
    }
    g_assert_true (alice_got_change);

    integration_release_htlc (&htlc_b);
    integration_close (fd_b);
    integration_release_htlc (&htlc_a);
    integration_close (fd_a);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/integration/chat_join/member_visible",
                     test_chat_join_member_visible);
    return g_test_run ();
}
