/*
 * tests/proto/test_chat.c — drive hx_chat_extract against canned
 * HTLS_HDR_CHAT wire bodies.
 *
 * The handler in rcv.c reads three chunks from a chat message:
 *   HTLS_DATA_CHAT       — the body bytes
 *   HTLS_DATA_CHAT_ID    — which chat (cid 0 = main, anything else
 *                          is a private chat)
 *   HTLS_DATA_UID        — the speaker's UID (1.5+ servers only;
 *                          older servers omit this and the body
 *                          starts with "\nFrom: ")
 *
 * The body then gets the same sanitisation pass we covered in
 * test_text_sanitisers, plus a leading-LF strip (the "\nUser: msg"
 * Hotline format would otherwise render as a blank line). Pango
 * UTF-8 sanitisation happens later in chat.c — not in scope here.
 *
 * Coverage notes:
 *   - Pin down the ID round-trip (CHAT_ID and UID are 4 / 2 bytes
 *     respectively in the wire format; dh_getint dispatches by len)
 *   - Body sanitisation: CR→LF, ANSI, leading-LF strip
 *   - Truncation at the 8 KiB stack-buffer boundary
 *   - Empty / missing chunks
 */

#include "config.h"
#include <string.h>
#include <netinet/in.h>
#include <glib.h>
#include "protocol.h"
#include "hotline.h"
#include "proto_helpers.h"
#include "wire_fixture.h"

/* ---------- Body bytes ---------- */

static void
test_chat_extracts_simple_body (void)
{
	struct htlc_conn htlc;
	const char *body = "hello, world";
	wire_fixture_init (&htlc, HTLS_HDR_CHAT, 1, 0);
	wire_fixture_add_chunk (&htlc, HTLS_DATA_CHAT,
	                        strlen (body), body);

	struct hx_chat_msg msg;
	g_assert_true   (hx_chat_extract (&htlc, &msg));
	g_assert_cmpstr (msg.text, ==, "hello, world");
	g_assert_cmpuint (msg.text_len, ==, strlen (body));
	g_assert_cmpuint (msg.cid, ==, 0);
	g_assert_cmpuint (msg.uid, ==, 0);

	wire_fixture_free (&htlc);
}

static void
test_chat_strips_leading_newline (void)
{
	/* Hotline servers commonly format chat as "\nUser: message".
	 * The handler skips the leading LF so the chat widget doesn't
	 * render a blank line above the message. */
	struct htlc_conn htlc;
	const char *body = "\nMisha: hello!";
	wire_fixture_init (&htlc, HTLS_HDR_CHAT, 1, 0);
	wire_fixture_add_chunk (&htlc, HTLS_DATA_CHAT,
	                        strlen (body), body);

	struct hx_chat_msg msg;
	g_assert_true    (hx_chat_extract (&htlc, &msg));
	/* Note the leading '\n' is gone. */
	g_assert_cmpstr  (msg.text, ==, "Misha: hello!");
	g_assert_cmpuint (msg.text_len, ==, strlen (body) - 1);
	/* msg.text points one past msg.buf — into the same allocation. */
	g_assert_true    (msg.text == msg.buf + 1);

	wire_fixture_free (&htlc);
}

static void
test_chat_does_not_strip_internal_newline (void)
{
	/* Leading-LF strip is exactly one byte, only at position 0.
	 * Internal newlines stay. */
	struct htlc_conn htlc;
	const char *body = "alpha\nbeta\ngamma";
	wire_fixture_init (&htlc, HTLS_HDR_CHAT, 1, 0);
	wire_fixture_add_chunk (&htlc, HTLS_DATA_CHAT,
	                        strlen (body), body);

	struct hx_chat_msg msg;
	g_assert_true   (hx_chat_extract (&htlc, &msg));
	g_assert_cmpstr (msg.text, ==, "alpha\nbeta\ngamma");
	g_assert_true   (msg.text == msg.buf);  /* no leading-LF strip */

	wire_fixture_free (&htlc);
}

static void
test_chat_converts_cr_to_lf (void)
{
	/* Hotline wire endings are '\r'. CR2LF runs before the
	 * leading-LF strip — so a body of "\rUser: hi" becomes
	 * "\nUser: hi" then strips to "User: hi". */
	struct htlc_conn htlc;
	const char *body = "\rUser: hi\rline two";
	wire_fixture_init (&htlc, HTLS_HDR_CHAT, 1, 0);
	wire_fixture_add_chunk (&htlc, HTLS_DATA_CHAT,
	                        strlen (body), body);

	struct hx_chat_msg msg;
	g_assert_true   (hx_chat_extract (&htlc, &msg));
	g_assert_cmpstr (msg.text, ==, "User: hi\nline two");

	wire_fixture_free (&htlc);
}

