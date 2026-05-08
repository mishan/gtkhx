/*
 * tests/proto/test_selfinfo.c — drive hx_selfinfo_parse against
 * canned wire-format HTLS_HDR_USER_SELFINFO buffers.
 *
 * SELFINFO is the message the server sends after login that tells
 * us our access bitmap, our session UID, our icon, and our display
 * name. Phase 5 made this the canonical "post-login is finished"
 * trigger — do_post_login_fetches() fires off USER_GETLIST and the
 * news fetch from here. Getting the parse wrong means downstream
 * gating (READ_NEWS, DISCONNECT_USERS, etc. on the access bitmap)
 * gates against the wrong values.
 *
 * Two payload chunks matter:
 *   HTLS_DATA_ACCESS    — exactly 8 bytes, the access bitmap.
 *   HTLS_DATA_USER_LIST — hl_userlist_hdr (uid, icon, color, nlen) +
 *                         display name. The handler quirk: the UID
 *                         is byte-swapped from htlc->uid (which the
 *                         GUI already filled in during login), not
 *                         from the chunk body.
 *
 * The fixture builder packs htlc->in.buf the same way hlwrite would.
 */

#include "config.h"
#include <string.h>
#include <netinet/in.h>
#include <glib.h>
#include "protocol.h"
#include "hotline.h"
#include "proto_helpers.h"
#include "wire_fixture.h"

/* Build a HTLS_DATA_USER_LIST chunk into a stack buffer. Returns
 * the byte length. The hl_userlist_hdr struct's first two fields
 * (type, len) overlap with hl_data_hdr — wire_fixture_add_chunk
 * writes its own dh_hdr, so we hand it just the post-header tail
 * (uid/icon/color/nlen + name). */
static gsize
build_userlist_payload (guint8 *out, gsize out_size,
                        guint16 uid, guint16 icon, guint16 color,
                        const char *name, gsize name_len)
{
	/* Layout from hotline.h, sans the leading hl_data_hdr that
	 * wire_fixture_add_chunk handles for us:
	 *
	 *   guint16 uid, icon, color, nlen
	 *   guint8  name[]
	 */
	gsize need = 8 + name_len;
	g_assert_cmpuint (need, <=, out_size);

	guint16 v;
	v = htons (uid);   memcpy (out + 0, &v, 2);
	v = htons (icon);  memcpy (out + 2, &v, 2);
	v = htons (color); memcpy (out + 4, &v, 2);
	v = htons ((guint16) name_len); memcpy (out + 6, &v, 2);
	memcpy (out + 8, name, name_len);
	return need;
}

/* ---------- Access bitmap ---------- */

static void
test_selfinfo_extracts_access_bitmap (void)
{
	struct htlc_conn htlc;
	wire_fixture_init (&htlc, HTLS_HDR_USER_SELFINFO, 1, 0);

	/* The hlserver-guest canonical access bitmap (same vector
	 * test_hl_access uses). Pin it byte-for-byte here so a future
	 * selfinfo parser that swaps endianness or shifts bytes fails
	 * loudly on a known-good value. */
	const guint8 access[8] = {
		0x60, 0x70, 0x00, 0xa0, 0x00, 0x80, 0x00, 0x00
	};
	wire_fixture_add_chunk (&htlc, HTLS_DATA_ACCESS,
	                        sizeof (access), access);

	unsigned seen = hx_selfinfo_parse (&htlc);
	g_assert_true   (seen & HX_SELFINFO_ACCESS);
	g_assert_false  (seen & HX_SELFINFO_USER_LIST);
	/* htlc.access is a guint64; compare its bytes against the wire
	 * vector. memcpy preserved the wire bytes in memory order. */
	g_assert_cmpmem (&htlc.access, 8, access, 8);

	wire_fixture_free (&htlc);
}

/* The handler explicitly checks `_len != 8` and silently skips a
 * malformed access chunk. Verify that — anything else would be a
 * memcpy-out-of-bounds bug waiting to happen. */
static void
test_selfinfo_skips_malformed_short_access (void)
{
	struct htlc_conn htlc;
	wire_fixture_init (&htlc, HTLS_HDR_USER_SELFINFO, 1, 0);

	const guint8 too_short[5] = { 0x11, 0x22, 0x33, 0x44, 0x55 };
	wire_fixture_add_chunk (&htlc, HTLS_DATA_ACCESS,
	                        sizeof (too_short), too_short);

	unsigned seen = hx_selfinfo_parse (&htlc);
	g_assert_false (seen & HX_SELFINFO_ACCESS);
	/* And htlc.access stays zeroed by wire_fixture_init. */
	g_assert_cmphex (htlc.access, ==, 0);

	wire_fixture_free (&htlc);
}

