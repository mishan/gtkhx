/*
 * tests/proto/test_filelist_walker.c — pin the hl_filelist_hdr
 * chunk walker that the remote files browser uses to populate the
 * file list from an accumulated HTLS_DATA_FILE_LIST response.
 *
 * Wire layout per chunk:
 *
 *   u16 type      (always HTLS_DATA_FILE_LIST = 0xc8)
 *   u16 len       (chunk body size, big-endian)
 *   u32 ftype     (FourCC, e.g. "fldr" / "TEXT" / "JPEG")
 *   u32 fcreator  (FourCC)
 *   u32 fsize     (byte size; folder rows reuse this for item count)
 *   u32 unknown
 *   u32 fnlen     (filename byte length)
 *   u8  fname[fnlen]
 *
 * Total bytes for one chunk = 4 (type+len header) + 20 (5 u32) + fnlen
 *                           = SIZEOF_HL_DATA_HDR + 20 + fnlen.
 * `len` on the wire holds the 20 + fnlen body length.
 *
 * The walker is the load-bearing piece of every remote file listing
 * the user sees in the new browser. Get the per-entry stride wrong
 * and the listing truncates after one entry (the legacy bug that
 * shipped before the ntohs-on-fh->len fix) or walks off the end of
 * the receive buffer.
 */

#include "config.h"
#include <string.h>
#include <arpa/inet.h>
#include <glib.h>
#include "compat.h"             /* PACKED */
#include "hotline.h"
#include "filelist_walker.h"

/* GArray<struct entry> the callback appends to so the test can
 * assert against it after the walk finishes. */
struct entry {
    guint32 ftype;
    guint32 fsize;
    char name[256];
    gsize name_len;
};

static void
collect_cb (guint32 ftype, guint32 fsize, const guint8 *name, gsize name_len,
            void *user_data)
{
    GArray *out = (GArray *)user_data;
    struct entry e = { 0 };

    e.ftype = ftype;
    e.fsize = fsize;
    e.name_len = name_len < sizeof (e.name) ? name_len : sizeof (e.name) - 1;
    memcpy (e.name, name, e.name_len);
    e.name[e.name_len] = '\0';

    g_array_append_val (out, e);
}

/* Build one packed hl_filelist_hdr-shaped chunk into `out`,
 * returning the number of bytes appended. Caller passes ftype +
 * fsize in HOST order; this packs them big-endian on the wire. */
static gsize
pack_chunk (GByteArray *out, const char *ftype_fcc, guint32 fsize,
            const char *name)
{
    gsize fnlen = strlen (name);
    guint16 type_be = htons (HTLS_DATA_FILE_LIST);
    guint16 len_be = htons ((guint16) (20 + fnlen));
    guint32 ftype_be, fcreator_be, fsize_be, unknown_be, fnlen_be;

    g_assert_cmpint (strlen (ftype_fcc), ==, 4);

    memcpy (&ftype_be, ftype_fcc, 4);   /* already big-endian as ASCII */
    fcreator_be = htonl (0);
    fsize_be = htonl (fsize);
    unknown_be = htonl (0);
    fnlen_be = htonl ((guint32)fnlen);

    gsize start = out->len;
    g_byte_array_append (out, (guint8 *)&type_be, 2);
    g_byte_array_append (out, (guint8 *)&len_be, 2);
    g_byte_array_append (out, (guint8 *)&ftype_be, 4);
    g_byte_array_append (out, (guint8 *)&fcreator_be, 4);
    g_byte_array_append (out, (guint8 *)&fsize_be, 4);
    g_byte_array_append (out, (guint8 *)&unknown_be, 4);
    g_byte_array_append (out, (guint8 *)&fnlen_be, 4);
    g_byte_array_append (out, (guint8 *)name, fnlen);
    return out->len - start;
}

/* Empty buffer = no callbacks. */
static void
test_empty_buffer (void)
{
    GArray *out = g_array_new (FALSE, FALSE, sizeof (struct entry));
    hl_filelist_walk (NULL, 0, collect_cb, out);
    g_assert_cmpint (out->len, ==, 0);
    g_array_free (out, TRUE);
}

/* Single file entry. */
static void
test_single_file_entry (void)
{
    GByteArray *buf = g_byte_array_new ();
    pack_chunk (buf, "TEXT", 12345, "hello.txt");

    GArray *out = g_array_new (FALSE, FALSE, sizeof (struct entry));
    hl_filelist_walk (buf->data, buf->len, collect_cb, out);

    g_assert_cmpint (out->len, ==, 1);
    struct entry *e = &g_array_index (out, struct entry, 0);
    g_assert_cmphex (e->ftype, ==, 0x54455854u); /* 'TEXT' */
    g_assert_cmpuint (e->fsize, ==, 12345);
    g_assert_cmpstr (e->name, ==, "hello.txt");

    g_byte_array_free (buf, TRUE);
    g_array_free (out, TRUE);
}

/* Folder entry — ftype is 'fldr', fsize is the item count rather
 * than a byte count. The walker doesn't know or care about that
 * distinction; it returns the raw u32 either way. */
