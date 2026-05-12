/*
 * tests/proto/test_chat_event.c — drive hx_chat_event_new with canned
 * raw chat-message bytes.
 *
 * Where hx_chat_extract turns a wire-format HTLS_HDR_CHAT frame into
 * sanitised body bytes, hx_chat_event_new takes that next step: parse
 * the body into a packaged HxChatEvent that downstream consumers
 * (chat.c::output_chat, notify.c::gtkhx_notify_chat) can read derived
 * facts off of without redoing the work themselves.
 *
 * The constructor's job, per proto_helpers.h:
 *   1. UTF-8-sanitise the raw bytes (gtkhx_text_to_utf8).
 *   2. Detect the "[hx]" info-prefix and stamp is_info — info lines
 *      skip the sender/body split and any downstream highlight.
 *   3. Split the line into sender_off/_len and body_off/_len via
 *      hx_chat_split_nick_body. A failed split leaves both lens 0.
 *   4. Compare sender bytes against self_nick (NULL-safe) and stamp
 *      is_self.
 *
 * These tests pin each of those steps so a future refactor that, say,
 * changes the info-prefix or skips the split won't ship silently.
 *
 * Note: hx_chat_event_new does NOT take a wire-format buffer — it's a
 * pure parser over an already-sanitised byte run. The wire_fixture
 * infrastructure isn't needed here.
 */

#include "config.h"
#include <string.h>
#include <glib.h>
#include "proto_helpers.h"

/* ---------- Basic split: "Nick: body" ---------- */

static void
test_chat_event_typical_line (void)
{
	const char *raw = " misha:  hello world";
	HxChatEvent *e = hx_chat_event_new (raw, strlen (raw), 0, NULL);

	g_assert_nonnull (e);
	g_assert_cmpuint (e->cid, ==, 0);
	g_assert_cmpstr  (e->line, ==, raw);
	g_assert_cmpuint (e->line_len, ==, strlen (raw));

	g_assert_false   (e->is_info);
	g_assert_false   (e->is_self);

	g_assert_cmpuint (e->sender_len, ==, 5);
	g_assert_true    (memcmp (e->line + e->sender_off, "misha", 5) == 0);
	g_assert_cmpuint (e->body_len, ==, strlen ("hello world"));
	g_assert_true    (memcmp (e->line + e->body_off,
	                          "hello world", e->body_len) == 0);

	hx_chat_event_free (e);
}

static void
test_chat_event_preserves_cid (void)
{
	/* cid round-trips verbatim — the constructor only stashes it. */
	const char *raw = "alice: hi";
	HxChatEvent *e = hx_chat_event_new (raw, strlen (raw), 0xdeadbeef, NULL);

	g_assert_cmphex (e->cid, ==, 0xdeadbeefu);

	hx_chat_event_free (e);
}

/* ---------- is_self ---------- */

static void
test_chat_event_is_self_matches (void)
{
	/* Sender byte-for-byte equal to self_nick: is_self TRUE. */
	const char *raw = "misha: hi all";
	HxChatEvent *e = hx_chat_event_new (raw, strlen (raw), 0, "misha");

	g_assert_true  (e->is_self);
	g_assert_false (e->is_info);
	g_assert_cmpuint (e->sender_len, ==, 5);

	hx_chat_event_free (e);
}

static void
test_chat_event_is_self_mismatch (void)
{
	const char *raw = "alice: hi all";
	HxChatEvent *e = hx_chat_event_new (raw, strlen (raw), 0, "misha");

	g_assert_false (e->is_self);
	g_assert_cmpuint (e->sender_len, ==, 5);
	g_assert_true   (memcmp (e->line + e->sender_off, "alice", 5) == 0);

	hx_chat_event_free (e);
}

static void
test_chat_event_is_self_substring_does_not_match (void)
{
	/* self_nick "mish" matches a prefix of "misha" — should NOT
	 * flip is_self. The compare requires exact length equality. */
	const char *raw = "misha: hi";
	HxChatEvent *e = hx_chat_event_new (raw, strlen (raw), 0, "mish");

	g_assert_false (e->is_self);

	hx_chat_event_free (e);
}

static void
test_chat_event_is_self_null_self_nick (void)
{
	/* NULL self_nick: is_self always FALSE, no crash. */
	const char *raw = "misha: hi";
	HxChatEvent *e = hx_chat_event_new (raw, strlen (raw), 0, NULL);

	g_assert_false (e->is_self);
	g_assert_cmpuint (e->sender_len, ==, 5);

	hx_chat_event_free (e);
}

static void
test_chat_event_is_self_empty_self_nick (void)
{
	/* Empty-string self_nick is treated the same as NULL. */
	const char *raw = "misha: hi";
	HxChatEvent *e = hx_chat_event_new (raw, strlen (raw), 0, "");

	g_assert_false (e->is_self);

	hx_chat_event_free (e);
}

