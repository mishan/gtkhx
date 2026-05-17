/*
 * tests/unit/test_uniquify_path.c — drive uniquify_path with a
 * synthetic "which paths exist" predicate so we can pin the
 * collision-resolution shapes without touching the filesystem.
 *
 * Lives in src/uniquify_path.c (extracted from xfers.c with the
 * stat()-based local_path_exists check replaced by a callback).
 * The production caller in xfers.c plugs in a wrapper that
 * checks stat() + Apple Double resource fork. Here we plug in a
 * GHashTable<path-string, dummy> lookup so the test stays a single
 * self-contained translation unit pair.
 *
 * Behavior shape (per the doc comment on uniquify_path):
 *
 *   /dl/foo.txt        + foo.txt present     →  /dl/foo (1).txt
 *   /dl/archive.tar.gz + that present        →  /dl/archive.tar (1).gz
 *   /dl/README         + that present        →  /dl/README (1)
 *   /dl/.bashrc        + that present        →  /dl/.bashrc (1)
 *   /dl/foo.txt        + foo.txt + foo (1).txt present
 *                                            →  /dl/foo (2).txt
 *   /dl/free.txt       + nothing present     →  /dl/free.txt  (unchanged)
 */

#include "config.h"
#include <string.h>
#include <glib.h>
#include <sys/param.h>          /* MAXPATHLEN */
#include "uniquify_path.h"

/* Predicate that consults a GHashTable<gchar*, gpointer> of seeded
 * "existing" paths. The hash table's keys are owned by the caller. */
static int
hash_table_exists (const char *path, void *user_data)
{
    GHashTable *taken = (GHashTable *)user_data;
    return g_hash_table_contains (taken, path);
}

/* Build a GHashTable of the given strings; caller frees with
 * g_hash_table_destroy. Keys point at the literal strings in the
 * test's argv, so they outlive the table. */
static GHashTable *
seed_taken (const char *const *paths)
{
    GHashTable *t = g_hash_table_new (g_str_hash, g_str_equal);
    for (const char *const *p = paths; *p; p++) {
        g_hash_table_add (t, (gpointer)*p);
    }
    return t;
}

/* Helper: run uniquify_path with the seeded table and return the
 * mutated path as a new string the caller frees. */
static char *
run_uniquify (const char *input, const char *const *taken_paths)
{
    char path[MAXPATHLEN];
    g_strlcpy (path, input, sizeof path);

    GHashTable *taken = seed_taken (taken_paths);
    uniquify_path (path, sizeof path, hash_table_exists, taken);
    g_hash_table_destroy (taken);

    return g_strdup (path);
}

/* If nothing collides, path is untouched. */
static void
test_no_collision_leaves_path_unchanged (void)
{
    const char *taken[] = { NULL };
    char *result = run_uniquify ("/dl/foo.txt", taken);
    g_assert_cmpstr (result, ==, "/dl/foo.txt");
    g_free (result);
}

/* Single collision: foo.txt → foo (1).txt. */
static void
test_simple_extension_insert (void)
{
    const char *taken[] = { "/dl/foo.txt", NULL };
    char *result = run_uniquify ("/dl/foo.txt", taken);
    g_assert_cmpstr (result, ==, "/dl/foo (1).txt");
    g_free (result);
}

/* The " (N)" goes before the LAST dot. archive.tar.gz keeps .gz as
 * the extension and inserts before it. */
static void
test_extension_is_last_dot (void)
{
    const char *taken[] = { "/dl/archive.tar.gz", NULL };
    char *result = run_uniquify ("/dl/archive.tar.gz", taken);
    g_assert_cmpstr (result, ==, "/dl/archive.tar (1).gz");
    g_free (result);
}

/* No dot in the basename: " (N)" appends to the end. */
static void
test_no_extension_appends (void)
{
    const char *taken[] = { "/dl/README", NULL };
    char *result = run_uniquify ("/dl/README", taken);
    g_assert_cmpstr (result, ==, "/dl/README (1)");
    g_free (result);
}

/* Leading-dot basename (".bashrc"): the dot is part of the name,
 * not an extension. The " (N)" appends instead of inserting. */
static void
test_leading_dot_basename (void)
{
    const char *taken[] = { "/dl/.bashrc", NULL };
    char *result = run_uniquify ("/dl/.bashrc", taken);
    g_assert_cmpstr (result, ==, "/dl/.bashrc (1)");
    g_free (result);
}

/* Counter advances until it finds a free slot. foo.txt + foo (1).txt
 * present → result is foo (2).txt. */
static void
test_counter_advances_past_taken_variants (void)
{
    const char *taken[]
        = { "/dl/foo.txt", "/dl/foo (1).txt", NULL };
    char *result = run_uniquify ("/dl/foo.txt", taken);
    g_assert_cmpstr (result, ==, "/dl/foo (2).txt");
    g_free (result);
}

/* Three consecutive collisions still resolve cleanly. */
static void
test_counter_advances_three_slots (void)
{
    const char *taken[] = { "/dl/foo.txt",      "/dl/foo (1).txt",
                            "/dl/foo (2).txt",  "/dl/foo (3).txt",
                            NULL };
    char *result = run_uniquify ("/dl/foo.txt", taken);
    g_assert_cmpstr (result, ==, "/dl/foo (4).txt");
    g_free (result);
}

/* Bare-filename path (no leading directory). */
static void
test_no_directory_in_path (void)
{
    const char *taken[] = { "report.pdf", NULL };
    char *result = run_uniquify ("report.pdf", taken);
    g_assert_cmpstr (result, ==, "report (1).pdf");
    g_free (result);
}

/* Path with a dot in a directory name (not the basename) doesn't
 * confuse the extension finder. The function uses strrchr on the
 * basename, not the whole path. */
static void
test_dot_in_directory_name_ignored (void)
{
    const char *taken[] = { "/path.with.dots/foo", NULL };
    char *result = run_uniquify ("/path.with.dots/foo", taken);
    g_assert_cmpstr (result, ==, "/path.with.dots/foo (1)");
    g_free (result);
}

/* Path with a dot in a directory AND an extension on the file —
 * extension correctly identified as the .ext at the end. */
static void
test_dot_in_directory_with_extension (void)
{
    const char *taken[] = { "/path.v2/foo.txt", NULL };
    char *result = run_uniquify ("/path.v2/foo.txt", taken);
    g_assert_cmpstr (result, ==, "/path.v2/foo (1).txt");
    g_free (result);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/uniquify_path/no_collision_leaves_path_unchanged",
                     test_no_collision_leaves_path_unchanged);
    g_test_add_func ("/uniquify_path/simple_extension_insert",
                     test_simple_extension_insert);
    g_test_add_func ("/uniquify_path/extension_is_last_dot",
                     test_extension_is_last_dot);
    g_test_add_func ("/uniquify_path/no_extension_appends",
                     test_no_extension_appends);
    g_test_add_func ("/uniquify_path/leading_dot_basename",
                     test_leading_dot_basename);
    g_test_add_func ("/uniquify_path/counter_advances_past_taken_variants",
                     test_counter_advances_past_taken_variants);
    g_test_add_func ("/uniquify_path/counter_advances_three_slots",
                     test_counter_advances_three_slots);
    g_test_add_func ("/uniquify_path/no_directory_in_path",
                     test_no_directory_in_path);
    g_test_add_func ("/uniquify_path/dot_in_directory_name_ignored",
                     test_dot_in_directory_name_ignored);
    g_test_add_func ("/uniquify_path/dot_in_directory_with_extension",
                     test_dot_in_directory_with_extension);

    return g_test_run ();
}
