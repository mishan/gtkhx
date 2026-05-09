/*
 * tests/integration/test_chat_roundtrip.c — send a chat message and
 * verify it comes back as a broadcast.
 *
 * Hotline servers broadcast every chat message to every user in
 * the chat, INCLUDING the sender (that's how the chat widget shows
 * our own messages). When we're the only user logged in, the
 * broadcast still goes out — to us and only us. So:
 *
 *   1. Login.
 *   2. Send HTLC_HDR_CHAT with HTLC_DATA_STYLE + HTLC_DATA_CHAT.
 *   3. Read the next HTLS_HDR_CHAT broadcast.
 *   4. Verify the body contains our text + our display name.
 *
 * mhxd reformats the chat through a chat_format template that puts
 * the user name in front, so the body we see won't be the raw
 * "hello world" we sent — it'll be something like
 * "\n MyName:  hello world". We assert on substring matches, not
 * the full string, so the test is resilient to template tweaks.
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

/* Drain server messages until we see an HTLS_HDR_CHAT broadcast
 * whose uid matches `wanted_uid`. We have to filter by uid because
 * meson runs integration test binaries in parallel: chat broadcasts
 * from other concurrent test processes (logged in under different
 * names) hit our connection too, and they'd otherwise be the first
 * HTLS_HDR_CHAT we see and trick our assertion.
 *
 * Returns TRUE if our own chat is found within `max_messages`,
 * FALSE on timeout. On success the matching message is in htlc->in
 * and `out` is filled via hx_chat_extract. */
static gboolean
drain_until_own_chat (int fd, struct htlc_conn *htlc,
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

/* Send one chat line. Mirrors the GtkHx commands.c send shape:
 *   HTLC_HDR_CHAT
 *   HTLC_DATA_STYLE = htons(1)
 *   HTLC_DATA_CHAT  = the bytes
 */
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

/* ---------- Test cases ---------- */

/* Drain budget: bumped from 4 to 16 to absorb cross-talk from
 * other concurrent integration tests sharing the same mhxd. We
 * filter by uid in drain_until_own_chat, so the budget only
 * controls how patient we are before declaring the server didn't
 * echo our chat. 16 messages × 3 s timeout = 48 s upper bound;
 * the test() target's 30 s timeout caps it lower in practice. */
#define CHAT_DRAIN_BUDGET 16

static void
test_chat_roundtrip_simple (void)
{
	struct htlc_conn htlc;
	int fd = integration_open_login_or_skip (
		&htlc, "ChatBot Tier-3", 412);
	if (fd < 0)
		return;

	const char *line = "hello from the integration suite";
	g_assert_true (send_chat (fd, &htlc, line));

	struct hx_chat_msg cm;
	g_assert_true (drain_until_own_chat (
		fd, &htlc, htlc.uid, &cm, CHAT_DRAIN_BUDGET));

	/* The server prepends the name and a colon to the body via its
	 * chat_format template. Pin down the parts that should always
	 * be present without over-specifying the exact format. */
	g_assert_nonnull (g_strstr_len (cm.text, cm.text_len, line));
	g_assert_nonnull (g_strstr_len (cm.text, cm.text_len, "ChatBot Tier-3"));

	/* cid 0 = main chat (the only chat we ever joined). */
	g_assert_cmphex (cm.cid, ==, 0);

	integration_release_htlc (&htlc);
	integration_close (fd);
}

static void
test_chat_roundtrip_unicode_payload (void)
{
	/* Make sure non-ASCII bytes survive the wire intact (mhxd
	 * doesn't re-encode the body, just runs strip_ansi-style
	 * sanitisation). UTF-8 emoji + multi-byte glyphs are the
	 * typical real-world stress case. */
	struct htlc_conn htlc;
	int fd = integration_open_login_or_skip (
		&htlc, "Unicode Tier-3", 412);
	if (fd < 0)
		return;

	const char *line = "\xe2\x98\x83 \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e";
	/* "☃ 日本語" — snowman + Japanese, both common edge cases. */

	g_assert_true (send_chat (fd, &htlc, line));

	struct hx_chat_msg cm;
	g_assert_true (drain_until_own_chat (
		fd, &htlc, htlc.uid, &cm, CHAT_DRAIN_BUDGET));
	g_assert_nonnull (g_strstr_len (cm.text, cm.text_len, line));

	integration_release_htlc (&htlc);
	integration_close (fd);
}

static void
test_chat_roundtrip_two_messages_in_order (void)
{
	struct htlc_conn htlc;
	int fd = integration_open_login_or_skip (
		&htlc, "TwoMsg Tier-3", 412);
	if (fd < 0)
		return;

	g_assert_true (send_chat (fd, &htlc, "first message"));
	g_assert_true (send_chat (fd, &htlc, "second message"));

	struct hx_chat_msg cm;

	/* Read first broadcast (filtered to our own uid). */
	g_assert_true (drain_until_own_chat (
		fd, &htlc, htlc.uid, &cm, CHAT_DRAIN_BUDGET));
	g_assert_nonnull (g_strstr_len (cm.text, cm.text_len, "first message"));

	/* Read second broadcast. */
	g_assert_true (drain_until_own_chat (
		fd, &htlc, htlc.uid, &cm, CHAT_DRAIN_BUDGET));
	g_assert_nonnull (g_strstr_len (cm.text, cm.text_len, "second message"));

	integration_release_htlc (&htlc);
	integration_close (fd);
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);

	g_test_add_func ("/integration/chat/roundtrip_simple",
	                 test_chat_roundtrip_simple);
	g_test_add_func ("/integration/chat/roundtrip_unicode_payload",
	                 test_chat_roundtrip_unicode_payload);
	g_test_add_func ("/integration/chat/roundtrip_two_messages_in_order",
	                 test_chat_roundtrip_two_messages_in_order);

	return g_test_run ();
}
