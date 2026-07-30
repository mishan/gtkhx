/*
 * test_icon_pack — coverage for gtkhx_icon under the bundled-theme
 * model: themes ship their icons in $CONFIG/themes/<name>/icons/,
 * the resolver looks there first, falls back to the GResource
 * pixmaps per icon when the theme didn't override.
 *
 * The test target links gtkhx_resources so the built-in PNGs
 * surface via gdk_pixbuf_new_from_resource — that's what powers
 * the "fallback to GResource" assertions. The bundled-icon side
 * is driven via tmp-dir fixtures: write a 1×1 known-colour PNG
 * into $CONFIG/themes/<theme>/icons/<logical>.png, point the
 * fixture's gtkhx_config_dir override at the tmp dir, set
 * gtkhx_prefs.theme_name to the theme name, and the resolver
 * picks it up.
 *
 * See gtkhx_icon.{c,h} and docs/theming-file-format.md.
 */
#include "config.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include <gtk/gtk.h>

#include "gtkhx_icon.h"
#include "gtkhx_theme.h"
#include "hx.h"

/* gtkhx_theme.c reads gtkhx_prefs.theme_name to figure out which
 * theme is active. The fixture mutates that field directly between
 * cases (no full prefs read/write). */
struct gtkhx_prefs gtkhx_prefs;

/* gtkhx_icon.c calls gtkhx_config_dir() to build
 * $CONFIG/themes/<theme>/icons/<logical>.png. The fixture points
 * that at a tmp dir we manage so we never touch the user's real
 * $XDG_CONFIG_HOME. */
static char *g_test_config_dir;

extern const char *gtkhx_config_dir (void);
const char *
gtkhx_config_dir (void)
{
    return g_test_config_dir ? g_test_config_dir : "/tmp/gtkhx-test";
}

/* ---- Fixture helpers ------------------------------------------------ */

static char *
make_tmp_config_dir (void)
{
    GError *err = NULL;
    char *dir = g_dir_make_tmp ("gtkhx-icons-XXXXXX", &err);
    g_assert_no_error (err);
    g_assert_nonnull (dir);
    return dir;
}

/* Write a 1x1 PNG into <config-dir>/themes/<theme>/icons/<basename>.png. */
static void
write_theme_icon (const char *config_dir, const char *theme,
                  const char *basename, int r, int g, int b)
{
    char *dir = g_build_filename (config_dir, "themes", theme, "icons", NULL);
    g_assert_cmpint (g_mkdir_with_parents (dir, 0700), ==, 0);
    char *path = g_build_filename (dir, basename, NULL);

    GdkPixbuf *pb = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, 1, 1);
    g_assert_nonnull (pb);
    guchar *pixels = gdk_pixbuf_get_pixels (pb);
    pixels[0] = (guchar)r;
    pixels[1] = (guchar)g;
    pixels[2] = (guchar)b;

    GError *err = NULL;
    gboolean ok = gdk_pixbuf_save (pb, path, "png", &err, NULL);
    g_assert_no_error (err);
    g_assert_true (ok);

    g_object_unref (pb);
    g_free (path);
    g_free (dir);
}

static void
rmrf (const char *path)
{
    GDir *dir = g_dir_open (path, 0, NULL);
    if (dir) {
        const char *name;
        while ((name = g_dir_read_name (dir)) != NULL) {
            char *child = g_build_filename (path, name, NULL);
            if (g_file_test (child, G_FILE_TEST_IS_DIR)) {
                rmrf (child);
            } else {
                g_unlink (child);
            }
            g_free (child);
        }
        g_dir_close (dir);
    }
    g_rmdir (path);
}

static void
set_fixture (const char *config_dir, const char *theme_name)
{
    g_test_config_dir = (char *)config_dir;
    g_free (gtkhx_prefs.theme_name);
    gtkhx_prefs.theme_name = theme_name ? g_strdup (theme_name) : NULL;
    gtkhx_icon_invalidate_cache ();
}

static void
clear_fixture (void)
{
    g_test_config_dir = NULL;
    g_free (gtkhx_prefs.theme_name);
    gtkhx_prefs.theme_name = NULL;
    gtkhx_icon_invalidate_cache ();
}

