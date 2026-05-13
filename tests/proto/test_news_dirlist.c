/*
 * tests/proto/test_news_dirlist.c — drive hx_news_dirlist_parse_folderitem /
 * hx_news_dirlist_parse_categoryitem with canned HTLC_DATA_NEWSFOLDERITEM
 * (0x0140) and HTLC_DATA_CATEGORYITEM (0x0143) chunk bodies.
 *
 * Background: Hotline 1.5 added two threaded-news containers — news
 * folders (which contain folders and/or categories) and news
 * categories (which contain posts). The directory-listing task
 * reply for a folder (HTLC_HDR_NEWSDIRLIST) enumerates that
 * folder's contents, one chunk per entry. Each entry independently
 * identifies as either a folder-entry or a category-entry.
 *
 * Two wire chunk types are used to encode an entry. Servers vary
 * on which one they emit; the client accepts both. Either type can
 * carry either kind of entry:
 *
 *   HTLC_DATA_NEWSFOLDERITEM (0x0140):
 *     u8 ntype + u8 name[]
 *
 *   HTLC_DATA_CATEGORYITEM   (0x0143) — carries extra per-category
 *                                       sync metadata:
 *     u16 ntype + u16 count + (if category-entry: GUID + addsn +
 *     deletesn) + u8 namelen + name + optional trailing padding
 *
 * Until recently we only parsed the NEWSFOLDERITEM encoding, so
 * directory listings from servers that emit CATEGORYITEM (e.g.
 * Badmoon) came back empty. Both parsers normalise into the same
 * struct so the rcv.c side can wrap either result in a folder_item
 * without caring which encoding arrived.
 *
 * No wire_fixture needed — both parsers take already-extracted chunk
 * bytes, not a wire frame.
 */

#include "config.h"
#include <string.h>
#include <glib.h>
#include "proto_helpers.h"

/* ---------- CATEGORYITEM, folder-entry (ntype = 2) ---------- */

static void
test_categoryitem_folder (void)
{
	/* ntype=2 (folder), count=4, namelen=18, "Admin BadMoon Info",
	 * no trailing padding. */
	const guint8 wire[] = {
		0x00, 0x02,                        /* ntype = 2 */
		0x00, 0x04,                        /* count = 4 */
		0x12,                              /* namelen = 18 */
		'A','d','m','i','n',' ','B','a','d','M','o','o','n',' ',
		'I','n','f','o'
	};
	struct hx_news_dirlist_entry e;
	g_assert_true   (hx_news_dirlist_parse_categoryitem (wire, sizeof wire, &e));
	g_assert_cmpint (e.kind,     ==, 1);   /* 1 = folder */
	g_assert_cmpuint (e.name_len, ==, 18);
	g_assert_cmpstr  (e.name,     ==, "Admin BadMoon Info");
}

static void
test_categoryitem_folder_with_trailing_padding (void)
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
	struct hx_news_dirlist_entry e;
	g_assert_true   (hx_news_dirlist_parse_categoryitem (wire, sizeof wire, &e));
	g_assert_cmpint (e.kind,     ==, 1);
	g_assert_cmpstr (e.name,     ==, "Admin BadMoon Info");
	g_assert_cmpuint (e.name_len, ==, 18);
}

/* ---------- CATEGORYITEM, category-entry (ntype = 3) ---------- */

static void
test_categoryitem_category (void)
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
	struct hx_news_dirlist_entry e;
	g_assert_true    (hx_news_dirlist_parse_categoryitem (wire, sizeof wire, &e));
	g_assert_cmpint  (e.kind,     ==, 2);  /* 2 = category */
	g_assert_cmpuint (e.name_len, ==, 10);
	g_assert_cmpstr  (e.name,     ==, "Guest Book");
}

