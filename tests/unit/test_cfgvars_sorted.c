/*
 * test_cfgvars_sorted — pin the alphabetical sort of the cfgvars[]
 * table in src/options.c.
 *
 * Background: cfgvars[] is the master prefs table. cfgvar_for_name
 * uses bsearch() on it, which requires the entries to be sorted by
 * the on-disk key string. There's a runtime assertion in options.c
 * (cfgvars_assert_sorted) that errors at first launch on a bad
 * ordering, but a fresh contributor moving entries around can ship
 * a misorder, hit the runtime check, and only realise after a
 * launch attempt. This test catches the same class of bug at
 * `meson test` time so the regression never reaches main.
 *
 * Approach: pure source-text scan. We can't link options.c into a
 * unit-test target — it pulls in every GTK / libadwaita / libpanel
 * dependency in the tree — so we parse:
 *
 *   src/cfgkeys.h  to build  { CFG_<NAME>  →  "on-disk key" }
 *   src/options.c  to extract the ordered list of CFG_<NAME>
 *                  identifiers as they appear in cfgvars[]
 *
 * then assert that the resolved on-disk strings are in sorted
 * order. The source root is passed as argv[1] (meson.build wires
 * meson.project_source_root() through).
 */
#include "config.h"

#include <glib.h>
#include <string.h>

/* Set by main() from argv[1]. The test funcs need the same value
 * but g_test_add_func doesn't take a context argument; a file-local
 * static is the path of least resistance. */
static const char *source_root;

static char *
slurp (const char *path)
{
    char *contents = NULL;
    GError *err = NULL;
    if (!g_file_get_contents (path, &contents, NULL, &err)) {
        g_error ("can't read %s: %s", path, err ? err->message : "(unknown)");
    }
    return contents;
}

/* Parse `src/cfgkeys.h` for `#define CFG_FOO "ONDISK"` lines.
 * Returns a hash table mapping CFG_FOO → ONDISK (both g_strdup'd).
 * Tolerant of whitespace and trailing comments; ignores #define
 * lines whose value isn't a quoted string (CFG_THEME_SYSTEM = "system"
 * etc are picked up too — that's fine, they just won't be queried). */
static GHashTable *
parse_cfgkeys (const char *cfgkeys_path)
{
    char *contents = slurp (cfgkeys_path);
    GHashTable *t = g_hash_table_new_full (g_str_hash, g_str_equal,
                                           g_free, g_free);
    /* (multiline) ^#define\s+(CFG_[A-Z0-9_]+)\s+"([^"]*)" */
    GRegex *re = g_regex_new ("^#define\\s+(CFG_[A-Z0-9_]+)\\s+\"([^\"]*)\"",
                              G_REGEX_MULTILINE, 0, NULL);
    GMatchInfo *mi = NULL;
    g_regex_match (re, contents, 0, &mi);
    while (g_match_info_matches (mi)) {
        gchar *macro = g_match_info_fetch (mi, 1);
        gchar *value = g_match_info_fetch (mi, 2);
        g_hash_table_replace (t, macro, value);
        g_match_info_next (mi, NULL);
    }
    g_match_info_free (mi);
    g_regex_unref (re);
    g_free (contents);
    return t;
}

/* Extract the cfgvars[] block from `src/options.c` and return the
 * ordered list of CFG_<NAME> identifiers that appear as the first
 * field of each entry. The table is declared as
 *
 *   struct cfgvar { ...fields... } cfgvars[] = {
 *     { CFG_<NAME>, ... },
 *     ...
 *   };
 *
 * so the opening anchor is `} cfgvars[] = {` (the *closing* brace of
 * the struct definition immediately preceding the array name), and
 * the closing anchor is `\n};`. Within the block every entry begins
 * `{ CFG_<NAME>, ...`. */