static void
test_selfinfo_skips_malformed_long_access (void)
{
	struct htlc_conn htlc;
	wire_fixture_init (&htlc, HTLS_HDR_USER_SELFINFO, 1, 0);

	guint8 too_long[16];
	memset (too_long, 0xff, sizeof (too_long));
	wire_fixture_add_chunk (&htlc, HTLS_DATA_ACCESS,
	                        sizeof (too_long), too_long);

	unsigned seen = hx_selfinfo_parse (&htlc);
	g_assert_false (seen & HX_SELFINFO_ACCESS);
	/* Crucially: nothing got copied into htlc.access (which would
	 * have to overflow into adjacent fields with a 16-byte chunk). */
	g_assert_cmphex (htlc.access, ==, 0);

	wire_fixture_free (&htlc);
}

/* ---------- User list chunk ---------- */

static void
test_selfinfo_extracts_user_list (void)
{
	struct htlc_conn htlc;
	wire_fixture_init (&htlc, HTLS_HDR_USER_SELFINFO, 1, 0);

	guint8 payload[64];
	const char *name = "Misha";
	gsize plen = build_userlist_payload (
		payload, sizeof (payload),
		/*uid*/  0,         /* unused — see HN16 quirk note below */
		/*icon*/ 412,
		/*color*/ 0,
		name, strlen (name));
	wire_fixture_add_chunk (&htlc, HTLS_DATA_USER_LIST,
	                        (guint16) plen, payload);

	unsigned seen = hx_selfinfo_parse (&htlc);
	g_assert_true   (seen & HX_SELFINFO_USER_LIST);

	g_assert_cmphex (htlc.icon, ==, 412);
	g_assert_cmpstr ((const char *) htlc.name, ==, "Misha");

	/* htlc.uid: see test_selfinfo_uid_handler_quirk — the original
	 * handler does HN16(&htlc->uid, &htlc->uid), which is a no-op
	 * with a bug (writes from[1] to both bytes). We preserve the
	 * existing behaviour here rather than fix it without a code
	 * audit; downstream callers that read htlc->uid (chat, msg)
	 * appear to set it from elsewhere. */

	wire_fixture_free (&htlc);
}

/* Pin down the HN16(&htlc->uid, &htlc->uid) quirk so a future fix
 * trips this test on purpose. The macro reads from[1] twice (because
 * the first line clobbers from[0] with from[1]), so both bytes end
 * up as the original high byte. Effect: htlc->uid's two bytes both
 * become whatever its high byte was before SELFINFO. */
static void
test_selfinfo_uid_handler_quirk (void)
{
	struct htlc_conn htlc;
	wire_fixture_init (&htlc, HTLS_HDR_USER_SELFINFO, 1, 0);

	/* Pre-set htlc->uid bytes to 0x12 0x34 in memory. */
	guint8 *uid_bytes = (guint8 *) &htlc.uid;
	uid_bytes[0] = 0x12;
	uid_bytes[1] = 0x34;

	guint8 payload[32];
	gsize plen = build_userlist_payload (
		payload, sizeof (payload),
		0, 0, 0, "x", 1);
	wire_fixture_add_chunk (&htlc, HTLS_DATA_USER_LIST,
	                        (guint16) plen, payload);

	hx_selfinfo_parse (&htlc);

	/* After the buggy HN16, both bytes equal the *original* high
	 * byte (the first MSB-source read happens before the LSB write
	 * clobbers it on most compilers). The exact post-state isn't
	 * what matters — what matters is the test fails LOUDLY if
	 * someone "fixes" the macro without auditing every call site. */
	g_assert_cmpuint (uid_bytes[0], ==, uid_bytes[1]);

	wire_fixture_free (&htlc);
}

static void
test_selfinfo_truncates_name_at_31 (void)
{
	struct htlc_conn htlc;
	wire_fixture_init (&htlc, HTLS_HDR_USER_SELFINFO, 1, 0);

	/* htlc->name is 32 bytes (31 chars + NUL). The handler caps
	 * nlen at 31 to leave room for the trailing NUL. Verify that
	 * a 64-byte name is truncated cleanly without overflowing. */
	char long_name[64];
	memset (long_name, 'X', sizeof (long_name));
	guint8 payload[80];
	gsize plen = build_userlist_payload (
		payload, sizeof (payload),
		0, 0, 0,
		long_name, sizeof (long_name));
	wire_fixture_add_chunk (&htlc, HTLS_DATA_USER_LIST,
	                        (guint16) plen, payload);

	hx_selfinfo_parse (&htlc);

	g_assert_cmpuint (strlen ((const char *) htlc.name), ==, 31);
	for (gsize i = 0; i < 31; i++)
		g_assert_cmphex (htlc.name[i], ==, 'X');
	g_assert_cmphex  (htlc.name[31], ==, '\0');

	wire_fixture_free (&htlc);
}