/* ---------- Info-prefix detection ---------- */

static void
test_chat_event_info_prefix_detected (void)
{
	/* hx_printf_prefix emits exactly this byte sequence. The
	 * constructor must spot it AND skip the sender split (info
	 * lines aren't "Nick: body" — they're internal notices). */
	const char *raw = " \00310[\00303hx\00310]\003 reconnected";
	HxChatEvent *e = hx_chat_event_new (raw, strlen (raw), 0, NULL);

	g_assert_true    (e->is_info);
	/* No split on info lines, even when the body trailing the
	 * prefix happens to look like "name: thing". */
	g_assert_cmpuint (e->sender_len, ==, 0);
	g_assert_cmpuint (e->body_len, ==, 0);
	g_assert_false   (e->is_self);

	hx_chat_event_free (e);
}

static void
test_chat_event_info_prefix_skips_split (void)
{
	/* Even if the info-prefix trailer contains a colon-bearing
	 * line, we must not extract a sender from it. */
	const char *raw = " \00310[\00303hx\00310]\003 server: hello";
	HxChatEvent *e = hx_chat_event_new (raw, strlen (raw), 0, "server");

	g_assert_true    (e->is_info);
	g_assert_cmpuint (e->sender_len, ==, 0);
	g_assert_false   (e->is_self);     /* gated on sender_len>0 */

	hx_chat_event_free (e);
}

/* ---------- Lines that don't split ---------- */

static void
test_chat_event_no_sender_emote (void)
{
	/* Emote / raw server prose: no colon, no split. The line is
	 * still UTF-8-sanitised and preserved, sender_len and body_len
	 * both stay 0 — downstream consumers render the line verbatim. */
	const char *raw = "*** misha waves";
	HxChatEvent *e = hx_chat_event_new (raw, strlen (raw), 0, NULL);

	g_assert_false   (e->is_info);
	g_assert_cmpstr  (e->line, ==, raw);
	g_assert_cmpuint (e->sender_len, ==, 0);
	g_assert_cmpuint (e->body_len, ==, 0);
	g_assert_false   (e->is_self);

	hx_chat_event_free (e);
}

static void
test_chat_event_no_sender_long_pre_colon (void)
{
	/* Pre-colon portion exceeds the 31-byte nick cap; split must
	 * reject. Verifies the constructor honours hx_chat_split_nick_body
	 * 's URL-rejection behaviour. */
	const char *raw = " the long preamble I wrote before: was here";
	HxChatEvent *e = hx_chat_event_new (raw, strlen (raw), 0, NULL);

	g_assert_cmpuint (e->sender_len, ==, 0);
	g_assert_cmpuint (e->body_len, ==, 0);

	hx_chat_event_free (e);
}

/* ---------- Edge cases: empty / NULL inputs ---------- */

static void
test_chat_event_empty_line (void)
{
	HxChatEvent *e = hx_chat_event_new ("", 0, 7, "misha");

	g_assert_nonnull (e);
	g_assert_cmpuint (e->cid, ==, 7);
	g_assert_nonnull (e->line);          /* always g_strdup-ed */
	g_assert_cmpuint (e->line_len, ==, 0);
	g_assert_false   (e->is_info);
	g_assert_cmpuint (e->sender_len, ==, 0);
	g_assert_cmpuint (e->body_len, ==, 0);
	g_assert_false   (e->is_self);

	hx_chat_event_free (e);
}

static void
test_chat_event_null_raw (void)
{
	/* gtkhx_text_to_utf8 tolerates NULL raw (treats as empty),
	 * so the constructor mustn't crash either. */
	HxChatEvent *e = hx_chat_event_new (NULL, 0, 0, NULL);

	g_assert_nonnull (e);
	g_assert_nonnull (e->line);
	g_assert_cmpuint (e->line_len, ==, 0);
	g_assert_cmpuint (e->sender_len, ==, 0);
	g_assert_cmpuint (e->body_len, ==, 0);

	hx_chat_event_free (e);
}

/* ---------- UTF-8 sanitisation ---------- */

static void
test_chat_event_utf8_passthrough (void)
{
	/* Already-valid UTF-8 multibyte input flows through unchanged
	 * (sender slice still indexes by bytes, not characters). */
	const char raw[] = "misha: héllo wörld";
	HxChatEvent *e = hx_chat_event_new (raw, sizeof (raw) - 1, 0, NULL);

	g_assert_cmpstr  (e->line, ==, raw);
	g_assert_cmpuint (e->line_len, ==, sizeof (raw) - 1);
	g_assert_cmpuint (e->sender_len, ==, 5);
	g_assert_true    (memcmp (e->line + e->sender_off, "misha", 5) == 0);

	hx_chat_event_free (e);
}

