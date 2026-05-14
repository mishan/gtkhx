/*
 * tests/unit/test_path_util.c — verify path_basename(path, sep)
 * matches the dirchar_basename behaviour the file-transfer code
 * depends on.
 *
 * Hotline paths use whatever separator dir_char happens to be —
 * historically ':' (the Mac OS Classic convention), occasionally
 * '/' on Unix-side servers. The remote path "Files:Photos:cat.jpg"
 * is "Files/Photos/cat.jpg" elsewhere; the basename is "cat.jpg"
 * either way.
 *
 * The function is one of the load-bearing pieces of xfers.c
 * (display name in the Tasks window), files.c (renames, moves),
 * and rcv.c (incoming file transfer setup). If it ever returns
 * the wrong substring, file transfers display garbled names.
 */

#include "config.h"
#include <string.h>
#include <glib.h>
#include "path_util.h"

/* ---------- Mac-style ':' separator (the canonical Hotline form) --- */

static void
test_basename_colon_simple (void)
{
    char path[] = "Files:Photos:cat.jpg";
    g_assert_cmpstr (path_basename (path, ':'), ==, "cat.jpg");
}

static void
test_basename_colon_one_segment (void)
{
    /* No separator → the whole string IS the basename. */
    char path[] = "cat.jpg";
    g_assert_true (path_basename (path, ':') == path);
}

static void
test_basename_colon_root_only (void)
{
    /* Trailing ':' means the basename is the empty string after
	 * the last separator. The function returns a pointer to
	 * path[len] which is the trailing NUL. */
    char path[] = "Files:";
    char *out = path_basename (path, ':');
    g_assert_nonnull (out);
    g_assert_cmpstr (out, ==, "");
    /* It's the byte right after the colon: */
    g_assert_true (out == path + 6);
}

/* ---------- Unix-style '/' separator ---------- */

static void
test_basename_slash_simple (void)
{
    char path[] = "/home/misha/photos/cat.jpg";
    g_assert_cmpstr (path_basename (path, '/'), ==, "cat.jpg");
}

static void
test_basename_slash_root_only (void)
{
    char path[] = "/";
    char *out = path_basename (path, '/');
    g_assert_cmpstr (out, ==, "");
    g_assert_true (out == path + 1);
}

/* ---------- Empty / pathological ---------- */

static void
test_basename_empty_string (void)
{
    /* strlen is 0; the while loop never runs; we return path
	 * unchanged. The result is an empty string at the same
	 * address. */
    char path[] = "";
    char *out = path_basename (path, '/');
    g_assert_true (out == path);
    g_assert_cmpstr (out, ==, "");
}

static void
test_basename_only_separators (void)
{
    /* "/////" — every byte is a separator. The function walks
	 * backwards finding the last one and returns one past it,
	 * which is the trailing NUL — empty basename. */
    char path[] = "/////";
    char *out = path_basename (path, '/');
    g_assert_cmpstr (out, ==, "");
    g_assert_true (out == path + 5);
}

static void
test_basename_separator_at_start (void)
{
    /* ":alpha" — separator first, then content. The basename is
	 * everything after the only separator. */
    char path[] = ":alpha";
    g_assert_cmpstr (path_basename (path, ':'), ==, "alpha");
}

/* ---------- The separator parameter actually matters ---------- */

static void
test_basename_wrong_separator_returns_full_path (void)
{
    /* If we ask for the basename with the wrong separator, we get
	 * the whole path back. Catches a regression where a caller
	 * forgets to pass dir_char. */
    char path[] = "Files:Photos:cat.jpg";
    g_assert_true (path_basename (path, '/') == path);
}

static void
test_basename_two_separator_chars_disagree (void)
{
    /* Mixed separators in the same path: we honour ONLY the one
	 * we were asked for. (Hotline servers don't actually mix
	 * separators in the same response, but the function should
	 * still behave deterministically.) */
    char path[] = "a:b/c:d/e";
    /* With ':' the last colon is at index 5; basename is "d/e". */
    g_assert_cmpstr (path_basename (path, ':'), ==, "d/e");
    /* With '/' the last slash is at index 7; basename is "e". */
    g_assert_cmpstr (path_basename (path, '/'), ==, "e");
}

/* ---------- The returned pointer is into the input buffer ---------- */

static void
test_basename_returned_pointer_is_into_input (void)
{
    char path[] = "Files:Photos:cat.jpg";
    char *out = path_basename (path, ':');
    /* Document the no-allocation contract: the returned pointer
	 * is somewhere inside `path`. */
    g_assert_true (out >= path && out <= path + strlen (path));
    /* Specifically: it's the byte after the last colon. */
    g_assert_true (out == path + 13);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/path_util/basename_colon_simple",
                     test_basename_colon_simple);
    g_test_add_func ("/path_util/basename_colon_one_segment",
                     test_basename_colon_one_segment);
    g_test_add_func ("/path_util/basename_colon_root_only",
                     test_basename_colon_root_only);

    g_test_add_func ("/path_util/basename_slash_simple",
                     test_basename_slash_simple);
    g_test_add_func ("/path_util/basename_slash_root_only",
                     test_basename_slash_root_only);

    g_test_add_func ("/path_util/basename_empty_string",
                     test_basename_empty_string);
    g_test_add_func ("/path_util/basename_only_separators",
                     test_basename_only_separators);
    g_test_add_func ("/path_util/basename_separator_at_start",
                     test_basename_separator_at_start);

    g_test_add_func ("/path_util/basename_wrong_separator_returns_full_path",
                     test_basename_wrong_separator_returns_full_path);
    g_test_add_func ("/path_util/basename_two_separator_chars_disagree",
                     test_basename_two_separator_chars_disagree);

    g_test_add_func ("/path_util/basename_returned_pointer_is_into_input",
                     test_basename_returned_pointer_is_into_input);

    return g_test_run ();
}