static void
test_chat_strips_ansi (void)
{
	struct htlc_conn htlc;
	const char body[]     = "\x1b[31mred message\x1b[0m";
	const char expected[] = "[[31mred message[[0m";
	wire_fixture_init (&htlc, HTLS_HDR_CHAT, 1, 0);
	wire_fixture_add_chunk (&htlc, HTLS_DATA_CHAT,
	                        sizeof (body) - 1, body);

	struct hx_chat_msg msg;
	g_assert_true   (hx_chat_extract (&htlc, &msg));
	g_assert_cmpstr (msg.text, ==, expected);

	wire_fixture_free (&htlc);
}

/* ---------- CHAT_ID dispatch ---------- */

static void
test_chat_extracts_chat_id (void)
{
	struct htlc_conn htlc;
	const char *body = "hi";
	const guint32 cid_wire = htonl (0x12345678);
	wire_fixture_init (&htlc, HTLS_HDR_CHAT, 1, 0);
	wire_fixture_add_chunk (&htlc, HTLS_DATA_CHAT_ID,
	                        sizeof (cid_wire), &cid_wire);
	wire_fixture_add_chunk (&htlc, HTLS_DATA_CHAT,
	                        strlen (body), body);

	struct hx_chat_msg msg;
	g_assert_true   (hx_chat_extract (&htlc, &msg));
	g_assert_cmphex (msg.cid, ==, 0x12345678u);
	g_assert_cmpstr (msg.text, ==, "hi");

	wire_fixture_free (&htlc);
}

/* HTLS_DATA_UID is 2 bytes wire-format. dh_getint dispatches off
 * `_len`: 4 bytes uses HN32, anything else uses HN16. Verify the
 * 2-byte path. */
static void
test_chat_extracts_uid (void)
{
	struct htlc_conn htlc;
	const char *body = "msg";
	const guint16 uid_wire = htons (0xabcd);
	wire_fixture_init (&htlc, HTLS_HDR_CHAT, 1, 0);
	wire_fixture_add_chunk (&htlc, HTLS_DATA_UID,
	                        sizeof (uid_wire), &uid_wire);
	wire_fixture_add_chunk (&htlc, HTLS_DATA_CHAT,
	                        strlen (body), body);

	struct hx_chat_msg msg;
	g_assert_true   (hx_chat_extract (&htlc, &msg));
	g_assert_cmphex (msg.uid, ==, 0xabcdu);

	wire_fixture_free (&htlc);
}

/* ---------- Truncation at buffer size ---------- */

static void
test_chat_truncates_oversized_body (void)
{
	/* The handler caps the copy at sizeof(buf) - 1 = 8192 bytes.
	 * Send 9000 bytes of 'A', expect 8192 'A's plus NUL. */
	struct htlc_conn htlc;
	guint8 big[9000];
	memset (big, 'A', sizeof (big));
	/* DATA_CHAT chunk len is uint16 — values must fit in 65535,
	 * which 9000 does. */
	wire_fixture_init (&htlc, HTLS_HDR_CHAT, 1, 0);
	wire_fixture_add_chunk (&htlc, HTLS_DATA_CHAT,
	                        sizeof (big), big);

	struct hx_chat_msg msg;
	g_assert_true    (hx_chat_extract (&htlc, &msg));
	g_assert_cmpuint (strlen (msg.text), ==, 8192);
	for (gsize i = 0; i < 8192; i++)
		g_assert_cmphex (msg.text[i], ==, 'A');
	g_assert_cmphex  (msg.buf[8192], ==, '\0');

	wire_fixture_free (&htlc);
}

/* ---------- Empty / missing chunks ---------- */

