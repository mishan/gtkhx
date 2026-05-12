/*
 * tests/proto/test_newscat.c — drive hx_newscat_parse against canned
 * HTLC_DATA_CATLIST wire bodies.
 *
 * The CATLIST chunk carries the 1.5 threaded-news article listing
 * (reply to HTLC_HDR_NEWSCATLIST). Wire format (chunk body, after
 * the 4-byte hl_data_hdr — see mhxd's hl_news_threadlist_hdr +
 * hl_news_thread_hdr):
 *
 *   u32 __x0           opaque
 *   u32 post_count
 *   u16 __x1           opaque
 *
 *   per post:
 *     u32  postid
 *     u16  date.base_year
 *     u16  date.pad
 *     u32  date.seconds
 *     u32  parentid
 *     u32  __flags     opaque (post header is 22 bytes total)
 *     u16  partcount
 *     pstring subject
 *     pstring sender
 *     per part:
 *       pstring mime
 *       u16     size
 *
 * The original parser in rcv.c had no bounds checks — it walked a
 * raw pointer past whatever pstring lengths the wire claimed. These
 * tests pin both the happy-path layout (so the rcv.c translation
 * keeps producing the same news_item values it used to) and the
 * malformed-input refusal cases (length-byte overrun, post_count
 * larger than the chunk could hold, etc.).
 */

#include "config.h"
#include <string.h>
#include <netinet/in.h>
#include <glib.h>
#include "protocol.h"
#include "hotline.h"
#include "proto_helpers.h"
#include "wire_fixture.h"

/* ---------- Wire-format builder ----------
 *
 * The CATLIST chunk body is a single opaque blob (everything's
 * packed inside one hl_data_hdr), so we build it into a GByteArray
 * and then wire_fixture_add_chunk it as a single chunk. */

static void
ba_append_u32 (GByteArray *ba, guint32 v)
{
	guint32 net = htonl (v);
	g_byte_array_append (ba, (const guint8 *) &net, 4);
}

static void
ba_append_u16 (GByteArray *ba, guint16 v)
{
	guint16 net = htons (v);
	g_byte_array_append (ba, (const guint8 *) &net, 2);
}

static void
ba_append_pstring (GByteArray *ba, const char *s)
{
	guint8 len = s ? (guint8) strlen (s) : 0;
	g_byte_array_append (ba, &len, 1);
	if (len)
		g_byte_array_append (ba, (const guint8 *) s, len);
}

static void
ba_append_threadlist_hdr (GByteArray *ba, guint32 post_count)
{
	ba_append_u32 (ba, 0);              /* __x0 */
	ba_append_u32 (ba, post_count);
	ba_append_u16 (ba, 0);              /* __x1 */
}

static void
ba_append_thread_hdr (GByteArray *ba,
                      guint32 postid, guint32 parentid,
                      guint16 base_year, guint16 date_pad,
                      guint32 seconds, guint16 partcount)
{
	ba_append_u32 (ba, postid);
	ba_append_u16 (ba, base_year);
	ba_append_u16 (ba, date_pad);
	ba_append_u32 (ba, seconds);
	ba_append_u32 (ba, parentid);
	ba_append_u32 (ba, 0);              /* __flags (opaque) */
	ba_append_u16 (ba, partcount);
}

/* ---------- Happy paths ---------- */

static void
test_newscat_empty_list (void)
{
	/* Threadlist with post_count = 0. Valid, but yields zero posts. */
	struct htlc_conn htlc;
	GByteArray *ba = g_byte_array_new ();
	ba_append_threadlist_hdr (ba, 0);

	wire_fixture_init (&htlc, HTLS_HDR_TASK, 1, 0);
	wire_fixture_add_chunk (&htlc, HTLC_DATA_CATLIST, ba->len, ba->data);

	struct hx_newscat r;
	g_assert_true    (hx_newscat_parse (&htlc, &r));
	g_assert_cmpuint (r.post_count, ==, 0);
	g_assert_null    (r.posts);

	hx_newscat_clear (&r);
	g_byte_array_free (ba, TRUE);
	wire_fixture_free (&htlc);
}

