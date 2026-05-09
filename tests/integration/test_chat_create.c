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

static guint32
hdr_type (const struct htlc_conn *htlc)
{
	const struct hl_hdr *h = (const struct hl_hdr *) htlc->in.buf;
	return ntohl (h->type);
}

static guint32
hdr_trans (const struct htlc_conn *htlc)
{
	const struct hl_hdr *h = (const struct hl_hdr *) htlc->in.buf;
	return ntohl (h->trans);
}

static guint32
hdr_flag (const struct htlc_conn *htlc)
{
	const struct hl_hdr *h = (const struct hl_hdr *) htlc->in.buf;
	return ntohl (h->flag);
}

static void
test_chat_create_invites_target (void)
{
	/* Alice connects first. */
	struct htlc_conn htlc_a;
	int fd_a = integration_open_login_or_skip (
		&htlc_a, "PChatAlice Tier-3", 412);
	if (fd_a < 0)
		return;

	/* Bob second. */
	struct htlc_conn htlc_b;
	int fd_b = integration_open_login_or_skip (
		&htlc_b, "PChatBob Tier-3", 412);
	if (fd_b < 0) {
		integration_release_htlc (&htlc_a);
		integration_close (fd_a);
		return;
	}

	/* Alice creates a private chat, inviting Bob. */
	guint16 bob_uid_be = htons (htlc_b.uid);
	guint32 our_trans = htlc_a.trans;
	g_assert_true (integration_send_message (
		fd_a, &htlc_a,
		HTLC_HDR_CHAT_CREATE, /*flag=*/0, /*hc=*/1,
		(int) HTLC_DATA_UID, (int) sizeof (bob_uid_be), &bob_uid_be));

	/* Drain Alice's connection for the TASK reply. */
	gboolean alice_got_reply = FALSE;
	guint32 chat_id = 0;
	for (int i = 0; i < 16 && !alice_got_reply; i++) {
		g_assert_true (integration_recv_message (
			fd_a, &htlc_a, /*timeout_ms=*/3000));
		if (hdr_type (&htlc_a) != HTLS_HDR_TASK)
			continue;
		if (hdr_trans (&htlc_a) != our_trans)
			continue;
		alice_got_reply = TRUE;

		/* Reply must not be a task-error. */
		g_assert_cmphex (hdr_flag (&htlc_a) & 1, ==, 0);

		/* Walk the chunks; HTLS_DATA_CHAT_ID is what matters. */
		dh_start (&htlc_a) {
			if (_type == HTLS_DATA_CHAT_ID)
				dh_getint (chat_id);
		} dh_end ();
	}
	g_assert_true (alice_got_reply);
	g_assert_cmphex (chat_id, !=, 0);

	/* On Bob's connection, drain looking for HTLS_HDR_CHAT_INVITE. */
	gboolean bob_got_invite = FALSE;
	struct hx_chat_invite_msg im = { 0 };
	for (int i = 0; i < 16 && !bob_got_invite; i++) {
		if (!integration_recv_message (
				fd_b, &htlc_b, /*timeout_ms=*/3000))
			break;
		if (hdr_type (&htlc_b) != HTLS_HDR_CHAT_INVITE)
			continue;
		if (!hx_chat_invite_extract (&htlc_b, &im))
			continue;
		bob_got_invite = TRUE;
	}
	g_assert_true (bob_got_invite);

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
