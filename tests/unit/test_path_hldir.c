/*
 * tests/unit/test_path_hldir.c — verify the Hotline DIR-chunk
 * encoder (path_to_hldir) plus the dirmask prefix-stripper. Both
 * live in src/path_hldir.c specifically so this test can link them
 * without dragging in files.c's GTK / Adwaita pile.
 *
 * Wire format pinned here:
 *
 *   u16 component_count (big-endian)
 *   repeated component_count times:
 *     u16 enc       (always zero in practice; the receiver ignores it)
 *     u8  namelen
 *     ... namelen bytes of name ...
 *
 * Every Hotline opcode that touches a file or folder (FILE_GET,
 * FILE_PUT, FILE_MKDIR, FILE_DELETE, FILE_GETINFO, FILE_GETFOLDER,
 * FILE_PUTFOLDER, NEWSDIRLIST, …) carries one of these for the
 * directory portion of the target path. Get the encoding wrong and
 * every one of those operations gets sent to nowhere.
 *
 * is_file = 1 trims one component off the end — used at call sites
 * that ship the filename separately in a FILE_NAME chunk (so a "/"
 * in the filename survives instead of being interpreted as a path
 * boundary). is_file = 0 keeps every component (used by directory-
 * level opcodes).
 */

#include "config.h"
#include <string.h>
#include <glib.h>
#include "path_hldir.h"

/* dir_char lives in files.c at runtime. The test owns its own copy
 * — path_hldir.c references it as `extern`, and the linker resolves
 * here. Default to '/' (matches the production default and what
 * mhxd / hlserver advertise). */
guint8 dir_char = '/';

/* Read a u16 from a big-endian wire offset. */
static guint16
beget16 (const guint8 *p)
{
    return ((guint16)p[0] << 8) | (guint16)p[1];
}

/* ---------- path_to_hldir ---------- */

/* Empty path with is_file=0 should encode zero components: just the
 * two-byte component-count header reading 0. */
static void
test_empty_path_is_file_false (void)
{
    guint16 len = 0;
    guint8 *hldir = path_to_hldir ("", &len, 0);

    g_assert_cmpint (len, ==, 2);
    g_assert_cmpint (beget16 (hldir), ==, 0);

    g_free (hldir);
}

/* Empty path with is_file=1 — same shape (zero components, two-byte
 * header). is_file only affects whether the trailing component is
 * appended, and there is no trailing component to append. */
static void
test_empty_path_is_file_true (void)
{
    guint16 len = 0;
    guint8 *hldir = path_to_hldir ("", &len, 1);

    g_assert_cmpint (len, ==, 2);
    g_assert_cmpint (beget16 (hldir), ==, 0);

    g_free (hldir);
}

/* Single-component path with is_file=0 — emits one component
 * record: 2 header bytes + 1 namelen + 5 name bytes = 8 total. */
static void
test_single_component (void)
{
    guint16 len = 0;
    guint8 *hldir = path_to_hldir ("files", &len, 0);

    /* 2 (count) + 2 (enc) + 1 (namelen) + 5 (name) */
    g_assert_cmpint (len, ==, 2 + 2 + 1 + 5);
    g_assert_cmpint (beget16 (hldir), ==, 1);
    g_assert_cmphex (hldir[2], ==, 0);
    g_assert_cmphex (hldir[3], ==, 0);
    g_assert_cmpint (hldir[4], ==, 5);
    g_assert_cmpint (memcmp (&hldir[5], "files", 5), ==, 0);

    g_free (hldir);
}

/* Single-component path with is_file=1 — emits zero components,
 * because is_file says "the last component is the filename, not a
 * directory entry, so don't put it in the DIR chunk". */
static void
test_single_component_is_file (void)
{
    guint16 len = 0;
    guint8 *hldir = path_to_hldir ("README.txt", &len, 1);

    g_assert_cmpint (len, ==, 2);
    g_assert_cmpint (beget16 (hldir), ==, 0);

    g_free (hldir);
}

/* Nested path with is_file=0. The whole path is directory-bearing,
 * so all three components emit. */
