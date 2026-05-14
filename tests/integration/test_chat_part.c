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
#include <netinet/in.h>
#include <unistd.h>
#include <glib.h>
#include "compat.h"
#include "hotline.h"
#include "protocol.h"
#include "proto_helpers.h"
#include "integration_harness.h"

static guint32
hdr_type (const struct htlc_conn *htlc)
{
    const struct hl_hdr *h = (const struct hl_hdr *)htlc->in.buf;
    return ntohl (h->type);
}

static guint32
hdr_trans (const struct htlc_conn *htlc)
{
    const struct hl_hdr *h = (const struct hl_hdr *)htlc->in.buf;
    return ntohl (h->trans);
}

static guint32
hdr_flag (const struct htlc_conn *htlc)
{
    const struct hl_hdr *h = (const struct hl_hdr *)htlc->in.buf;
    return ntohl (h->flag);
}

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
    guint16 bob_uid_be = htons (htlc_b.uid);
    guint32 alice_create_trans = htlc_a.trans;
    g_assert_true (integration_send_message (
        fd_a, &htlc_a, HTLC_HDR_CHAT_CREATE, /*flag=*/0, /*hc=*/1,
        (int)HTLC_DATA_UID, (int)sizeof (bob_uid_be), &bob_uid_be));

    guint32 chat_id = 0;
    gboolean alice_got = FALSE;
    for (int i = 0; i < 64 && !alice_got; i++) {
        g_assert_true (
            integration_recv_message (fd_a, &htlc_a, /*timeout_ms=*/3000));
        if (hdr_type (&htlc_a) != HTLS_HDR_TASK) {
            continue;
        }
        if (hdr_trans (&htlc_a) != alice_create_trans) {
            continue;
        }
        alice_got = TRUE;
        dh_start (&htlc_a)
        {
            if (_type == HTLS_DATA_CHAT_ID) {
                dh_getint (chat_id);
            }
        }
        dh_end ();
    }
    g_assert_true (alice_got);
    g_assert_cmphex (chat_id, !=, 0);

    /* Bob drains for the invite. */
    gboolean bob_got_invite = FALSE;
    for (int i = 0; i < 64 && !bob_got_invite; i++) {
        if (!integration_recv_message (fd_b, &htlc_b, /*timeout_ms=*/3000)) {
            break;
        }
        if (hdr_type (&htlc_b) == HTLS_HDR_CHAT_INVITE) {
            bob_got_invite = TRUE;
        }
    }
    g_assert_true (bob_got_invite);

    /* Bob joins. We need Bob in the chat before he can part it. */
    guint32 cid_be = htonl (chat_id);
    guint32 bob_join_trans = htlc_b.trans;
    g_assert_true (integration_send_message (
        fd_b, &htlc_b, HTLC_HDR_CHAT_JOIN, /*flag=*/0, /*hc=*/1,
        (int)HTLC_DATA_CHAT_ID, (int)sizeof (cid_be), &cid_be));

    gboolean bob_join_reply = FALSE;
    for (int i = 0; i < 64 && !bob_join_reply; i++) {
        g_assert_true (
            integration_recv_message (fd_b, &htlc_b, /*timeout_ms=*/3000));
        if (hdr_type (&htlc_b) != HTLS_HDR_TASK) {
            continue;
        }
        if (hdr_trans (&htlc_b) != bob_join_trans) {
            continue;
        }
        bob_join_reply = TRUE;
        g_assert_cmphex (hdr_flag (&htlc_b) & 1, ==, 0);
    }
    g_assert_true (bob_join_reply);

    /* Drain Alice's CHAT_USER_CHANGE for Bob (the join broadcast)
	 * so the remaining test only sees the part broadcast. */
    gboolean alice_saw_join = FALSE;
    for (int i = 0; i < 64 && !alice_saw_join; i++) {
        if (!integration_recv_message (fd_a, &htlc_a, /*timeout_ms=*/3000)) {
            break;
        }
        if (hdr_type (&htlc_a) == HTLS_HDR_CHAT_USER_CHANGE) {
            alice_saw_join = TRUE;
        }
    }
    g_assert_true (alice_saw_join);

    /* Bob parts. mhxd doesn't reply directly to the parter — the
	 * broadcast goes only to OTHER joined members. */
    g_assert_true (integration_send_message (
        fd_b, &htlc_b, HTLC_HDR_CHAT_PART, /*flag=*/0, /*hc=*/1,
        (int)HTLC_DATA_CHAT_ID, (int)sizeof (cid_be), &cid_be));

    /* Alice receives HTLS_HDR_CHAT_USER_PART for Bob. */
    gboolean alice_saw_part = FALSE;
    for (int i = 0; i < 64 && !alice_saw_part; i++) {
        if (!integration_recv_message (fd_a, &htlc_a, /*timeout_ms=*/3000)) {
            break;
        }
        if (hdr_type (&htlc_a) != HTLS_HDR_CHAT_USER_PART) {
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
        alice_saw_part = TRUE;
    }
    g_assert_true (alice_saw_part);

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
