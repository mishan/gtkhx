/*
 * gtkhx_icon.c — chrome icon resolver. See gtkhx_icon.h for the
 * contract.
 *
 * Themes are bundles. A theme can ship its own icons by living as
 * a *directory* — $CONFIG/themes/<name>/theme.ini plus
 * $CONFIG/themes/<name>/icons/<logical>.png — instead of (or as
 * well as) a flat .ini file. The resolver looks up:
 *
 *   1. $CONFIG/themes/<active>/icons/<logical>.png   (user dir-form)
 *   2. /com/nasledov/gtkhx/themes/<active>/icons/    (built-in dir-form
 *        <logical>.png                                in GResource)
 *   3. /com/nasledov/gtkhx/pixmaps/<logical>.png      (built-in fallback;
 *                                                      always present
 *                                                      for stock GtkHx
 *                                                      icons)
 *
 * Flat-form themes (a single .ini file with no sibling directory)
 * simply have nothing at steps 1+2 and fall through to step 3.
 * Per-icon fallback: a theme dir that ships only a few icons works
 * fine — every other icon falls through to step 3.
 *
 * The brand logo (gtkhx.png) is deliberately NOT routed through this
 * resolver — it's loaded via gdk_pixbuf_new_from_resource directly
 * in about.c.
 *
 * Cache shape: a `logical-name → GdkPixbuf*` GHashTable. Dropped
 * wholesale by gtkhx_icon_invalidate_cache, which the theme
 * "changed" handler in gtkhx.c calls before buttons rebuild via
 * button_load_source → gtkhx_icon_load.
 *
 * PNG-only in v1. SVG via glycin is the planned follow-up — see
 * docs/theming-scoping.md.
 */
#include "config.h"

#include <string.h>

#include "gtkhx_icon.h"
#include "gtkhx_theme.h"  /* gtkhx_theme_active_name */

/* gtkhx_config_dir lives in gtkhx.c (declared in gtkhx.h). Including
 * gtkhx.h pulls in GtkWidget / GdkRGBA / PangoFontDescription, which
 * means dragging gtk/gtk.h into a module that otherwise doesn't need
 * it. Forward-declare instead — same shape tls_trust.c uses. */
extern const char *gtkhx_config_dir (void);

#define GTKHX_ICON_PIXMAP_PREFIX  "/com/nasledov/gtkhx/pixmaps/"
#define GTKHX_ICON_THEME_PREFIX   "/com/nasledov/gtkhx/themes/"
#define GTKHX_ICON_THEME_SUBDIR   "themes"
#define GTKHX_ICON_ICONS_SUBDIR   "icons"

/* logical-name (gchar *, owned) → GdkPixbuf * (owned). NULL until
 * the first load. */
static GHashTable *icon_cache;

void
gtkhx_icon_invalidate_cache (void)
{
    if (icon_cache) {
        g_hash_table_remove_all (icon_cache);
    }
}

/* Strip everything up to and including the last '/', then strip a
 * trailing ".png" if present. Returns a fresh string. */
static char *
basename_no_png (const char *path_or_name)
{
    const char *slash = strrchr (path_or_name, '/');
    const char *base = slash ? slash + 1 : path_or_name;
    gsize n = strlen (base);
    if (n > 4 && g_ascii_strcasecmp (base + n - 4, ".png") == 0) {
        return g_strndup (base, n - 4);
    }
    return g_strdup (base);
}

/* Active theme name (THEMENAME pref), wrapped through the theme
 * module so we don't need to drag prefs.h's GTK transitive headers
 * into this translation unit. Never NULL. */
static const char *
active_theme (void)
{
    return gtkhx_theme_active_name ();
}

/* Try the user theme's bundled icon at
 * $CONFIG/themes/<theme>/icons/<logical>.png. */
static GdkPixbuf *
load_from_user_theme (const char *theme, const char *logical)
{
    /* Defensive: reject theme names with a path separator (matches
	 * gtkhx_theme's active_theme_path check). */
    if (strchr (theme, '/') || strchr (theme, '\\')) {
        return NULL;
    }
    char *path = g_strdup_printf (
        "%s/%s/%s/%s/%s.png",
        gtkhx_config_dir (), GTKHX_ICON_THEME_SUBDIR, theme,
        GTKHX_ICON_ICONS_SUBDIR, logical);
    GdkPixbuf *pb = NULL;
    if (g_file_test (path, G_FILE_TEST_IS_REGULAR)) {
        GError *err = NULL;
        pb = gdk_pixbuf_new_from_file (path, &err);
        if (!pb) {
            g_warning ("gtkhx_icon: %s exists but won't decode: %s",
                       path, err ? err->message : "(unknown)");
            g_clear_error (&err);
        }
    }
    g_free (path);
    return pb;
}

/* Try the built-in theme's bundled icon at
 * /com/nasledov/gtkhx/themes/<theme>/icons/<logical>.png. */
static GdkPixbuf *
load_from_builtin_theme (const char *theme, const char *logical)
{
    if (strchr (theme, '/') || strchr (theme, '\\')) {
        return NULL;
    }
    char *res = g_strdup_printf (
        "%s%s/%s/%s.png", GTKHX_ICON_THEME_PREFIX, theme,
        GTKHX_ICON_ICONS_SUBDIR, logical);
    GdkPixbuf *pb = gdk_pixbuf_new_from_resource (res, NULL);
    g_free (res);
    return pb;
}

/* Always-fallback: the stock chrome GResource. Every named GtkHx
 * icon lives here, so this is the last-resort that never misses
 * for a valid logical name. */
static GdkPixbuf *
load_from_default_pixmaps (const char *logical)
{
    char *res = g_strdup_printf ("%s%s.png",
                                 GTKHX_ICON_PIXMAP_PREFIX, logical);
    GdkPixbuf *pb = gdk_pixbuf_new_from_resource (res, NULL);
    g_free (res);
    return pb;
}

GdkPixbuf *
gtkhx_icon_load (const char *name_or_path)
{
    if (!name_or_path) {
        return NULL;
    }

    char *logical = basename_no_png (name_or_path);
    if (!*logical) {
        g_free (logical);
        return NULL;
    }

    /* Cache hit? Return a fresh reference. */
    if (icon_cache) {
        GdkPixbuf *cached = g_hash_table_lookup (icon_cache, logical);
        if (cached) {
            g_free (logical);
            return g_object_ref (cached);
        }
    }

    /* Lookup order: user theme bundle → built-in theme bundle →
	 * stock pixmaps. */
    const char *theme = active_theme ();
    GdkPixbuf *pb = load_from_user_theme (theme, logical);
    if (!pb) {
        pb = load_from_builtin_theme (theme, logical);
    }
    if (!pb) {
        pb = load_from_default_pixmaps (logical);
    }

    if (pb) {
        if (!icon_cache) {
            icon_cache = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                g_free, g_object_unref);
        }
        /* Cache holds its own reference; hand the caller a fresh one. */
        g_hash_table_insert (icon_cache, g_strdup (logical),
                             g_object_ref (pb));
    }

    g_free (logical);
    return pb;
}
