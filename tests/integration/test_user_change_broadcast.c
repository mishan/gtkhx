/*
 * tests/integration/test_user_change_broadcast.c — verify the
 * server broadcasts USER_CHANGE on join and USER_PART on
 * disconnect to other connected clients.
 *
 * Sequence:
 *   1. Alice logs in.
 *   2. Bob logs in. → Alice receives HTLS_HDR_USER_CHANGE for Bob
 *      (mhxd's snd_user_join in src/hxd/snd.c).
 *   3. Bob disconnects (clean socket close). → Alice receives
 *      HTLS_HDR_USER_PART for Bob (snd_user_part).
 *
 * Both broadcasts carry HTLS_DATA_UID identifying the
 * joining/leaving user. We use the Tier 2 hx_user_change_extract
 * and hx_user_part_extract helpers to parse the chunks.
 *
 * Note: we don't filter by trans here because these are
 * unsolicited broadcasts (no trans matches anything we sent). We
 * filter by header type and by the UID inside the message.
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

/* Drain looking for an HTLS_HDR_USER_CHANGE / USER_PART matching
 * `wanted_uid`. Returns TRUE on match, FALSE on timeout. */
static gboolean
drain_for_user_change (int fd, struct htlc_conn *htlc,
                       guint16 wanted_uid, int max_messages)
{
	for (int i = 0; i < max_messages; i++) {
		if (!integration_recv_message (fd, htlc, /*timeout_ms=*/3000))
			return FALSE;
		if (hdr_type (htlc) != HTLS_HDR_USER_CHANGE)
			continue;
		struct hx_user_change_msg uc;
		if (!hx_user_change_extract (htlc, &uc))
			continue;
		if (uc.uid == wanted_uid)
			return TRUE;
	}
	return FALSE;
}

static gboolean
drain_for_user_part (int fd, struct htlc_conn *htlc,
                     guint16 wanted_uid, int max_messages)
{
	for (int i = 0; i < max_messages; i++) {
		if (!integration_recv_message (fd, htlc, /*timeout_ms=*/3000))
			return FALSE;
		if (hdr_type (htlc) != HTLS_HDR_USER_PART)
			continue;
		struct hx_user_part_msg pm;
		if (!hx_user_part_extract (htlc, &pm))
			continue;
		if (pm.uid == wanted_uid)
			return TRUE;
	}
	return FALSE;
}

static void
test_user_change_join_and_part (void)
{
	/* Alice logs in. */
	struct htlc_conn htlc_a;
	int fd_a = integration_open_login_or_skip (
		&htlc_a, "JoinAlice Tier-3", 412);
	if (fd_a < 0)
		return;

	/* Bob logs in second. */
	struct htlc_conn htlc_b;
	int fd_b = integration_open_login_or_skip (
		&htlc_b, "JoinBob Tier-3", 412);
	if (fd_b < 0) {
		integration_release_htlc (&htlc_a);
		integration_close (fd_a);
		return;
	}

	/* Alice should have received a USER_CHANGE notification for
	 * Bob's join. The broadcast may arrive before, during, or
	 * after Alice's own SELFINFO drain — we generously look up to
	 * 16 messages back through Alice's pending receive queue. */
	g_assert_true (drain_for_user_change (
		fd_a, &htlc_a, htlc_b.uid, /*max_messages=*/16));

	/* Disconnect Bob — close socket, expect mhxd to broadcast
	 * USER_PART to Alice. */
	integration_release_htlc (&htlc_b);
	integration_close (fd_b);

	g_assert_true (drain_for_user_part (
		fd_a, &htlc_a, htlc_b.uid, /*max_messages=*/16));

	integration_release_htlc (&htlc_a);
	integration_close (fd_a);
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);

	g_test_add_func ("/integration/user_change/join_and_part",
	                 test_user_change_join_and_part);

	return g_test_run ();
}