static void
test_selfinfo_skips_too_short_user_list (void)
{
	struct htlc_conn htlc;
	wire_fixture_init (&htlc, HTLS_HDR_USER_SELFINFO, 1, 0);

	/* hl_userlist_hdr's tail is 8 bytes (uid/icon/color/nlen);
	 * a chunk shorter than that should be skipped. */
	guint8 too_short[5] = { 0 };
	wire_fixture_add_chunk (&htlc, HTLS_DATA_USER_LIST,
	                        sizeof (too_short), too_short);

	unsigned seen = hx_selfinfo_parse (&htlc);
	g_assert_false (seen & HX_SELFINFO_USER_LIST);
	g_assert_cmpuint (htlc.icon, ==, 0);
	g_assert_cmphex (htlc.name[0], ==, 0);

	wire_fixture_free (&htlc);
}

/* ---------- Combined: access + user list ---------- */

static void
test_selfinfo_parses_both_chunks (void)
{
	struct htlc_conn htlc;
	wire_fixture_init (&htlc, HTLS_HDR_USER_SELFINFO, 1, 0);

	/* Server typically sends ACCESS first, USER_LIST second. */
	const guint8 access[8] = {
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
	};
	wire_fixture_add_chunk (&htlc, HTLS_DATA_ACCESS,
	                        sizeof (access), access);

	guint8 payload[64];
	const char *name = "admin";
	gsize plen = build_userlist_payload (
		payload, sizeof (payload),
		0, 100, 0, name, strlen (name));
	wire_fixture_add_chunk (&htlc, HTLS_DATA_USER_LIST,
	                        (guint16) plen, payload);

	unsigned seen = hx_selfinfo_parse (&htlc);
	g_assert_cmpuint (seen, ==,
	                  (HX_SELFINFO_ACCESS | HX_SELFINFO_USER_LIST));
	/* htlc.uid: see test_selfinfo_uid_handler_quirk. */
	g_assert_cmphex  (htlc.icon, ==, 100);
	g_assert_cmpstr  ((const char *) htlc.name, ==, "admin");
	/* All-ones in every byte → guint64 is 0xffffffffffffffff. */
	g_assert_cmphex  (htlc.access, ==, G_GUINT64_CONSTANT (0xffffffffffffffff));

	wire_fixture_free (&htlc);
}

static void
test_selfinfo_empty_message_returns_zero (void)
{
	struct htlc_conn htlc;
	wire_fixture_init (&htlc, HTLS_HDR_USER_SELFINFO, 1, 0);

	unsigned seen = hx_selfinfo_parse (&htlc);
	g_assert_cmpuint (seen, ==, 0);

	wire_fixture_free (&htlc);
}

static void
test_selfinfo_unrelated_chunks_are_ignored (void)
{
	struct htlc_conn htlc;
	wire_fixture_init (&htlc, HTLS_HDR_USER_SELFINFO, 1, 0);

	const guint16 some_uid = 99;
	wire_fixture_add_chunk (&htlc, HTLS_DATA_UID,
	                        sizeof (some_uid), &some_uid);
	wire_fixture_add_chunk (&htlc, HTLS_DATA_CHAT_ID,
	                        sizeof (some_uid), &some_uid);

	unsigned seen = hx_selfinfo_parse (&htlc);
	g_assert_cmpuint (seen, ==, 0);

	wire_fixture_free (&htlc);
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);

	g_test_add_func ("/proto/selfinfo/extracts_access_bitmap",
	                 test_selfinfo_extracts_access_bitmap);
	g_test_add_func ("/proto/selfinfo/skips_malformed_short_access",
	                 test_selfinfo_skips_malformed_short_access);
	g_test_add_func ("/proto/selfinfo/skips_malformed_long_access",
	                 test_selfinfo_skips_malformed_long_access);

	g_test_add_func ("/proto/selfinfo/extracts_user_list",
	                 test_selfinfo_extracts_user_list);
	g_test_add_func ("/proto/selfinfo/uid_handler_quirk",
	                 test_selfinfo_uid_handler_quirk);
	g_test_add_func ("/proto/selfinfo/truncates_name_at_31",
	                 test_selfinfo_truncates_name_at_31);
	g_test_add_func ("/proto/selfinfo/skips_too_short_user_list",
	                 test_selfinfo_skips_too_short_user_list);

	g_test_add_func ("/proto/selfinfo/parses_both_chunks",
	                 test_selfinfo_parses_both_chunks);
	g_test_add_func ("/proto/selfinfo/empty_message_returns_zero",
	                 test_selfinfo_empty_message_returns_zero);
	g_test_add_func ("/proto/selfinfo/unrelated_chunks_are_ignored",
	                 test_selfinfo_unrelated_chunks_are_ignored);

	return g_test_run ();
}
