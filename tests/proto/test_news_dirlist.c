/*
 * tests/proto/test_news_dirlist.c — drive hx_dirlist_parse_extended
 * with canned HTLC_DATA_CATEGORYITEM (0x0143) chunk bodies.
 *
 * Background: Hotline 1.5 added threaded news. The directory-listing
 * task reply (HTLC_HDR_NEWSDIRLIST) carries one chunk per entry, in
 * one of two forms:
 *
 *   HTLC_DATA_NEWSFOLDERITEM (0x0140) — plain:    u8 type, u8 name[]
 *   HTLC_DATA_CATEGORYITEM   (0x0143) — extended: ntype + count +
 *                                       (if category: GUID + addsn +
 *                                       deletesn) + namelen + name +
 *                                       optional trailing padding
 *
 * Different 1.5+ servers pick different forms; we have to handle
 * both. We were only parsing the plain form before, so directory
 * listings from servers using the extended form (e.g. Badmoon) came
 * back empty. The parser exercised here turns one chunk body into a
 * (kind, name) pair the rcv.c handler can wrap in folder_item.
 *
 * No wire_fixture needed — hx_dirlist_parse_extended takes already-
 * extracted chunk bytes, not a wire frame.
 */

#include "config.h"
#include <string.h>
#include <glib.h>
#include "proto_helpers.h"

/* ---------- Bundle (folder) ---------- */

static void
test_dirlist_bundle_basic (void)
{
	/* ntype=2 (bundle), count=4, namelen=18, "Admin BadMoon Info",
	 * no trailing padding. */
	const guint8 wire[] = {
		0x00, 0x02,                        /* ntype = 2 */
		0x00, 0x04,                        /* count = 4 */
		0x12,                              /* namelen = 18 */
		'A','d','m','i','n',' ','B','a','d','M','o','o','n',' ',
		'I','n','f','o'
	};
	struct hx_dirlist_ext_entry e;
	g_assert_true   (hx_dirlist_parse_extended (wire, sizeof wire, &e));
	g_assert_cmpint (e.kind,     ==, 1);   /* 1 = folder */
	g_assert_cmpuint (e.name_len, ==, 18);
	g_assert_cmpstr  (e.name,     ==, "Admin BadMoon Info");
}

static void
test_dirlist_bundle_with_trailing_padding (void)
{
	/* Same payload as above, plus one byte of trailing junk that
	 * some servers append after the name (a name-length-of-byte
	 * padding artefact). The parser must surface the entry and
	 * ignore the trailing byte. Real Badmoon bytes from the
	 * upstream bug report. */
	const guint8 wire[] = {
		0x00, 0x02, 0x00, 0x04, 0x12,
		'A','d','m','i','n',' ','B','a','d','M','o','o','n',' ',
		'I','n','f','o',
		0x4e                               /* trailing pad */
	};
	struct hx_dirlist_ext_entry e;
	g_assert_true   (hx_dirlist_parse_extended (wire, sizeof wire, &e));
	g_assert_cmpint (e.kind,     ==, 1);
	g_assert_cmpstr (e.name,     ==, "Admin BadMoon Info");
	g_assert_cmpuint (e.name_len, ==, 18);
}

/* ---------- Category ---------- */

static void
test_dirlist_category_basic (void)
{
	/* Real Badmoon bytes: ntype=3 (category), count=3, 16-byte
	 * GUID, addsn=4, deletesn=1, namelen=10, "Guest Book",
	 * 3 bytes trailing padding. */
	const guint8 wire[] = {
		0x00, 0x03,                        /* ntype = 3 */
		0x00, 0x03,                        /* count = 3 */
		0xb2, 0x1d, 0x0c, 0x3c,            /* GUID[0..3] */
		0x3b, 0x5b, 0x11, 0xf0,            /* GUID[4..7] */
		0x99, 0x7e, 0x00, 0x03,            /* GUID[8..11] */
		0x93, 0xbc, 0xd5, 0xfe,            /* GUID[12..15] */
		0x00, 0x00, 0x00, 0x04,            /* addsn = 4 */
		0x00, 0x00, 0x00, 0x01,            /* deletesn = 1 */
		0x0a,                              /* namelen = 10 */
		'G','u','e','s','t',' ','B','o','o','k',
		0x30, 0x00, 0x00                   /* trailing pad */
	};
	struct hx_dirlist_ext_entry e;
	g_assert_true    (hx_dirlist_parse_extended (wire, sizeof wire, &e));
	g_assert_cmpint  (e.kind,     ==, 2);  /* 2 = category */
	g_assert_cmpuint (e.name_len, ==, 10);
	g_assert_cmpstr  (e.name,     ==, "Guest Book");
}

static void
test_dirlist_category_long_name (void)
{
	/* Category with the maximum-length name a u8 namelen can carry
	 * (255 bytes). Verifies the 256-byte name field round-trips a
	 * full namelen without clipping. */
	guint8 wire[2 + 2 + 16 + 4 + 4 + 1 + 255];
	memset (wire, 0, sizeof wire);
	wire[0] = 0x00; wire[1] = 0x03;        /* ntype = 3 */
	wire[2] = 0x00; wire[3] = 0x01;        /* count = 1 */
	/* GUID + addsn + deletesn left zero */
	wire[28] = 0xff;                       /* namelen = 255 */
	memset (wire + 29, 'A', 255);

	struct hx_dirlist_ext_entry e;
	g_assert_true    (hx_dirlist_parse_extended (wire, sizeof wire, &e));
	g_assert_cmpint  (e.kind,     ==, 2);
	g_assert_cmpuint (e.name_len, ==, 255);
	g_assert_cmpuint (strlen (e.name), ==, 255);
	for (int i = 0; i < 255; i++)
		g_assert_cmpint (e.name[i], ==, 'A');
}