static void
test_newscat_single_post_no_parts (void)
{
	struct htlc_conn htlc;
	GByteArray *ba = g_byte_array_new ();
	ba_append_threadlist_hdr (ba, 1);
	ba_append_thread_hdr (ba,
	                      0x00000042,    /* postid */
	                      0,             /* parentid */
	                      2026,          /* base_year */
	                      0,             /* date_pad */
	                      12345,         /* seconds */
	                      0);            /* partcount */
	ba_append_pstring (ba, "Welcome");
	ba_append_pstring (ba, "Admin");

	wire_fixture_init (&htlc, HTLS_HDR_TASK, 1, 0);
	wire_fixture_add_chunk (&htlc, HTLC_DATA_CATLIST, ba->len, ba->data);

	struct hx_newscat r;
	g_assert_true    (hx_newscat_parse (&htlc, &r));
	g_assert_cmpuint (r.post_count, ==, 1);
	g_assert_nonnull (r.posts);

	g_assert_cmphex  (r.posts[0].postid,         ==, 0x00000042);
	g_assert_cmphex  (r.posts[0].parentid,       ==, 0);
	g_assert_cmpuint (r.posts[0].date_base_year, ==, 2026);
	g_assert_cmpuint (r.posts[0].date_pad,       ==, 0);
	g_assert_cmpuint (r.posts[0].date_seconds,   ==, 12345);
	g_assert_cmpuint (r.posts[0].partcount,      ==, 0);
	g_assert_cmpstr  (r.posts[0].subject,        ==, "Welcome");
	g_assert_cmpstr  (r.posts[0].sender,         ==, "Admin");
	g_assert_cmpuint (r.posts[0].size_total,     ==, 0);
	g_assert_null    (r.posts[0].parts);

	hx_newscat_clear (&r);
	g_byte_array_free (ba, TRUE);
	wire_fixture_free (&htlc);
}

static void
test_newscat_post_with_parts (void)
{
	/* One post with two MIME parts: text/plain (45 bytes) +
	 * text/html (123 bytes). size_total should be 168. */
	struct htlc_conn htlc;
	GByteArray *ba = g_byte_array_new ();
	ba_append_threadlist_hdr (ba, 1);
	ba_append_thread_hdr (ba, 100, 0, 1970, 0, 0, 2);
	ba_append_pstring (ba, "Subject");
	ba_append_pstring (ba, "Sender");
	ba_append_pstring (ba, "text/plain");
	ba_append_u16     (ba, 45);
	ba_append_pstring (ba, "text/html");
	ba_append_u16     (ba, 123);

	wire_fixture_init (&htlc, HTLS_HDR_TASK, 1, 0);
	wire_fixture_add_chunk (&htlc, HTLC_DATA_CATLIST, ba->len, ba->data);

	struct hx_newscat r;
	g_assert_true    (hx_newscat_parse (&htlc, &r));
	g_assert_cmpuint (r.post_count, ==, 1);

	g_assert_cmpuint (r.posts[0].partcount,  ==, 2);
	g_assert_cmpuint (r.posts[0].size_total, ==, 168);
	g_assert_nonnull (r.posts[0].parts);
	g_assert_cmpstr  (r.posts[0].parts[0].mime_type, ==, "text/plain");
	g_assert_cmpuint (r.posts[0].parts[0].size,      ==, 45);
	g_assert_cmpstr  (r.posts[0].parts[1].mime_type, ==, "text/html");
	g_assert_cmpuint (r.posts[0].parts[1].size,      ==, 123);

	hx_newscat_clear (&r);
	g_byte_array_free (ba, TRUE);
	wire_fixture_free (&htlc);
}