static void
test_chat_empty_body_is_valid (void)
{
	/* Zero-length DATA_CHAT chunk: text is "", text_len == 0.
	 * Some servers send these as keepalives.
	 *
	 * Note: wire_fixture_add_chunk's dh->data field is the FAM tail
	 * of the hl_data_hdr struct, but with zero len there's nothing
	 * to memcpy and the helper handles NULL data. */
	struct htlc_conn htlc;
	wire_fixture_init (&htlc, HTLS_HDR_CHAT, 1, 0);
	wire_fixture_add_chunk (&htlc, HTLS_DATA_CHAT, 0, NULL);

	struct hx_chat_msg msg;
	g_assert_true    (hx_chat_extract (&htlc, &msg));
	g_assert_cmpstr  (msg.text, ==, "");
	g_assert_cmpuint (msg.text_len, ==, 0);

	wire_fixture_free (&htlc);
}

static void
test_chat_missing_body_is_valid (void)
{
	/* No DATA_CHAT chunk at all (bizarre but defensively handled).
	 * text == "", cid / uid pulled from whatever IDs were sent. */
	struct htlc_conn htlc;
	const guint32 cid_wire = htonl (5);
	wire_fixture_init (&htlc, HTLS_HDR_CHAT, 1, 0);
	wire_fixture_add_chunk (&htlc, HTLS_DATA_CHAT_ID,
	                        sizeof (cid_wire), &cid_wire);

	struct hx_chat_msg msg;
	g_assert_true    (hx_chat_extract (&htlc, &msg));
	g_assert_cmpstr  (msg.text, ==, "");
	g_assert_cmphex  (msg.cid, ==, 5);

	wire_fixture_free (&htlc);
}

/* ---------- API edge cases ---------- */

static void
test_chat_null_out_returns_false (void)
{
	struct htlc_conn htlc;
	const char *body = "hi";
	wire_fixture_init (&htlc, HTLS_HDR_CHAT, 1, 0);
	wire_fixture_add_chunk (&htlc, HTLS_DATA_CHAT,
	                        strlen (body), body);

	g_assert_false (hx_chat_extract (&htlc, NULL));

	wire_fixture_free (&htlc);
}

/* ---------- All three chunks in one message ---------- */

static void
test_chat_all_three_chunks_combined (void)
{
	struct htlc_conn htlc;
	const guint32 cid_wire = htonl (7);
	const guint16 uid_wire = htons (42);
	const char *body = "\nMisha: hello team";
	wire_fixture_init (&htlc, HTLS_HDR_CHAT, 1, 0);
	wire_fixture_add_chunk (&htlc, HTLS_DATA_CHAT_ID,
	                        sizeof (cid_wire), &cid_wire);
	wire_fixture_add_chunk (&htlc, HTLS_DATA_UID,
	                        sizeof (uid_wire), &uid_wire);
	wire_fixture_add_chunk (&htlc, HTLS_DATA_CHAT,
	                        strlen (body), body);

	struct hx_chat_msg msg;
	g_assert_true    (hx_chat_extract (&htlc, &msg));
	g_assert_cmphex  (msg.cid, ==, 7);
	g_assert_cmphex  (msg.uid, ==, 42);
	g_assert_cmpstr  (msg.text, ==, "Misha: hello team");

	wire_fixture_free (&htlc);
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);

	g_test_add_func ("/proto/chat/extracts_simple_body",
	                 test_chat_extracts_simple_body);
	g_test_add_func ("/proto/chat/strips_leading_newline",
	                 test_chat_strips_leading_newline);
	g_test_add_func ("/proto/chat/does_not_strip_internal_newline",
	                 test_chat_does_not_strip_internal_newline);
	g_test_add_func ("/proto/chat/converts_cr_to_lf",
	                 test_chat_converts_cr_to_lf);
	g_test_add_func ("/proto/chat/strips_ansi",
	                 test_chat_strips_ansi);

	g_test_add_func ("/proto/chat/extracts_chat_id",
	                 test_chat_extracts_chat_id);
	g_test_add_func ("/proto/chat/extracts_uid",
	                 test_chat_extracts_uid);

	g_test_add_func ("/proto/chat/truncates_oversized_body",
	                 test_chat_truncates_oversized_body);

	g_test_add_func ("/proto/chat/empty_body_is_valid",
	                 test_chat_empty_body_is_valid);
	g_test_add_func ("/proto/chat/missing_body_is_valid",
	                 test_chat_missing_body_is_valid);

	g_test_add_func ("/proto/chat/null_out_returns_false",
	                 test_chat_null_out_returns_false);

	g_test_add_func ("/proto/chat/all_three_chunks_combined",
	                 test_chat_all_three_chunks_combined);

	return g_test_run ();
}