/* ---------- Empty / minimal payloads ---------- */

static void
test_dirlist_bundle_empty_name (void)
{
	/* Defensively-handled corner: a bundle with namelen 0. The
	 * resulting entry has an empty name, name_len 0. Not really
	 * a valid server-side state, but the parser shouldn't crash
	 * or refuse. */
	const guint8 wire[] = {
		0x00, 0x02, 0x00, 0x00,            /* ntype=2, count=0 */
		0x00                               /* namelen = 0 */
	};
	struct hx_dirlist_ext_entry e;
	g_assert_true   (hx_dirlist_parse_extended (wire, sizeof wire, &e));
	g_assert_cmpint (e.kind,     ==, 1);
	g_assert_cmpuint (e.name_len, ==, 0);
	g_assert_cmpstr (e.name,     ==, "");
}

/* ---------- Error / malformed ---------- */

static void
test_dirlist_rejects_truncated_header (void)
{
	/* Less than 4 bytes — can't even read ntype + count. */
	const guint8 wire[] = { 0x00, 0x02, 0x00 };
	struct hx_dirlist_ext_entry e;
	g_assert_false (hx_dirlist_parse_extended (wire, sizeof wire, &e));
}

static void
test_dirlist_rejects_unknown_ntype (void)
{
	/* ntype = 99: not 2, not 3. Parser refuses the entry so the
	 * surrounding handler can skip it and keep walking. */
	const guint8 wire[] = {
		0x00, 0x63,                        /* ntype = 99 */
		0x00, 0x00,
		0x05,
		'h','e','l','l','o'
	};
	struct hx_dirlist_ext_entry e;
	g_assert_false (hx_dirlist_parse_extended (wire, sizeof wire, &e));
}

static void
test_dirlist_rejects_bundle_truncated_before_namelen (void)
{
	/* Bundle header is 4 bytes; if dlen == 4 the namelen byte
	 * itself doesn't fit. */
	const guint8 wire[] = { 0x00, 0x02, 0x00, 0x01 };
	struct hx_dirlist_ext_entry e;
	g_assert_false (hx_dirlist_parse_extended (wire, sizeof wire, &e));
}

static void
test_dirlist_rejects_category_truncated_before_namelen (void)
{
	/* Category header is 28 bytes; this is only 27, missing the
	 * deletesn's last byte (and the namelen). */
	guint8 wire[27];
	memset (wire, 0, sizeof wire);
	wire[1] = 0x03;                        /* ntype = 3 */
	struct hx_dirlist_ext_entry e;
	g_assert_false (hx_dirlist_parse_extended (wire, sizeof wire, &e));
}

static void
test_dirlist_rejects_namelen_overrun (void)
{
	/* Bundle with namelen=10 but only 5 bytes of payload. */
	const guint8 wire[] = {
		0x00, 0x02, 0x00, 0x00,
		0x0a,                              /* namelen claims 10 */
		'a','b','c','d','e'                /* only 5 actually present */
	};
	struct hx_dirlist_ext_entry e;
	g_assert_false (hx_dirlist_parse_extended (wire, sizeof wire, &e));
}

static void
test_dirlist_null_out_returns_false (void)
{
	const guint8 wire[] = { 0x00, 0x02, 0x00, 0x00, 0x00 };
	g_assert_false (hx_dirlist_parse_extended (wire, sizeof wire, NULL));
}

static void
test_dirlist_null_data_returns_false (void)
{
	struct hx_dirlist_ext_entry e;
	g_assert_false (hx_dirlist_parse_extended (NULL, 0, &e));
	g_assert_false (hx_dirlist_parse_extended (NULL, 100, &e));
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);

	g_test_add_func ("/proto/news_dirlist/bundle_basic",
	                 test_dirlist_bundle_basic);
	g_test_add_func ("/proto/news_dirlist/bundle_with_trailing_padding",
	                 test_dirlist_bundle_with_trailing_padding);

	g_test_add_func ("/proto/news_dirlist/category_basic",
	                 test_dirlist_category_basic);
	g_test_add_func ("/proto/news_dirlist/category_long_name",
	                 test_dirlist_category_long_name);

	g_test_add_func ("/proto/news_dirlist/bundle_empty_name",
	                 test_dirlist_bundle_empty_name);

	g_test_add_func ("/proto/news_dirlist/rejects_truncated_header",
	                 test_dirlist_rejects_truncated_header);
	g_test_add_func ("/proto/news_dirlist/rejects_unknown_ntype",
	                 test_dirlist_rejects_unknown_ntype);
	g_test_add_func ("/proto/news_dirlist/rejects_bundle_truncated_before_namelen",
	                 test_dirlist_rejects_bundle_truncated_before_namelen);
	g_test_add_func ("/proto/news_dirlist/rejects_category_truncated_before_namelen",
	                 test_dirlist_rejects_category_truncated_before_namelen);
	g_test_add_func ("/proto/news_dirlist/rejects_namelen_overrun",
	                 test_dirlist_rejects_namelen_overrun);

	g_test_add_func ("/proto/news_dirlist/null_out_returns_false",
	                 test_dirlist_null_out_returns_false);
	g_test_add_func ("/proto/news_dirlist/null_data_returns_false",
	                 test_dirlist_null_data_returns_false);

	return g_test_run ();
}
