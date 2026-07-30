/*
 * test_theme_listing — coverage for the theme-discovery API
 * (gtkhx_theme_list_available_at). Drives both the user-dir walk
 * (filesystem fixture, tmp-dir-per-test) and the GResource walk
 * (the live built-ins are linked into the test target via
 * gtkhx_resources in tests/meson.build). Together that exercises:
 *
 *   - empty user dir + GResource registered → only built-ins surface
 *   - empty user dir + NULL resource prefix → synthetic "default"
 *     fallback fires
 *   - user files parse [gtkhx-theme] name and use it as display
 *   - missing name key falls back to the basename
 *   - non-.ini files are skipped
 *   - sort: "default" first, then alphabetical by display name
 *   - user dir shadows same-name GResource entries (drop a
 *     "solarized.ini" override in $CONFIG and it replaces the
 *     built-in)
 *
 * See docs/theming-file-format.md and gtkhx_theme.h.
 */
#include "config.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "gtkhx_theme.h"
#include "hx.h"

/* Same shape as test_theme_scale: gtkhx_theme.c needs a
 * gtkhx_prefs symbol + gtkhx_config_dir() to link. The
 * gtkhx_theme_list_available_at variant doesn't call either, but
 * the gtkhx_theme_list_available no-args variant does (we don't
 * exercise it in this test, since it'd reach into the user's real
 * $XDG_CONFIG_HOME). */
struct gtkhx_prefs gtkhx_prefs;
extern const char *gtkhx_config_dir (void);
const char *
gtkhx_config_dir (void)
{
    return "/tmp/gtkhx-test-not-used";
}

/* ---- Test helpers ---------------------------------------------------- */

static char *
make_tmp_themes_dir (void)
{
    GError *err = NULL;
    char *dir = g_dir_make_tmp ("gtkhx-themes-XXXXXX", &err);
    g_assert_no_error (err);
    g_assert_nonnull (dir);
    return dir;
}

static void
write_theme_file (const char *dir, const char *basename, const char *contents)
{
    char *path = g_build_filename (dir, basename, NULL);
    GError *err = NULL;
    g_file_set_contents (path, contents, -1, &err);
    g_assert_no_error (err);
    g_free (path);
}

static void
rmrf_dir (const char *dir)
{
    GDir *d = g_dir_open (dir, 0, NULL);
    if (d) {
        const char *fn;
        while ((fn = g_dir_read_name (d)) != NULL) {
            char *path = g_build_filename (dir, fn, NULL);
            g_unlink (path);
            g_free (path);
        }
        g_dir_close (d);
    }
    g_rmdir (dir);
}

/* Locate an entry by its name (the THEMENAME pref value) in a
 * list-available result. Returns NULL if not present. */
static GtkhxThemeEntry *
entry_named (GPtrArray *list, const char *name)
{
    for (guint i = 0; i < list->len; i++) {
        GtkhxThemeEntry *e = g_ptr_array_index (list, i);
        if (g_strcmp0 (e->name, name) == 0) {
            return e;
        }
    }
    return NULL;
}

/* ---- Tests ----------------------------------------------------------- */

/* With no GResource source and an empty user dir, the synthetic
 * "default" fallback fires so the picker is never empty. */
static void
test_empty_yields_default_fallback (void)
{
    char *dir = make_tmp_themes_dir ();

    GPtrArray *themes = gtkhx_theme_list_available_at (NULL, dir);
    g_assert_cmpint (themes->len, ==, 1);
    GtkhxThemeEntry *e = g_ptr_array_index (themes, 0);
    g_assert_cmpstr (e->name, ==, "default");

    g_ptr_array_unref (themes);
    rmrf_dir (dir);
    g_free (dir);
}

/* A user theme file with a [gtkhx-theme] name = ... key uses that
 * string as its display name. */
static void
test_reads_display_name_from_keyfile (void)
{
    char *dir = make_tmp_themes_dir ();
    write_theme_file (dir, "fluffy.ini",
                      "[gtkhx-theme]\n"
                      "name = Fluffy Pony\n");

    GPtrArray *themes = gtkhx_theme_list_available_at (NULL, dir);
    /* user-fluffy + synthetic default fallback */
    g_assert_cmpint (themes->len, ==, 2);

    GtkhxThemeEntry *e = entry_named (themes, "fluffy");
    g_assert_nonnull (e);
    g_assert_cmpstr (e->display, ==, "Fluffy Pony");

    g_ptr_array_unref (themes);
    rmrf_dir (dir);
    g_free (dir);
}

/* Missing name key → display falls back to the file basename
 * (without the .ini suffix). */