static void
test_nested_path (void)
{
    guint16 len = 0;
    guint8 *hldir = path_to_hldir ("files/Oni Tracks/loop", &len, 0);

    /* count + 3 * (enc + namelen + name) */
    /*   = 2 + (2+1+5) + (2+1+10) + (2+1+4) = 2 + 8 + 13 + 7 = 30 */
    g_assert_cmpint (len, ==, 30);
    g_assert_cmpint (beget16 (hldir), ==, 3);

    /* component 0: "files" */
    g_assert_cmphex (hldir[2], ==, 0);
    g_assert_cmphex (hldir[3], ==, 0);
    g_assert_cmpint (hldir[4], ==, 5);
    g_assert_cmpint (memcmp (&hldir[5], "files", 5), ==, 0);

    /* component 1: "Oni Tracks" */
    g_assert_cmphex (hldir[10], ==, 0);
    g_assert_cmphex (hldir[11], ==, 0);
    g_assert_cmpint (hldir[12], ==, 10);
    g_assert_cmpint (memcmp (&hldir[13], "Oni Tracks", 10), ==, 0);

    /* component 2: "loop" */
    g_assert_cmphex (hldir[23], ==, 0);
    g_assert_cmphex (hldir[24], ==, 0);
    g_assert_cmpint (hldir[25], ==, 4);
    g_assert_cmpint (memcmp (&hldir[26], "loop", 4), ==, 0);

    g_free (hldir);
}

/* Same nested path, is_file=1. Trailing component ("loop") is the
 * filename — skipped. Only the two parent dirs survive. */
static void
test_nested_path_is_file (void)
{
    guint16 len = 0;
    guint8 *hldir = path_to_hldir ("files/Oni Tracks/loop", &len, 1);

    g_assert_cmpint (len, ==, 2 + (2 + 1 + 5) + (2 + 1 + 10));
    g_assert_cmpint (beget16 (hldir), ==, 2);

    g_assert_cmpint (hldir[4], ==, 5);
    g_assert_cmpint (memcmp (&hldir[5], "files", 5), ==, 0);
    g_assert_cmpint (hldir[12], ==, 10);
    g_assert_cmpint (memcmp (&hldir[13], "Oni Tracks", 10), ==, 0);

    g_free (hldir);
}

/* Trailing separator: the implementation skips zero-length
 * components, so a trailing "/" doesn't emit an empty entry. */
static void
test_trailing_separator (void)
{
    guint16 len = 0;
    guint8 *hldir = path_to_hldir ("files/", &len, 0);

    /* Just one component, "files". */
    g_assert_cmpint (len, ==, 2 + 2 + 1 + 5);
    g_assert_cmpint (beget16 (hldir), ==, 1);
    g_assert_cmpint (hldir[4], ==, 5);
    g_assert_cmpint (memcmp (&hldir[5], "files", 5), ==, 0);

    g_free (hldir);
}

/* Repeated separators ("a//b") collapse to ("a", "b"). The
 * implementation walks past zero-length components inside the
 * while-loop. */
static void
test_repeated_separators (void)
{
    guint16 len = 0;
    guint8 *hldir = path_to_hldir ("a//b", &len, 0);

    g_assert_cmpint (beget16 (hldir), ==, 2);
    g_assert_cmpint (hldir[4], ==, 1);
    g_assert_cmphex (hldir[5], ==, 'a');
    g_assert_cmpint (hldir[8], ==, 1);
    g_assert_cmphex (hldir[9], ==, 'b');

    g_free (hldir);
}

/* When the server advertises a non-'/' dir separator (Mac-native
 * servers use ':'), path_to_hldir reads that from dir_char and
 * splits on it instead. Toggle dir_char around the call and verify
 * the components come out the way the server expects. */
static void
test_dir_char_colon (void)
{
    guint8 saved = dir_char;
    dir_char = ':';

    guint16 len = 0;
    guint8 *hldir = path_to_hldir ("files:Oni Tracks", &len, 0);

    g_assert_cmpint (beget16 (hldir), ==, 2);
    g_assert_cmpint (hldir[4], ==, 5);
    g_assert_cmpint (memcmp (&hldir[5], "files", 5), ==, 0);
    g_assert_cmpint (hldir[12], ==, 10);
    g_assert_cmpint (memcmp (&hldir[13], "Oni Tracks", 10), ==, 0);

    g_free (hldir);
    dir_char = saved;
}

/* When dir_char is ':', a literal '/' in a component is part of the
 * name. This is the whole reason the wire keeps the dir_char
 * negotiation: filenames with slashes (extremely common on Mac
 * servers) survive the round-trip because the separator can be
 * something else. */
static void
test_slash_in_name_when_dir_char_is_colon (void)
{
    guint8 saved = dir_char;
    dir_char = ':';

    guint16 len = 0;
    guint8 *hldir = path_to_hldir ("a/b:c", &len, 0);

    /* Two components: "a/b" and "c". */
    g_assert_cmpint (beget16 (hldir), ==, 2);
    g_assert_cmpint (hldir[4], ==, 3);
    g_assert_cmpint (memcmp (&hldir[5], "a/b", 3), ==, 0);
    g_assert_cmpint (hldir[10], ==, 1);
    g_assert_cmphex (hldir[11], ==, 'c');

    g_free (hldir);
    dir_char = saved;
}