static void
test_newscat_multi_post_with_threading (void)
{
	/* Three posts: a root and two replies. Tests both post_count
	 * round-trip and parentid round-trip (needed for the news15
	 * tree builder to nest replies under their parent). */
	struct htlc_conn htlc;
	GByteArray *ba = g_byte_array_new ();
	ba_append_threadlist_hdr (ba, 3);

	ba_append_thread_hdr (ba, 1, 0, 0, 0, 0, 0);
	ba_append_pstring (ba, "Root");
	ba_append_pstring (ba, "alice");

	ba_append_thread_hdr (ba, 2, 1, 0, 0, 0, 0);
	ba_append_pstring (ba, "Re: Root");
	ba_append_pstring (ba, "bob");

	ba_append_thread_hdr (ba, 3, 2, 0, 0, 0, 0);
	ba_append_pstring (ba, "Re: Re: Root");
	ba_append_pstring (ba, "carol");

	wire_fixture_init (&htlc, HTLS_HDR_TASK, 1, 0);
	wire_fixture_add_chunk (&htlc, HTLC_DATA_CATLIST, ba->len, ba->data);

	struct hx_newscat r;
	g_assert_true    (hx_newscat_parse (&htlc, &r));
	g_assert_cmpuint (r.post_count, ==, 3);

	g_assert_cmpuint (r.posts[0].postid,   ==, 1);
	g_assert_cmpuint (r.posts[0].parentid, ==, 0);
	g_assert_cmpstr  (r.posts[0].subject,  ==, "Root");
	g_assert_cmpstr  (r.posts[0].sender,   ==, "alice");

	g_assert_cmpuint (r.posts[1].postid,   ==, 2);
	g_assert_cmpuint (r.posts[1].parentid, ==, 1);
	g_assert_cmpstr  (r.posts[1].subject,  ==, "Re: Root");

	g_assert_cmpuint (r.posts[2].postid,   ==, 3);
	g_assert_cmpuint (r.posts[2].parentid, ==, 2);
	g_assert_cmpstr  (r.posts[2].subject,  ==, "Re: Re: Root");
	g_assert_cmpstr  (r.posts[2].sender,   ==, "carol");

	hx_newscat_clear (&r);
	g_byte_array_free (ba, TRUE);
	wire_fixture_free (&htlc);
}

static void
test_newscat_empty_pstrings (void)
{
	/* pstring length 0 → NULL string in the parsed post. The
	 * original gtkhx parser's get_pstring macro guaranteed this
	 * (ret=NULL on *ptr==0); downstream callers know to null-check
	 * before consuming subject/sender. */
	struct htlc_conn htlc;
	GByteArray *ba = g_byte_array_new ();
	ba_append_threadlist_hdr (ba, 1);
	ba_append_thread_hdr (ba, 1, 0, 0, 0, 0, 0);
	ba_append_pstring (ba, "");
	ba_append_pstring (ba, "");

	wire_fixture_init (&htlc, HTLS_HDR_TASK, 1, 0);
	wire_fixture_add_chunk (&htlc, HTLC_DATA_CATLIST, ba->len, ba->data);

	struct hx_newscat r;
	g_assert_true (hx_newscat_parse (&htlc, &r));
	g_assert_cmpuint (r.post_count, ==, 1);
	g_assert_null    (r.posts[0].subject);
	g_assert_null    (r.posts[0].sender);

	hx_newscat_clear (&r);
	g_byte_array_free (ba, TRUE);
	wire_fixture_free (&htlc);
}

/* ---------- Malformed buffers ---------- */

static void
test_newscat_missing_chunk_rejected (void)
{
	/* No CATLIST chunk at all. */
	struct htlc_conn htlc;
	wire_fixture_init (&htlc, HTLS_HDR_TASK, 1, 0);

	struct hx_newscat r;
	g_assert_false   (hx_newscat_parse (&htlc, &r));
	g_assert_cmpuint (r.post_count, ==, 0);
	g_assert_null    (r.posts);

	wire_fixture_free (&htlc);
}