static void
test_falls_back_to_basename_when_no_name_key (void)
{
    char *dir = make_tmp_themes_dir ();
    write_theme_file (dir, "Noname.ini", "[gtkhx-theme]\n");

    GPtrArray *themes = gtkhx_theme_list_available_at (NULL, dir);
    GtkhxThemeEntry *e = entry_named (themes, "Noname");
    g_assert_nonnull (e);
    g_assert_cmpstr (e->display, ==, "Noname");

    g_ptr_array_unref (themes);
    rmrf_dir (dir);
    g_free (dir);
}

/* Sort: "default" pinned to the front, everything else alphabetical
 * by display name. */
static void
test_sort_default_first_then_alphabetical (void)
{
    char *dir = make_tmp_themes_dir ();
    write_theme_file (dir, "zeta.ini", "[gtkhx-theme]\nname = Zeta\n");
    write_theme_file (dir, "alpha.ini", "[gtkhx-theme]\nname = Alpha\n");

    GPtrArray *themes = gtkhx_theme_list_available_at (NULL, dir);
    g_assert_cmpint (themes->len, ==, 3);

    GtkhxThemeEntry *e0 = g_ptr_array_index (themes, 0);
    GtkhxThemeEntry *e1 = g_ptr_array_index (themes, 1);
    GtkhxThemeEntry *e2 = g_ptr_array_index (themes, 2);
    g_assert_cmpstr (e0->name, ==, "default");
    g_assert_cmpstr (e1->display, ==, "Alpha");
    g_assert_cmpstr (e2->display, ==, "Zeta");

    g_ptr_array_unref (themes);
    rmrf_dir (dir);
    g_free (dir);
}

/* Non-.ini files (READMEs, editor backups, etc.) are skipped. The
 * test deliberately includes a "default.ini.bak" — a .bak that
 * starts with a real theme name should NOT slip in just because it
 * happens to look like one. */
static void
test_skips_non_ini_files (void)
{
    char *dir = make_tmp_themes_dir ();
    write_theme_file (dir, "README.txt", "not a theme\n");
    write_theme_file (dir, "default.ini.bak",
                      "[gtkhx-theme]\nname = Should Not Appear\n");

    GPtrArray *themes = gtkhx_theme_list_available_at (NULL, dir);
    /* Just the synthetic default. */
    g_assert_cmpint (themes->len, ==, 1);
    g_assert_null (entry_named (themes, "README"));
    g_assert_null (entry_named (themes, "default.ini"));

    g_ptr_array_unref (themes);
    rmrf_dir (dir);
    g_free (dir);
}

/* Theme names with embedded path separators are unsafe — the
 * loader rejects them at active_theme_path / safe_active_theme_name,
 * so surfacing them in the picker would create unselectable
 * entries that silently fall back to "default". Discovery
 * rejects them too. (POSIX disallows '/' inside a filename, so
 * only the backslash case is testable via a real on-disk
 * fixture; the slash case is dead code on Linux but the
 * predicate guards both for portability and for the GResource
 * walk where the rule still applies.) */
static void
test_skips_unsafe_theme_names (void)
{
    char *dir = make_tmp_themes_dir ();
    /* Flat-form file with a backslash in the basename — would
     * surface as theme name "foo\\bar" without the filter, which
     * the loader then rejects. */
    write_theme_file (dir, "foo\\bar.ini", "[gtkhx-theme]\nname = Phantom\n");

    GPtrArray *themes = gtkhx_theme_list_available_at (NULL, dir);
    /* Synthetic default only — phantom should be filtered. */
    g_assert_cmpint (themes->len, ==, 1);
    g_assert_null (entry_named (themes, "foo\\bar"));

    g_ptr_array_unref (themes);
    rmrf_dir (dir);
    g_free (dir);
}

/* The built-in themes (default, solarized) ride on the linked-in
 * GResource and surface through the resource_prefix arg. With an
 * empty user dir, both should appear in the sorted output.
 * Solarized's light + dark variants live in *one* file — the
 * active variant follows the system color scheme — so there's no
 * separate solarized-dark entry. */
static void
test_built_in_themes_from_resource (void)
{
    char *dir = make_tmp_themes_dir ();

    GPtrArray *themes
        = gtkhx_theme_list_available_at ("/com/nasledov/gtkhx/themes/", dir);
    /* Exactly the two built-ins we ship. If a third ever lands,
     * adjust the count — better to fail loudly than have the test
     * silently drift. */
    g_assert_cmpint (themes->len, ==, 2);

    g_assert_nonnull (entry_named (themes, "default"));
    GtkhxThemeEntry *s = entry_named (themes, "solarized");
    g_assert_nonnull (s);
    g_assert_cmpstr (s->display, ==, "Solarized");

    /* default pinned first. */
    GtkhxThemeEntry *e0 = g_ptr_array_index (themes, 0);
    g_assert_cmpstr (e0->name, ==, "default");

    g_ptr_array_unref (themes);
    rmrf_dir (dir);
    g_free (dir);
}