/* Read pixel (0,0) from a pixbuf so tests can confirm which source
 * the resolver picked. The GResource icons aren't 1×1 — they're
 * 16×16 PNGs from the gtkhx pixmap set — so a (0,0) sample is some
 * arbitrary pixel of the real icon. The fixture writes a known
 * 1×1 colour, so a mismatch with the fixture means we fell back
 * to GResource (which is what we want to assert positively). */
static void
sample_rgb (GdkPixbuf *pb, int *r, int *g, int *b)
{
    g_assert_nonnull (pb);
    guchar *px = gdk_pixbuf_get_pixels (pb);
    *r = px[0];
    *g = px[1];
    *b = px[2];
}

/* ---- Tests --------------------------------------------------------- */

/* Bundled icon: write a 1×1 known-colour PNG into a theme's
 * icons/ dir, set the active theme to that bundle, ask for the
 * named icon, confirm we got the fixture file's pixel back. */
static void
test_load_hits_theme_bundle (void)
{
    char *cfg = make_tmp_config_dir ();
    write_theme_icon (cfg, "mybundle", "connect.png", 11, 22, 33);
    set_fixture (cfg, "mybundle");

    GdkPixbuf *pb = gtkhx_icon_load ("connect");
    g_assert_nonnull (pb);
    int r, g, b;
    sample_rgb (pb, &r, &g, &b);
    g_assert_cmpint (r, ==, 11);
    g_assert_cmpint (g, ==, 22);
    g_assert_cmpint (b, ==, 33);
    g_object_unref (pb);

    clear_fixture ();
    rmrf (cfg);
    g_free (cfg);
}

/* Per-icon fallback: a theme bundle that supplies "connect" but
 * not "tasks" still serves "tasks" — via the GResource pixmaps.
 * Dimensions ≥ 8×8 catch the "got a real GResource icon, not the
 * 1×1 fixture file we didn't write" case without coupling to the
 * exact pixel art. */
static void
test_load_falls_back_to_pixmap_per_icon (void)
{
    char *cfg = make_tmp_config_dir ();
    write_theme_icon (cfg, "partial", "connect.png", 1, 2, 3);
    set_fixture (cfg, "partial");

    /* connect: from the bundle */
    GdkPixbuf *pb1 = gtkhx_icon_load ("connect");
    g_assert_nonnull (pb1);
    g_assert_cmpint (gdk_pixbuf_get_width (pb1), ==, 1);
    g_object_unref (pb1);

    /* tasks: bundle doesn't have it → GResource pixmap */
    GdkPixbuf *pb2 = gtkhx_icon_load ("tasks");
    g_assert_nonnull (pb2);
    g_assert_cmpint (gdk_pixbuf_get_width (pb2), >=, 8);
    g_object_unref (pb2);

    clear_fixture ();
    rmrf (cfg);
    g_free (cfg);
}

/* The argument may be either "connect" or the full GResource path
 * "/com/nasledov/gtkhx/pixmaps/connect.png" — same resolution.
 * Used by button_load_source which keeps its existing BTN_KEY_RESOURCE
 * storage shape (full path string). */
static void
test_load_accepts_full_resource_path (void)
{
    char *cfg = make_tmp_config_dir ();
    write_theme_icon (cfg, "mybundle", "connect.png", 5, 5, 5);
    set_fixture (cfg, "mybundle");

    GdkPixbuf *pb1 = gtkhx_icon_load ("connect");
    GdkPixbuf *pb2
        = gtkhx_icon_load ("/com/nasledov/gtkhx/pixmaps/connect.png");
    g_assert_nonnull (pb1);
    g_assert_nonnull (pb2);
    int r1, g1, b1, r2, g2, b2;
    sample_rgb (pb1, &r1, &g1, &b1);
    sample_rgb (pb2, &r2, &g2, &b2);
    g_assert_cmpint (r1, ==, r2);
    g_assert_cmpint (g1, ==, g2);
    g_assert_cmpint (b1, ==, b2);
    g_object_unref (pb1);
    g_object_unref (pb2);

    clear_fixture ();
    rmrf (cfg);
    g_free (cfg);
}