static void
test_folder_entry (void)
{
    GByteArray *buf = g_byte_array_new ();
    pack_chunk (buf, "fldr", 7, "Oni Tracks");

    GArray *out = g_array_new (FALSE, FALSE, sizeof (struct entry));
    hl_filelist_walk (buf->data, buf->len, collect_cb, out);

    g_assert_cmpint (out->len, ==, 1);
    struct entry *e = &g_array_index (out, struct entry, 0);
    g_assert_cmphex (e->ftype, ==, 0x666c6472u); /* 'fldr' */
    g_assert_cmpuint (e->fsize, ==, 7);
    g_assert_cmpstr (e->name, ==, "Oni Tracks");

    g_byte_array_free (buf, TRUE);
    g_array_free (out, TRUE);
}

/* Multiple entries packed back-to-back. The legacy walker bug
 * was that fh->len wasn't byteswapped before the stride advance,
 * so only the first entry came out. Pin all three. */
static void
test_three_entries_back_to_back (void)
{
    GByteArray *buf = g_byte_array_new ();
    pack_chunk (buf, "fldr", 5, "Folder");
    pack_chunk (buf, "TEXT", 1024, "README");
    pack_chunk (buf, "JPEG", 65536, "photo.jpg");

    GArray *out = g_array_new (FALSE, FALSE, sizeof (struct entry));
    hl_filelist_walk (buf->data, buf->len, collect_cb, out);

    g_assert_cmpint (out->len, ==, 3);
    g_assert_cmpstr (g_array_index (out, struct entry, 0).name, ==, "Folder");
    g_assert_cmpuint (g_array_index (out, struct entry, 0).fsize, ==, 5);
    g_assert_cmpstr (g_array_index (out, struct entry, 1).name, ==, "README");
    g_assert_cmpuint (g_array_index (out, struct entry, 1).fsize, ==, 1024);
    g_assert_cmpstr (g_array_index (out, struct entry, 2).name, ==, "photo.jpg");
    g_assert_cmpuint (g_array_index (out, struct entry, 2).fsize, ==, 65536);

    g_byte_array_free (buf, TRUE);
    g_array_free (out, TRUE);
}

/* High-byte filename bytes (UTF-8 multi-byte / Mac Roman) pass
 * through as opaque bytes — the walker doesn't sanitise. */
static void
test_high_byte_filename (void)
{
    GByteArray *buf = g_byte_array_new ();
    /* "café.txt" — UTF-8 c-a-f-0xc3-0xa9-.-t-x-t */
    pack_chunk (buf, "TEXT", 0, "caf\xc3\xa9.txt");

    GArray *out = g_array_new (FALSE, FALSE, sizeof (struct entry));
    hl_filelist_walk (buf->data, buf->len, collect_cb, out);

    g_assert_cmpint (out->len, ==, 1);
    struct entry *e = &g_array_index (out, struct entry, 0);
    g_assert_cmpint (e->name_len, ==, 9);
    g_assert_cmpmem (e->name, e->name_len, "caf\xc3\xa9.txt", 9);

    g_byte_array_free (buf, TRUE);
    g_array_free (out, TRUE);
}

/* Truncated buffer: the per-chunk len claims more bytes than `end`
 * provides. The walker must STOP rather than walk off the end —
 * this is the defense the new helper added that the legacy walker
 * lacked. */
static void
test_truncated_buffer_stops_safely (void)
{
    GByteArray *buf = g_byte_array_new ();
    pack_chunk (buf, "TEXT", 99, "ok.txt");
    pack_chunk (buf, "TEXT", 99, "incomplete.bin");
    /* Drop the last 5 bytes — the filename of the second entry
	 * is now incomplete, and the per-chunk len claims they're
	 * there. */
    g_byte_array_set_size (buf, buf->len - 5);

    GArray *out = g_array_new (FALSE, FALSE, sizeof (struct entry));
    hl_filelist_walk (buf->data, buf->len, collect_cb, out);

    /* First entry comes through fine; second entry is rejected
	 * because its declared body length doesn't fit in the
	 * remaining buffer. */
    g_assert_cmpint (out->len, ==, 1);
    g_assert_cmpstr (g_array_index (out, struct entry, 0).name, ==, "ok.txt");

    g_byte_array_free (buf, TRUE);
    g_array_free (out, TRUE);
}

/* NULL callback is tolerated — useful for "is this buffer
 * walkable at all" probes. The walker still iterates; it just
 * doesn't notify. */
static void
test_null_callback_is_noop (void)
{
    GByteArray *buf = g_byte_array_new ();
    pack_chunk (buf, "TEXT", 0, "anything");

    /* Just must not crash. */
    hl_filelist_walk (buf->data, buf->len, NULL, NULL);

    g_byte_array_free (buf, TRUE);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/filelist_walker/empty_buffer", test_empty_buffer);
    g_test_add_func ("/filelist_walker/single_file_entry",
                     test_single_file_entry);
    g_test_add_func ("/filelist_walker/folder_entry", test_folder_entry);
    g_test_add_func ("/filelist_walker/three_entries_back_to_back",
                     test_three_entries_back_to_back);
    g_test_add_func ("/filelist_walker/high_byte_filename",
                     test_high_byte_filename);
    g_test_add_func ("/filelist_walker/truncated_buffer_stops_safely",
                     test_truncated_buffer_stops_safely);
    g_test_add_func ("/filelist_walker/null_callback_is_noop",
                     test_null_callback_is_noop);

    return g_test_run ();
}