static void
test_categoryitem_category_long_name (void)
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

	struct hx_news_dirlist_entry e;
	g_assert_true    (hx_news_dirlist_parse_categoryitem (wire, sizeof wire, &e));
	g_assert_cmpint  (e.kind,     ==, 2);
	g_assert_cmpuint (e.name_len, ==, 255);
	g_assert_cmpuint (strlen (e.name), ==, 255);
	for (int i = 0; i < 255; i++)
		g_assert_cmpint (e.name[i], ==, 'A');
}

/* ---------- Empty / minimal payloads ---------- */

static void
test_categoryitem_folder_empty_name (void)
{
	/* Defensively-handled corner: a folder-entry with namelen 0. The
	 * resulting entry has an empty name, name_len 0. Not really
	 * a valid server-side state, but the parser shouldn't crash
	 * or refuse. */
	const guint8 wire[] = {
		0x00, 0x02, 0x00, 0x00,            /* ntype=2, count=0 */
		0x00                               /* namelen = 0 */
	};
	struct hx_news_dirlist_entry e;
	g_assert_true   (hx_news_dirlist_parse_categoryitem (wire, sizeof wire, &e));
	g_assert_cmpint (e.kind,     ==, 1);
	g_assert_cmpuint (e.name_len, ==, 0);
	g_assert_cmpstr (e.name,     ==, "");
}

/* ---------- Error / malformed ---------- */

static void
test_categoryitem_rejects_truncated_header (void)
{
	/* Less than 4 bytes — can't even read ntype + count. */
	const guint8 wire[] = { 0x00, 0x02, 0x00 };
	struct hx_news_dirlist_entry e;
	g_assert_false (hx_news_dirlist_parse_categoryitem (wire, sizeof wire, &e));
}

static void
test_categoryitem_rejects_unknown_ntype (void)
{
	/* ntype = 99: not 2, not 3. Parser refuses the entry so the
	 * surrounding handler can skip it and keep walking. */
	const guint8 wire[] = {
		0x00, 0x63,                        /* ntype = 99 */
		0x00, 0x00,
		0x05,
		'h','e','l','l','o'
	};
	struct hx_news_dirlist_entry e;
	g_assert_false (hx_news_dirlist_parse_categoryitem (wire, sizeof wire, &e));
}

static void
test_categoryitem_rejects_folder_truncated_before_namelen (void)
{
	/* Folder-entry header is 4 bytes; if dlen == 4 the namelen byte
	 * itself doesn't fit. */
	const guint8 wire[] = { 0x00, 0x02, 0x00, 0x01 };
	struct hx_news_dirlist_entry e;
	g_assert_false (hx_news_dirlist_parse_categoryitem (wire, sizeof wire, &e));
}

static void
test_categoryitem_rejects_category_truncated_before_namelen (void)
{
	/* Category header is 28 bytes; this is only 27, missing the
	 * deletesn's last byte (and the namelen). */
	guint8 wire[27];
	memset (wire, 0, sizeof wire);
	wire[1] = 0x03;                        /* ntype = 3 */
	struct hx_news_dirlist_entry e;
	g_assert_false (hx_news_dirlist_parse_categoryitem (wire, sizeof wire, &e));
}

static void
test_categoryitem_rejects_namelen_overrun (void)
{
	/* Folder-entry with namelen=10 but only 5 bytes of payload. */
	const guint8 wire[] = {
		0x00, 0x02, 0x00, 0x00,
		0x0a,                              /* namelen claims 10 */
		'a','b','c','d','e'                /* only 5 actually present */
	};
	struct hx_news_dirlist_entry e;
	g_assert_false (hx_news_dirlist_parse_categoryitem (wire, sizeof wire, &e));
}

static void
test_categoryitem_null_out_returns_false (void)
{
	const guint8 wire[] = { 0x00, 0x02, 0x00, 0x00, 0x00 };
	g_assert_false (hx_news_dirlist_parse_categoryitem (wire, sizeof wire, NULL));
}

