/*
 * tests/integration/test_two_client_chat.c — open two simultaneous
 * connections, log each one in as a different user, verify a chat
 * sent from client A is received by client B.
 *
 * The single-client chat test (test_chat_roundtrip.c) already
 * proves we can echo a chat back to ourselves. This test proves
 * the cross-user broadcast path: when A sends, B sees A's chat
 * with A's uid and A's name embedded.
 *
 * The two clients log in serially (one after the other) but stay
 * connected concurrently for the chat exchange. mhxd handles them
 * as separate htlc_conn entries in its connection list.
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

/* Drain looking for a HTLS_HDR_CHAT broadcast whose uid matches
 * `wanted_uid`. Same pattern as test_chat_roundtrip's filter; we
 * filter here because each test connection sees broadcasts from
 * its peer AND from any other concurrent integration test
 * binaries running in parallel. */
static gboolean
drain_until_chat_from_uid (int fd, struct htlc_conn *htlc,
                           guint16 wanted_uid,
                           struct hx_chat_msg *out,
                           int max_messages)
{
	for (int i = 0; i < max_messages; i++) {
		if (!integration_recv_message (fd, htlc, /*timeout_ms=*/3000))
			return FALSE;
		if (hdr_type (htlc) != HTLS_HDR_CHAT)
			continue;
		if (!hx_chat_extract (htlc, out))
			continue;
		if (out->uid == wanted_uid)
			return TRUE;
	}
	return FALSE;
}

static gboolean
send_chat (int fd, struct htlc_conn *htlc, const char *text)
{
	guint16 style = htons (1);
	return integration_send_message (
		fd, htlc,
		HTLC_HDR_CHAT, /*flag=*/0, /*hc=*/2,
		(int) HTLC_DATA_STYLE, (int) sizeof (style), &style,
		(int) HTLC_DATA_CHAT,  (int) strlen (text), (guint8 *) text);
}

static void
test_two_client_chat_a_to_b (void)
{
	/* Client A. */
	struct htlc_conn htlc_a;
	int fd_a = integration_open_login_or_skip (
		&htlc_a, "Alice Tier-3", 412);
	if (fd_a < 0)
		return;

	/* Client B — second connection while A is still up. */
	struct htlc_conn htlc_b;
	int fd_b = integration_open_login_or_skip (
		&htlc_b, "Bob Tier-3", 412);
	if (fd_b < 0) {
		/* Even if B fails to log in, clean up A. */
		integration_release_htlc (&htlc_a);
		integration_close (fd_a);
		return;
	}

	/* A sends a unique line. B should see it as a CHAT broadcast
	 * with A's uid and A's name in the body. */
	const char *line = "two-client integration ping";
	g_assert_true (send_chat (fd_a, &htlc_a, line));

	struct hx_chat_msg cm;
	g_assert_true (drain_until_chat_from_uid (
		fd_b, &htlc_b, htlc_a.uid, &cm,
		/*max_messages=*/64));

	/* B's view of the broadcast: A's uid, A's name in the
	 * formatted body, and our line in there too. */
	g_assert_cmphex (cm.uid, ==, htlc_a.uid);
	g_assert_nonnull (g_strstr_len (cm.text, cm.text_len, line));
	g_assert_nonnull (g_strstr_len (cm.text, cm.text_len, "Alice Tier-3"));

	integration_release_htlc (&htlc_b);
	integration_close (fd_b);

	integration_release_htlc (&htlc_a);
	integration_close (fd_a);
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);

	g_test_add_func ("/integration/two_client_chat/a_to_b",
	                 test_two_client_chat_a_to_b);

	return g_test_run ();
}