/* High-byte non-ASCII component names (UTF-8 multi-byte sequences)
 * pass through verbatim — path_to_hldir treats names as opaque
 * byte runs. The receiver does the UTF-8 sanitisation. */
static void
test_high_byte_names (void)
{
    guint16 len = 0;
    /* "café" — UTF-8 c-a-f-0xc3-0xa9, length 5 bytes. */
    guint8 *hldir = path_to_hldir ("caf\xc3\xa9", &len, 0);

    g_assert_cmpint (beget16 (hldir), ==, 1);
    g_assert_cmpint (hldir[4], ==, 5);
    g_assert_cmphex (hldir[5], ==, 'c');
    g_assert_cmphex (hldir[6], ==, 'a');
    g_assert_cmphex (hldir[7], ==, 'f');
    g_assert_cmphex (hldir[8], ==, 0xc3);
    g_assert_cmphex (hldir[9], ==, 0xa9);

    g_free (hldir);
}

/* ---------- dirmask ---------- */

/* Common-prefix strip: src begins with mask, dst receives the tail. */
static void
test_dirmask_strips_prefix (void)
{
    char dst[64] = { 0 };
    char src[] = "/home/misha/Downloads/file.txt";
    char mask[] = "/home/misha/";

    dirmask (dst, src, mask);

    g_assert_cmpstr (dst, ==, "Downloads/file.txt");
}

/* If mask is exhausted first, the unmatched src tail copies. */
static void
test_dirmask_mask_shorter (void)
{
    char dst[32] = { 0 };
    char src[] = "abc/def";
    char mask[] = "abc/";

    dirmask (dst, src, mask);

    g_assert_cmpstr (dst, ==, "def");
}

/* If src ends before mask does, src has nothing to copy. */
static void
test_dirmask_src_shorter (void)
{
    char dst[32] = { 0 };
    char src[] = "abc";
    char mask[] = "abc/def";

    dirmask (dst, src, mask);

    g_assert_cmpstr (dst, ==, "");
}

/* Mismatch at the first byte: src copies verbatim (the inner loop
 * advances both pointers when mask[i] != src[i], so it actually
 * skips that one differing byte — pin that real behavior down). */
static void
test_dirmask_first_byte_mismatch (void)
{
    char dst[32] = { 0 };
    char src[] = "xfoo";
    char mask[] = "y";

    dirmask (dst, src, mask);

    /* Both pointers advance once on the mismatch, so the first byte
	 * gets eaten and dst receives the remainder. */
    g_assert_cmpstr (dst, ==, "foo");
}

/* Empty mask is a no-op — entire src copies through. */
static void
test_dirmask_empty_mask (void)
{
    char dst[32] = { 0 };
    char src[] = "anything";
    char mask[] = "";

    dirmask (dst, src, mask);

    g_assert_cmpstr (dst, ==, "anything");
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/path_hldir/empty_path_is_file_false",
                     test_empty_path_is_file_false);
    g_test_add_func ("/path_hldir/empty_path_is_file_true",
                     test_empty_path_is_file_true);
    g_test_add_func ("/path_hldir/single_component", test_single_component);
    g_test_add_func ("/path_hldir/single_component_is_file",
                     test_single_component_is_file);
    g_test_add_func ("/path_hldir/nested_path", test_nested_path);
    g_test_add_func ("/path_hldir/nested_path_is_file",
                     test_nested_path_is_file);
    g_test_add_func ("/path_hldir/trailing_separator",
                     test_trailing_separator);
    g_test_add_func ("/path_hldir/repeated_separators",
                     test_repeated_separators);
    g_test_add_func ("/path_hldir/dir_char_colon", test_dir_char_colon);
    g_test_add_func ("/path_hldir/slash_in_name_when_dir_char_is_colon",
                     test_slash_in_name_when_dir_char_is_colon);
    g_test_add_func ("/path_hldir/high_byte_names", test_high_byte_names);

    g_test_add_func ("/path_hldir/dirmask/strips_prefix",
                     test_dirmask_strips_prefix);
    g_test_add_func ("/path_hldir/dirmask/mask_shorter",
                     test_dirmask_mask_shorter);
    g_test_add_func ("/path_hldir/dirmask/src_shorter",
                     test_dirmask_src_shorter);
    g_test_add_func ("/path_hldir/dirmask/first_byte_mismatch",
                     test_dirmask_first_byte_mismatch);
    g_test_add_func ("/path_hldir/dirmask/empty_mask", test_dirmask_empty_mask);

    return g_test_run ();
}
