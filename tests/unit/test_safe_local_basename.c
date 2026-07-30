/*
 * Tier 1 unit test for hx_files_provider_safe_local_basename.
 *
 * Sanitizes a remote-supplied file name into a value safe to join
 * onto the user's download directory. Tests the path-traversal
 * defense paths and ordinary-name pass-through.
 */

#include <glib.h>
#include <string.h>

#include "files_provider.h"

static void
check (const char *in, const char *expected)
{
    char *got = hx_files_provider_safe_local_basename (in);
    g_assert_cmpstr (got, ==, expected);
    g_free (got);
}

static void
test_passthrough (void)
{
    check ("readme.txt", "readme.txt");
    check ("Photo 2026-05-25.jpg", "Photo 2026-05-25.jpg");
    /* Dots are fine when not the entire name — only "." / ".." map
     * to filesystem traversal targets. */
    check ("..readme..", "..readme..");
    check (".hidden", ".hidden");
}

static void
test_path_separators_replaced (void)
{
    /* '/' is legal in Hotline names (Classic-Mac convention) but
     * unsafe in a local path component; replace with '_'. */
    check ("foo/bar", "foo_bar");
    check ("foo\\bar", "foo_bar");
    /* Multiple separators each get replaced independently. */
    check ("../etc/passwd", ".._etc_passwd");
    check ("..\\..\\Windows\\System32", ".._.._Windows_System32");
    /* Mix is handled. */
    check ("a/b\\c/d", "a_b_c_d");
}

static void
test_pure_dot_names (void)
{
    /* Pure "." and ".." would resolve to the parent/current dir
     * at the filesystem level — replace with a safe placeholder. */
    check (".", "download");
    check ("..", "download");
}

static void
test_empty_and_null (void)
{
    check (NULL, "download");
    check ("", "download");
}

static void
test_traversal_attack_payloads (void)
{
    /* The defensive question to ask: after sanitization, does
     * g_build_filename ("/home/user/Downloads", out) stay inside
     * "/home/user/Downloads"? Sanitization passes iff `out` has
     * no path separators AND isn't pure-dot. Spot-check a few
     * classic traversal payloads. */
    char *out;

    out = hx_files_provider_safe_local_basename ("../../../etc/passwd");
    g_assert_null (strchr (out, '/'));
    g_assert_null (strchr (out, '\\'));
    g_assert_cmpstr (out, !=, ".");
    g_assert_cmpstr (out, !=, "..");
    g_free (out);

    out = hx_files_provider_safe_local_basename ("..\\..\\..\\boot.ini");
    g_assert_null (strchr (out, '/'));
    g_assert_null (strchr (out, '\\'));
    g_free (out);

    out = hx_files_provider_safe_local_basename ("/etc/shadow");
    g_assert_null (strchr (out, '/'));
    g_free (out);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/files/safe_local_basename/passthrough",
                     test_passthrough);
    g_test_add_func ("/files/safe_local_basename/path_separators_replaced",
                     test_path_separators_replaced);
    g_test_add_func ("/files/safe_local_basename/pure_dot_names",
                     test_pure_dot_names);
    g_test_add_func ("/files/safe_local_basename/empty_and_null",
                     test_empty_and_null);
    g_test_add_func ("/files/safe_local_basename/traversal_attack_payloads",
                     test_traversal_attack_payloads);
    return g_test_run ();
}