static void
test_categoryitem_null_data_returns_false (void)
{
	struct hx_news_dirlist_entry e;
	g_assert_false (hx_news_dirlist_parse_categoryitem (NULL, 0, &e));
	g_assert_false (hx_news_dirlist_parse_categoryitem (NULL, 100, &e));
}

/* ============================================================== */
/* Plain form (HTLC_DATA_NEWSFOLDERITEM, 0x0140)                   */
/* ============================================================== */

static void
test_folderitem_folder (void)
{
	/* ntype=1 → folder. Name is the rest of the chunk. */
	const guint8 wire[] = {
		0x01,                            /* ntype = 1 (folder) */
		'A','d','m','i','n',' ','B','a','d','M','o','o','n',' ',
		'I','n','f','o'
	};
	struct hx_news_dirlist_entry e;
	g_assert_true    (hx_news_dirlist_parse_folderitem (wire, sizeof wire, &e));
	g_assert_cmpint  (e.kind,     ==, 1);
	g_assert_cmpuint (e.name_len, ==, sizeof (wire) - 1);
	g_assert_cmpstr  (e.name,     ==, "Admin BadMoon Info");
}

static void
test_folderitem_category (void)
{
	/* Any ntype != 1 → category (the original gtkhx contract).
	 * mhxd's plain-form snd path doesn't really ship 0x0140 in
	 * the wild, but if it did, ntype 2 (used by some legacy
	 * implementations) would map to category here. */
	const guint8 wire[] = {
		0x02,                            /* ntype = 2 (category) */
		'G','u','e','s','t',' ','B','o','o','k'
	};
	struct hx_news_dirlist_entry e;
	g_assert_true    (hx_news_dirlist_parse_folderitem (wire, sizeof wire, &e));
	g_assert_cmpint  (e.kind,     ==, 2);
	g_assert_cmpuint (e.name_len, ==, 10);
	g_assert_cmpstr  (e.name,     ==, "Guest Book");
}

static void
test_folderitem_ntype_zero_is_category (void)
{
	/* Boundary on the kind decision: ntype 0 isn't 1, so it maps
	 * to category. Matches `dh->data[0]` semantics of the original
	 * parser exactly. */
	const guint8 wire[] = { 0x00, 'x' };
	struct hx_news_dirlist_entry e;
	g_assert_true   (hx_news_dirlist_parse_folderitem (wire, sizeof wire, &e));
	g_assert_cmpint (e.kind, ==, 2);
	g_assert_cmpstr (e.name, ==, "x");
}

static void
test_folderitem_only_ntype_byte (void)
{
	/* dlen == 1: just the ntype byte, no name. Parser returns
	 * TRUE with an empty name. */
	const guint8 wire[] = { 0x01 };
	struct hx_news_dirlist_entry e;
	g_assert_true    (hx_news_dirlist_parse_folderitem (wire, sizeof wire, &e));
	g_assert_cmpint  (e.kind,     ==, 1);
	g_assert_cmpuint (e.name_len, ==, 0);
	g_assert_cmpstr  (e.name,     ==, "");
}

static void
test_folderitem_truncates_oversized_name (void)
{
	/* Wire name longer than the output buffer (256 - 1 = 255).
	 * Parser caps the name at the buffer size and NUL-terminates.
	 * Hotline-spec names are bounded at 31 chars, so this is more
	 * a defensive bound than a realistic case. */
	guint8 wire[1 + 400];
	wire[0] = 0x01;
	memset (wire + 1, 'A', 400);
	struct hx_news_dirlist_entry e;
	g_assert_true    (hx_news_dirlist_parse_folderitem (wire, sizeof wire, &e));
	g_assert_cmpint  (e.kind,     ==, 1);
	g_assert_cmpuint (e.name_len, ==, 255);
	g_assert_cmpuint (strlen (e.name), ==, 255);
	for (int i = 0; i < 255; i++)
		g_assert_cmpint (e.name[i], ==, 'A');
}