static void
test_newscat_short_header_rejected (void)
{
	/* Chunk body too small to hold the 10-byte threadlist header. */
	struct htlc_conn htlc;
	const guint8 short_bytes[5] = { 0 };
	wire_fixture_init (&htlc, HTLS_HDR_TASK, 1, 0);
	wire_fixture_add_chunk (&htlc, HTLC_DATA_CATLIST,
	                        sizeof (short_bytes), short_bytes);

	struct hx_newscat r;
	g_assert_false (hx_newscat_parse (&htlc, &r));
	g_assert_null  (r.posts);

	wire_fixture_free (&htlc);
}

static void
test_newscat_forged_post_count_rejected (void)
{
	/* post_count = 1000 but no per-post bytes follow. The defensive
	 * bound check (post_count vs remaining/24) refuses this without
	 * trying to allocate gigabytes. */
	struct htlc_conn htlc;
	GByteArray *ba = g_byte_array_new ();
	ba_append_threadlist_hdr (ba, 1000);

	wire_fixture_init (&htlc, HTLS_HDR_TASK, 1, 0);
	wire_fixture_add_chunk (&htlc, HTLC_DATA_CATLIST, ba->len, ba->data);

	struct hx_newscat r;
	g_assert_false (hx_newscat_parse (&htlc, &r));
	g_assert_null  (r.posts);

	g_byte_array_free (ba, TRUE);
	wire_fixture_free (&htlc);
}

static void
test_newscat_truncated_post_header_rejected (void)
{
	/* Claims 1 post but provides only 10 bytes (less than the
	 * 22-byte per-post header). */
	struct htlc_conn htlc;
	GByteArray *ba = g_byte_array_new ();
	ba_append_threadlist_hdr (ba, 1);
	for (int i = 0; i < 10; i++)
		g_byte_array_append (ba, (const guint8 *) "\0", 1);

	wire_fixture_init (&htlc, HTLS_HDR_TASK, 1, 0);
	wire_fixture_add_chunk (&htlc, HTLC_DATA_CATLIST, ba->len, ba->data);

	struct hx_newscat r;
	g_assert_false (hx_newscat_parse (&htlc, &r));

	g_byte_array_free (ba, TRUE);
	wire_fixture_free (&htlc);
}

static void
test_newscat_pstring_overrun_rejected (void)
{
	/* Subject pstring claims length 200 but only 3 bytes follow.
	 * The bounds check in newscat_read_pstring catches this; the
	 * caller cleans up partial allocations and the result struct
	 * comes back zeroed. */
	struct htlc_conn htlc;
	GByteArray *ba = g_byte_array_new ();
	ba_append_threadlist_hdr (ba, 1);
	ba_append_thread_hdr (ba, 1, 0, 0, 0, 0, 0);
	/* Subject pstring with bogus length. */
	const guint8 bogus_pstring[] = { 200, 'a', 'b', 'c' };
	g_byte_array_append (ba, bogus_pstring, sizeof (bogus_pstring));

	wire_fixture_init (&htlc, HTLS_HDR_TASK, 1, 0);
	wire_fixture_add_chunk (&htlc, HTLC_DATA_CATLIST, ba->len, ba->data);

	struct hx_newscat r;
	g_assert_false   (hx_newscat_parse (&htlc, &r));
	g_assert_cmpuint (r.post_count, ==, 0);
	g_assert_null    (r.posts);

	g_byte_array_free (ba, TRUE);
	wire_fixture_free (&htlc);
}

static void
test_newscat_forged_partcount_rejected (void)
{
	/* partcount = 1000 with no part bytes following. */
	struct htlc_conn htlc;
	GByteArray *ba = g_byte_array_new ();
	ba_append_threadlist_hdr (ba, 1);
	ba_append_thread_hdr (ba, 1, 0, 0, 0, 0, 1000);
	ba_append_pstring (ba, "Subj");
	ba_append_pstring (ba, "Snd");
	/* No part bytes follow. */

	wire_fixture_init (&htlc, HTLS_HDR_TASK, 1, 0);
	wire_fixture_add_chunk (&htlc, HTLC_DATA_CATLIST, ba->len, ba->data);

	struct hx_newscat r;
	g_assert_false (hx_newscat_parse (&htlc, &r));

	g_byte_array_free (ba, TRUE);
	wire_fixture_free (&htlc);
}

