/*
 * tests/integration/test_chat_in_pchat.c — chat message routing
 * through a private chat (cid != 0).
 *
 * The existing test_chat_roundtrip and test_two_client_chat both
 * exercise the public-chat broadcast path (no HTLC_DATA_CHAT_ID,
 * mhxd's chat_lookup_ref returns NULL → fans out to every logged-
 * in client). This test exercises the private-chat routing:
 *
 *   1. Alice and Bob both login.
 *   2. Alice creates a private chat naming Bob (CHAT_CREATE) →
 *      gets chat_id back in the TASK reply.
 *   3. Bob joins via CHAT_JOIN. Alice sees CHAT_USER_CHANGE.
 *   4. Alice sends HTLC_HDR_CHAT carrying HTLC_DATA_CHAT_ID =
 *      chat_id and HTLC_DATA_CHAT body. mhxd's rcv_chat looks up
 *      the chat by ref and only fans out to members of THIS chat.
 *   5. Bob receives HTLS_HDR_CHAT with HTLS_DATA_CHAT_ID matching
 *      the private chat_id and the body.
 *
 * Pins down the multi-cid routing that the hx_chat_extract Tier 2
 * helper parses on the receive side. A regression in mhxd's
 * chat_lookup_ref or chat_isset would surface here as Bob never
 * seeing the message OR seeing it tagged with cid=0.
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
test_chat_in_pchat_routes_to_member (void)
{
    struct htlc_conn htlc_a;
    int fd_a
        = integration_open_login_or_skip (&htlc_a, "PChatChatAlice T-3", 412);
    if (fd_a < 0) {
        return;
    }

    struct htlc_conn htlc_b;
    int fd_b
        = integration_open_login_or_skip (&htlc_b, "PChatChatBob T-3", 412);
    if (fd_b < 0) {
        integration_release_htlc (&htlc_a);
        integration_close (fd_a);
        return;
    }

    /* CHAT_CREATE → chat_id. */
    guint16 bob_uid_be = htons (htlc_b.uid);
    guint32 alice_create_trans = htlc_a.trans;
    g_assert_true (integration_send_message (
        fd_a, &htlc_a, HTLC_HDR_CHAT_CREATE, /*flag=*/0, /*hc=*/1,
        (int)HTLC_DATA_UID, (int)sizeof (bob_uid_be), &bob_uid_be));

    guint32 chat_id = 0;
    gboolean alice_got_create = FALSE;
    for (int i = 0; i < 64 && !alice_got_create; i++) {
        g_assert_true (
            integration_recv_message (fd_a, &htlc_a, /*timeout_ms=*/3000));
        if (hdr_type (&htlc_a) != HTLS_HDR_TASK) {
            continue;
        }
        if (hdr_trans (&htlc_a) != alice_create_trans) {
            continue;
        }
        alice_got_create = TRUE;
        dh_start (&htlc_a)
        {
            if (_type == HTLS_DATA_CHAT_ID) {
                dh_getint (chat_id);
            }
        }
        dh_end ();
    }
    g_assert_true (alice_got_create);
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

    /* Bob joins. */
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

    /* Drain Alice's CHAT_USER_CHANGE so the next-event-on-Alice
	 * search isn't fooled by the stale join broadcast. */
    for (int i = 0; i < 64; i++) {
        if (!integration_recv_message (fd_a, &htlc_a, /*timeout_ms=*/2000)) {
            break;
        }
        if (hdr_type (&htlc_a) == HTLS_HDR_CHAT_USER_CHANGE) {
            break;
        }
    }

    /* Alice sends a chat message addressed to the private chat. */
    const char *body = "tier-3 pchat hello";
    g_assert_true (integration_send_message (
        fd_a, &htlc_a, HTLC_HDR_CHAT, /*flag=*/0, /*hc=*/2,
        (int)HTLC_DATA_CHAT_ID, (int)sizeof (cid_be), &cid_be,
        (int)HTLC_DATA_CHAT, (int)strlen (body), (guint8 *)body));

    /* Bob receives HTLS_HDR_CHAT with the chat_id and the body. */
    gboolean bob_got_chat = FALSE;
    for (int i = 0; i < 64 && !bob_got_chat; i++) {
        if (!integration_recv_message (fd_b, &htlc_b, /*timeout_ms=*/3000)) {
            break;
        }
        if (hdr_type (&htlc_b) != HTLS_HDR_CHAT) {
            continue;
        }

        struct hx_chat_msg cm = { 0 };
        if (!hx_chat_extract (&htlc_b, &cm)) {
            continue;
        }
        if (cm.cid != chat_id) {
            continue;
        }
        /* Body should contain our payload. mhxd may add a
		 * "<name>:" prefix to the chat line per the chat handler;
		 * substring-match is the right contract. */
        g_assert_nonnull (g_strstr_len (cm.text, cm.text_len, body));
        bob_got_chat = TRUE;
    }
    g_assert_true (bob_got_chat);

    integration_release_htlc (&htlc_b);
    integration_close (fd_b);
    integration_release_htlc (&htlc_a);
    integration_close (fd_a);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/integration/chat_in_pchat/routes_to_member",
                     test_chat_in_pchat_routes_to_member);
    return g_test_run ();
}