static void
test_folderitem_rejects_empty (void)
{
	/* dlen == 0 means the chunk had no body — can't even read
	 * ntype. Refuse. */
	const guint8 *wire = NULL;     /* won't be dereferenced */
	struct hx_news_dirlist_entry e;
	g_assert_false (hx_news_dirlist_parse_folderitem (wire, 0, &e));
}

static void
test_folderitem_null_out_returns_false (void)
{
	const guint8 wire[] = { 0x01, 'x' };
	g_assert_false (hx_news_dirlist_parse_folderitem (wire, sizeof wire, NULL));
}

static void
test_folderitem_null_data_returns_false (void)
{
	struct hx_news_dirlist_entry e;
	g_assert_false (hx_news_dirlist_parse_folderitem (NULL, 0, &e));
	g_assert_false (hx_news_dirlist_parse_folderitem (NULL, 100, &e));
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);

	g_test_add_func ("/proto/news_dirlist/categoryitem_folder",
	                 test_categoryitem_folder);
	g_test_add_func ("/proto/news_dirlist/categoryitem_folder_with_trailing_padding",
	                 test_categoryitem_folder_with_trailing_padding);

	g_test_add_func ("/proto/news_dirlist/categoryitem_category",
	                 test_categoryitem_category);
	g_test_add_func ("/proto/news_dirlist/categoryitem_category_long_name",
	                 test_categoryitem_category_long_name);

	g_test_add_func ("/proto/news_dirlist/categoryitem_folder_empty_name",
	                 test_categoryitem_folder_empty_name);

	g_test_add_func ("/proto/news_dirlist/categoryitem_rejects_truncated_header",
	                 test_categoryitem_rejects_truncated_header);
	g_test_add_func ("/proto/news_dirlist/categoryitem_rejects_unknown_ntype",
	                 test_categoryitem_rejects_unknown_ntype);
	g_test_add_func ("/proto/news_dirlist/categoryitem_rejects_folder_truncated_before_namelen",
	                 test_categoryitem_rejects_folder_truncated_before_namelen);
	g_test_add_func ("/proto/news_dirlist/categoryitem_rejects_category_truncated_before_namelen",
	                 test_categoryitem_rejects_category_truncated_before_namelen);
	g_test_add_func ("/proto/news_dirlist/categoryitem_rejects_namelen_overrun",
	                 test_categoryitem_rejects_namelen_overrun);

	g_test_add_func ("/proto/news_dirlist/categoryitem_null_out_returns_false",
	                 test_categoryitem_null_out_returns_false);
	g_test_add_func ("/proto/news_dirlist/categoryitem_null_data_returns_false",
	                 test_categoryitem_null_data_returns_false);

	/* Plain form (HTLC_DATA_NEWSFOLDERITEM / 0x0140) */
	g_test_add_func ("/proto/news_dirlist/folderitem_folder",
	                 test_folderitem_folder);
	g_test_add_func ("/proto/news_dirlist/folderitem_category",
	                 test_folderitem_category);
	g_test_add_func ("/proto/news_dirlist/folderitem_ntype_zero_is_category",
	                 test_folderitem_ntype_zero_is_category);
	g_test_add_func ("/proto/news_dirlist/folderitem_only_ntype_byte",
	                 test_folderitem_only_ntype_byte);
	g_test_add_func ("/proto/news_dirlist/folderitem_truncates_oversized_name",
	                 test_folderitem_truncates_oversized_name);
	g_test_add_func ("/proto/news_dirlist/folderitem_rejects_empty",
	                 test_folderitem_rejects_empty);
	g_test_add_func ("/proto/news_dirlist/folderitem_null_out_returns_false",
	                 test_folderitem_null_out_returns_false);
	g_test_add_func ("/proto/news_dirlist/folderitem_null_data_returns_false",
	                 test_folderitem_null_data_returns_false);

	return g_test_run ();
}