static void
test_chat_event_mac_roman_converts (void)
{
	/* MacRoman 0xE9 = é; in raw Mac Roman it's a single high-bit
	 * byte that's invalid UTF-8. gtkhx_text_to_utf8 either converts
	 * via the fallback charset or substitutes U+FFFD — either way,
	 * the resulting line must be valid UTF-8 and the sender split
	 * must still find "misha". */
	const char raw[] = "misha: h\xe9llo";       /* the 0xe9 byte */
	HxChatEvent *e = hx_chat_event_new (raw, sizeof (raw) - 1, 0, NULL);

	g_assert_nonnull (e->line);
	g_assert_true    (g_utf8_validate (e->line, e->line_len, NULL));
	g_assert_cmpuint (e->sender_len, ==, 5);
	g_assert_true    (memcmp (e->line + e->sender_off, "misha", 5) == 0);

	hx_chat_event_free (e);
}

/* ---------- Copy / free roundtrip ---------- */

static void
test_chat_event_copy_preserves_fields (void)
{
	const char *raw = "misha: a test line";
	HxChatEvent *e = hx_chat_event_new (raw, strlen (raw), 11, "misha");
	HxChatEvent *c = hx_chat_event_copy (e);

	g_assert_nonnull (c);
	g_assert_true    (c != e);
	g_assert_true    (c->line != e->line);    /* deep copy of line */

	g_assert_cmpuint (c->cid,       ==, e->cid);
	g_assert_cmpuint (c->line_len,  ==, e->line_len);
	g_assert_cmpstr  (c->line,      ==, e->line);
	g_assert_cmpuint (c->sender_off, ==, e->sender_off);
	g_assert_cmpuint (c->sender_len, ==, e->sender_len);
	g_assert_cmpuint (c->body_off,   ==, e->body_off);
	g_assert_cmpuint (c->body_len,   ==, e->body_len);
	g_assert_true    (c->is_info == e->is_info);
	g_assert_true    (c->is_self == e->is_self);

	hx_chat_event_free (c);
	hx_chat_event_free (e);
}

static void
test_chat_event_free_null_is_noop (void)
{
	/* Same idiom as g_free: free(NULL) must be safe. */
	hx_chat_event_free (NULL);
}

static void
test_chat_event_copy_null_returns_null (void)
{
	g_assert_null (hx_chat_event_copy (NULL));
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);

	g_test_add_func ("/proto/chat_event/typical_line",
	                 test_chat_event_typical_line);
	g_test_add_func ("/proto/chat_event/preserves_cid",
	                 test_chat_event_preserves_cid);

	g_test_add_func ("/proto/chat_event/is_self_matches",
	                 test_chat_event_is_self_matches);
	g_test_add_func ("/proto/chat_event/is_self_mismatch",
	                 test_chat_event_is_self_mismatch);
	g_test_add_func ("/proto/chat_event/is_self_substring_does_not_match",
	                 test_chat_event_is_self_substring_does_not_match);
	g_test_add_func ("/proto/chat_event/is_self_null_self_nick",
	                 test_chat_event_is_self_null_self_nick);
	g_test_add_func ("/proto/chat_event/is_self_empty_self_nick",
	                 test_chat_event_is_self_empty_self_nick);

	g_test_add_func ("/proto/chat_event/info_prefix_detected",
	                 test_chat_event_info_prefix_detected);
	g_test_add_func ("/proto/chat_event/info_prefix_skips_split",
	                 test_chat_event_info_prefix_skips_split);

	g_test_add_func ("/proto/chat_event/no_sender_emote",
	                 test_chat_event_no_sender_emote);
	g_test_add_func ("/proto/chat_event/no_sender_long_pre_colon",
	                 test_chat_event_no_sender_long_pre_colon);

	g_test_add_func ("/proto/chat_event/empty_line",
	                 test_chat_event_empty_line);
	g_test_add_func ("/proto/chat_event/null_raw",
	                 test_chat_event_null_raw);

	g_test_add_func ("/proto/chat_event/utf8_passthrough",
	                 test_chat_event_utf8_passthrough);
	g_test_add_func ("/proto/chat_event/mac_roman_converts",
	                 test_chat_event_mac_roman_converts);

	g_test_add_func ("/proto/chat_event/copy_preserves_fields",
	                 test_chat_event_copy_preserves_fields);
	g_test_add_func ("/proto/chat_event/free_null_is_noop",
	                 test_chat_event_free_null_is_noop);
	g_test_add_func ("/proto/chat_event/copy_null_returns_null",
	                 test_chat_event_copy_null_returns_null);

	return g_test_run ();
}