/* The "default" theme name still consults
 * $CONFIG/themes/default/icons/ for overrides — that's the
 * shadow-the-built-in rule that lets a user customize stock
 * icons without inventing a new theme name. Mirrors how theme
 * files let $CONFIG/themes/default.ini shadow the built-in
 * default theme GResource. */
static void
test_default_theme_shadows_when_user_icon_present (void)
{
    char *cfg = make_tmp_config_dir ();
    write_theme_icon (cfg, "default", "tasks.png", 99, 88, 77);
    set_fixture (cfg, "default");

    GdkPixbuf *pb = gtkhx_icon_load ("tasks");
    g_assert_nonnull (pb);
    int r, g, b;
    sample_rgb (pb, &r, &g, &b);
    g_assert_cmpint (r, ==, 99);
    g_assert_cmpint (g, ==, 88);
    g_assert_cmpint (b, ==, 77);
    g_object_unref (pb);

    clear_fixture ();
    rmrf (cfg);
    g_free (cfg);
}

/* Cache is keyed by logical name; the same name resolved twice
 * returns the same underlying pixbuf (two refs to one object).
 * After invalidation, a fresh load returns a *different* pixbuf
 * (the cache forgot the previous one and re-decoded). */
static void
test_cache_returns_same_pixbuf_until_invalidated (void)
{
    char *cfg = make_tmp_config_dir ();
    write_theme_icon (cfg, "cachy", "connect.png", 7, 7, 7);
    set_fixture (cfg, "cachy");

    GdkPixbuf *a = gtkhx_icon_load ("connect");
    GdkPixbuf *b = gtkhx_icon_load ("connect");
    g_assert_nonnull (a);
    g_assert_true (a == b);
    g_object_unref (a);
    g_object_unref (b);

    /* Swap to a different theme + source the same name with a
     * different colour. After invalidate, the fresh load picks up
     * the new bytes. */
    set_fixture (cfg, "other");
    write_theme_icon (cfg, "other", "connect.png", 50, 60, 70);

    GdkPixbuf *c = gtkhx_icon_load ("connect");
    g_assert_nonnull (c);
    int r, g_, b_;
    sample_rgb (c, &r, &g_, &b_);
    g_assert_cmpint (r, ==, 50);
    g_assert_cmpint (g_, ==, 60);
    g_assert_cmpint (b_, ==, 70);
    g_object_unref (c);

    clear_fixture ();
    rmrf (cfg);
    g_free (cfg);
}

/* NULL pref / no fixture: every icon load falls through to the
 * GResource pixmaps without crashing. */
static void
test_no_active_theme_falls_back_to_pixmaps (void)
{
    char *cfg = make_tmp_config_dir ();
    set_fixture (cfg, NULL);

    GdkPixbuf *pb = gtkhx_icon_load ("tasks");
    g_assert_nonnull (pb);
    g_assert_cmpint (gdk_pixbuf_get_width (pb), >=, 8);
    g_object_unref (pb);

    clear_fixture ();
    rmrf (cfg);
    g_free (cfg);
}

int
main (int argc, char **argv)
{
    /* gtk_init_check() rather than gtk_init(): this test only needs
     * GLib + GdkPixbuf (the PNG loaders and the GResource
     * auto-constructor that self-registers at load), none of which
     * require a display. gtk_init() aborts on a headless CI runner
     * with no GdkDisplay; gtk_init_check() degrades to FALSE instead,
     * which is fine here — we ignore the result. */
    (void)gtk_init_check ();

    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/icon-pack/load-hits-theme-bundle",
                     test_load_hits_theme_bundle);
    g_test_add_func ("/icon-pack/load-falls-back-per-icon",
                     test_load_falls_back_to_pixmap_per_icon);
    g_test_add_func ("/icon-pack/load-accepts-full-resource-path",
                     test_load_accepts_full_resource_path);
    g_test_add_func ("/icon-pack/default-theme-shadows-when-user-icon-present",
                     test_default_theme_shadows_when_user_icon_present);
    g_test_add_func ("/icon-pack/cache-returns-same-pixbuf-until-invalidated",
                     test_cache_returns_same_pixbuf_until_invalidated);
    g_test_add_func ("/icon-pack/no-active-theme-falls-back-to-pixmaps",
                     test_no_active_theme_falls_back_to_pixmaps);
    return g_test_run ();
}