static GPtrArray *
parse_cfgvars_order (const char *options_path)
{
    char *contents = slurp (options_path);
    GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
    GError *err = NULL;

    /* Find the block. .*? in DOTALL mode is lazy across newlines so
	 * we don't accidentally swallow a later struct declaration. */
    GRegex *block_re = g_regex_new (
        "\\}\\s*cfgvars\\s*\\[\\s*\\]\\s*=\\s*\\{(.*?)\\n\\};",
        G_REGEX_DOTALL, 0, &err);
    g_assert_no_error (err);

    GMatchInfo *bmi = NULL;
    if (!g_regex_match (block_re, contents, 0, &bmi)) {
        g_match_info_free (bmi);
        g_regex_unref (block_re);
        g_error ("cfgvars[] block not found in %s — has the table moved?",
                 options_path);
    }
    gchar *block = g_match_info_fetch (bmi, 1);
    g_match_info_free (bmi);
    g_regex_unref (block_re);

    /* Match each row's leading `{ CFG_<NAME>,`. The cfgvars table
	 * entries are uniform in shape — every row begins this way —
	 * so a simple regex suffices without needing a full C parser. */
    GRegex *row_re = g_regex_new ("\\{\\s*(CFG_[A-Z0-9_]+)\\s*,",
                                  0, 0, NULL);
    GMatchInfo *rmi = NULL;
    g_regex_match (row_re, block, 0, &rmi);
    while (g_match_info_matches (rmi)) {
        g_ptr_array_add (names, g_match_info_fetch (rmi, 1));
        g_match_info_next (rmi, NULL);
    }
    g_match_info_free (rmi);
    g_regex_unref (row_re);

    g_free (block);
    g_free (contents);
    return names;
}

/* The headline test: every consecutive pair in cfgvars[] must be in
 * strictly-increasing alphabetical order by the on-disk key string.
 * This is what bsearch() requires and what the in-source runtime
 * assertion (cfgvars_assert_sorted) enforces at launch. */
static void
test_cfgvars_alphabetical (void)
{
    char *cfgkeys_path
        = g_build_filename (source_root, "src", "cfgkeys.h", NULL);
    char *options_path
        = g_build_filename (source_root, "src", "options.c", NULL);

    GHashTable *keys = parse_cfgkeys (cfgkeys_path);
    GPtrArray *order = parse_cfgvars_order (options_path);

    /* Sanity: we found *something*. If parsing failed silently the
	 * test could otherwise pass with zero comparisons. */
    g_assert_cmpint (order->len, >=, 10);

    /* Walk the table in order. Report the first violation with
	 * enough context (both macro name and on-disk string) that
	 * fixing it is mechanical. */
    for (guint i = 1; i < order->len; i++) {
        const char *prev_macro = g_ptr_array_index (order, i - 1);
        const char *curr_macro = g_ptr_array_index (order, i);
        const char *prev_str = g_hash_table_lookup (keys, prev_macro);
        const char *curr_str = g_hash_table_lookup (keys, curr_macro);

        if (!prev_str) {
            g_error ("cfgvars[] references %s but cfgkeys.h has no "
                     "matching #define",
                     prev_macro);
        }
        if (!curr_str) {
            g_error ("cfgvars[] references %s but cfgkeys.h has no "
                     "matching #define",
                     curr_macro);
        }
        if (strcmp (prev_str, curr_str) >= 0) {
            g_error (
                "cfgvars[] is not sorted: \"%s\" (%s) must come before "
                "\"%s\" (%s). bsearch() in cfgvar_for_name relies on "
                "the table being alphabetical by on-disk key.",
                curr_str, curr_macro, prev_str, prev_macro);
        }
    }

    g_hash_table_unref (keys);
    g_ptr_array_unref (order);
    g_free (cfgkeys_path);
    g_free (options_path);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    /* Meson wires meson.project_source_root() through as argv[1]
	 * (see tests/meson.build). Running the binary by hand:
	 *
	 *   build/tests/test_cfgvars_sorted /path/to/gtkhx
	 */
    if (argc < 2) {
        g_printerr (
            "usage: %s <gtkhx-source-root>\n"
            "  (when running under meson, the source root is wired\n"
            "  in via tests/meson.build args:)\n",
            argv[0]);
        return 2;
    }
    source_root = argv[1];

    g_test_add_func ("/cfgvars/alphabetical", test_cfgvars_alphabetical);
    return g_test_run ();
}