/* A user file with the same basename as a built-in shadows the
 * GResource entry — the user's "solarized.ini" replaces the
 * shipped one. Verified by reading back a display name only the
 * user copy carries. */
static void
test_user_file_shadows_built_in (void)
{
    char *dir = make_tmp_themes_dir ();
    write_theme_file (dir, "solarized.ini",
                      "[gtkhx-theme]\n"
                      "name = User Solarized Override\n");

    GPtrArray *themes
        = gtkhx_theme_list_available_at ("/com/nasledov/gtkhx/themes/", dir);
    /* Still two entries (the user's solarized replaced the
     * built-in one — no duplication). */
    g_assert_cmpint (themes->len, ==, 2);

    GtkhxThemeEntry *s = entry_named (themes, "solarized");
    g_assert_nonnull (s);
    g_assert_cmpstr (s->display, ==, "User Solarized Override");

    /* The built-in default is unaffected. */
    g_assert_nonnull (entry_named (themes, "default"));

    g_ptr_array_unref (themes);
    rmrf_dir (dir);
    g_free (dir);
}

/* gtkhx_theme_list_available_at(NULL, NULL) is the degenerate
 * everything-disabled case. The synthetic default fallback should
 * still fire so callers never get an empty picker. */
static void
test_null_sources_yields_default_only (void)
{
    GPtrArray *themes = gtkhx_theme_list_available_at (NULL, NULL);
    g_assert_cmpint (themes->len, ==, 1);
    GtkhxThemeEntry *e = g_ptr_array_index (themes, 0);
    g_assert_cmpstr (e->name, ==, "default");
    g_ptr_array_unref (themes);
}

/* When a flat <name>.ini and a dir-form <name>/theme.ini both exist
 * under the user dir, the listing must surface "name" exactly once and
 * prefer the dir-form bundle (its display name wins). */
static void
test_dir_form_preferred_over_flat (void)
{
    char *dir = make_tmp_themes_dir ();
    char *bundle = g_build_filename (dir, "foo", NULL);
    char *manifest;
    guint count = 0;
    GtkhxThemeEntry *e;
    GPtrArray *themes;

    /* Flat form. */
    write_theme_file (dir, "foo.ini", "[gtkhx-theme]\nname = FlatFoo\n");
    /* Dir form, same basename. */
    g_assert_cmpint (g_mkdir (bundle, 0700), ==, 0);
    write_theme_file (bundle, "theme.ini", "[gtkhx-theme]\nname = BundleFoo\n");

    themes = gtkhx_theme_list_available_at (NULL, dir);

    for (guint i = 0; i < themes->len; i++) {
        GtkhxThemeEntry *it = g_ptr_array_index (themes, i);
        if (g_strcmp0 (it->name, "foo") == 0) {
            count++;
        }
    }
    g_assert_cmpint (count, ==, 1); /* no duplicate */
    e = entry_named (themes, "foo");
    g_assert_nonnull (e);
    g_assert_cmpstr (e->display, ==, "BundleFoo"); /* dir-form won */

    g_ptr_array_unref (themes);
    manifest = g_build_filename (bundle, "theme.ini", NULL);
    g_unlink (manifest);
    g_rmdir (bundle);
    g_free (manifest);
    g_free (bundle);
    rmrf_dir (dir);
    g_free (dir);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/theme-listing/empty-yields-default-fallback",
                     test_empty_yields_default_fallback);
    g_test_add_func ("/theme-listing/reads-display-name-from-keyfile",
                     test_reads_display_name_from_keyfile);
    g_test_add_func ("/theme-listing/falls-back-to-basename",
                     test_falls_back_to_basename_when_no_name_key);
    g_test_add_func ("/theme-listing/sort-default-first-then-alphabetical",
                     test_sort_default_first_then_alphabetical);
    g_test_add_func ("/theme-listing/skips-non-ini-files",
                     test_skips_non_ini_files);
    g_test_add_func ("/theme-listing/skips-unsafe-theme-names",
                     test_skips_unsafe_theme_names);
    g_test_add_func ("/theme-listing/built-in-themes-from-resource",
                     test_built_in_themes_from_resource);
    g_test_add_func ("/theme-listing/user-file-shadows-built-in",
                     test_user_file_shadows_built_in);
    g_test_add_func ("/theme-listing/null-sources-yields-default-only",
                     test_null_sources_yields_default_only);
    g_test_add_func ("/theme-listing/dir-form-preferred-over-flat",
                     test_dir_form_preferred_over_flat);
    return g_test_run ();
}