static void
test_newscat_part_size_truncated_rejected (void)
{
	/* partcount = 1, mime pstring present, but missing the u16
	 * size that should follow. */
	struct htlc_conn htlc;
	GByteArray *ba = g_byte_array_new ();
	ba_append_threadlist_hdr (ba, 1);
	ba_append_thread_hdr (ba, 1, 0, 0, 0, 0, 1);
	ba_append_pstring (ba, "Subj");
	ba_append_pstring (ba, "Snd");
	ba_append_pstring (ba, "text/plain");
	/* Missing u16 size. */

	wire_fixture_init (&htlc, HTLS_HDR_TASK, 1, 0);
	wire_fixture_add_chunk (&htlc, HTLC_DATA_CATLIST, ba->len, ba->data);

	struct hx_newscat r;
	g_assert_false (hx_newscat_parse (&htlc, &r));

	g_byte_array_free (ba, TRUE);
	wire_fixture_free (&htlc);
}

/* ---------- API edge cases ---------- */

static void
test_newscat_null_out_returns_false (void)
{
	struct htlc_conn htlc;
	GByteArray *ba = g_byte_array_new ();
	ba_append_threadlist_hdr (ba, 0);
	wire_fixture_init (&htlc, HTLS_HDR_TASK, 1, 0);
	wire_fixture_add_chunk (&htlc, HTLC_DATA_CATLIST, ba->len, ba->data);

	g_assert_false (hx_newscat_parse (&htlc, NULL));

	g_byte_array_free (ba, TRUE);
	wire_fixture_free (&htlc);
}

static void
test_newscat_clear_null_is_noop (void)
{
	hx_newscat_clear (NULL);
}

static void
test_newscat_clear_zero_struct_is_noop (void)
{
	/* Calling clear on a zeroed struct is safe — used by callers
	 * who allocate the struct on the stack and want to reuse it. */
	struct hx_newscat r;
	memset (&r, 0, sizeof (r));
	hx_newscat_clear (&r);
	g_assert_cmpuint (r.post_count, ==, 0);
	g_assert_null    (r.posts);
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);

	g_test_add_func ("/proto/newscat/empty_list",
	                 test_newscat_empty_list);
	g_test_add_func ("/proto/newscat/single_post_no_parts",
	                 test_newscat_single_post_no_parts);
	g_test_add_func ("/proto/newscat/post_with_parts",
	                 test_newscat_post_with_parts);
	g_test_add_func ("/proto/newscat/multi_post_with_threading",
	                 test_newscat_multi_post_with_threading);
	g_test_add_func ("/proto/newscat/empty_pstrings",
	                 test_newscat_empty_pstrings);

	g_test_add_func ("/proto/newscat/missing_chunk_rejected",
	                 test_newscat_missing_chunk_rejected);
	g_test_add_func ("/proto/newscat/short_header_rejected",
	                 test_newscat_short_header_rejected);
	g_test_add_func ("/proto/newscat/forged_post_count_rejected",
	                 test_newscat_forged_post_count_rejected);
	g_test_add_func ("/proto/newscat/truncated_post_header_rejected",
	                 test_newscat_truncated_post_header_rejected);
	g_test_add_func ("/proto/newscat/pstring_overrun_rejected",
	                 test_newscat_pstring_overrun_rejected);
	g_test_add_func ("/proto/newscat/forged_partcount_rejected",
	                 test_newscat_forged_partcount_rejected);
	g_test_add_func ("/proto/newscat/part_size_truncated_rejected",
	                 test_newscat_part_size_truncated_rejected);

	g_test_add_func ("/proto/newscat/null_out_returns_false",
	                 test_newscat_null_out_returns_false);
	g_test_add_func ("/proto/newscat/clear_null_is_noop",
	                 test_newscat_clear_null_is_noop);
	g_test_add_func ("/proto/newscat/clear_zero_struct_is_noop",
	                 test_newscat_clear_zero_struct_is_noop);

	return g_test_run ();
}
