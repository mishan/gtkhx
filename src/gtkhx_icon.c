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
 * docs/theming.md.
 */
#include "config.h"

#include <string.h>

#include "gtkhx_icon.h"
#include "gtkhx_theme.h" /* gtkhx_theme_active_name */

/* gtkhx_config_dir lives in gtkhx.c (declared in gtkhx.h). Including
 * gtkhx.h pulls in GtkWidget / GdkRGBA / PangoFontDescription, which
 * means dragging gtk/gtk.h into a module that otherwise doesn't need
 * it. Forward-declare instead — same shape tls_trust.c uses. */
extern const char *gtkhx_config_dir (void);

#define GTKHX_ICON_PIXMAP_PREFIX "/com/nasledov/gtkhx/pixmaps/"
#define GTKHX_ICON_THEME_PREFIX "/com/nasledov/gtkhx/themes/"
#define GTKHX_ICON_THEME_SUBDIR "themes"
#define GTKHX_ICON_ICONS_SUBDIR "icons"

/* logical-name (gchar *, owned) → GdkPixbuf * (owned). NULL until
 * the first load. */
static GHashTable *icon_cache;

/* See gtkhx_icon_symbolic_name. */
static GHashTable *symbolic_cache;

void
gtkhx_icon_invalidate_cache (void)
{
    if (icon_cache) {
        g_hash_table_remove_all (icon_cache);
    }
    if (symbolic_cache) {
        g_hash_table_remove_all (symbolic_cache);
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
     * the safe_active_theme_name / gtkhx_theme_active_name check
     * in gtkhx_theme.c). */
    if (strchr (theme, '/') || strchr (theme, '\\')) {
        return NULL;
    }
    char *path = g_strdup_printf ("%s/%s/%s/%s/%s.png", gtkhx_config_dir (),
                                  GTKHX_ICON_THEME_SUBDIR, theme,
                                  GTKHX_ICON_ICONS_SUBDIR, logical);
    GdkPixbuf *pb = NULL;
    if (g_file_test (path, G_FILE_TEST_IS_REGULAR)) {
        GError *err = NULL;
        pb = gdk_pixbuf_new_from_file (path, &err);
        if (!pb) {
            g_warning ("gtkhx_icon: %s exists but won't decode: %s", path,
                       err ? err->message : "(unknown)");
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
    char *res = g_strdup_printf ("%s%s/%s/%s.png", GTKHX_ICON_THEME_PREFIX,
                                 theme, GTKHX_ICON_ICONS_SUBDIR, logical);
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
    char *res = g_strdup_printf ("%s%s.png", GTKHX_ICON_PIXMAP_PREFIX, logical);
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

    /* Cache key is "<active-theme>/<logical>" — including the
     * active-theme prefix keeps entries from the previous theme
     * from being served after a switch, even if the theme
     * "changed" handler order means a button rebuild runs before
     * gtkhx_icon_invalidate_cache. (Belt-and-braces — the
     * invalidate path still runs, but key-by-theme makes the cache
     * correct independent of that ordering. A "/" separator is
     * fine because active_theme() rejects names with path
     * separators — see active_theme.) */
    const char *theme = active_theme ();
    char *cache_key = g_strdup_printf ("%s/%s", theme, logical);

    if (icon_cache) {
        GdkPixbuf *cached = g_hash_table_lookup (icon_cache, cache_key);
        if (cached) {
            g_free (cache_key);
            g_free (logical);
            return g_object_ref (cached);
        }
    }

    /* Lookup order: user theme bundle → built-in theme bundle →
     * stock pixmaps. */
    GdkPixbuf *pb = load_from_user_theme (theme, logical);
    if (!pb) {
        pb = load_from_builtin_theme (theme, logical);
    }
    if (!pb) {
        pb = load_from_default_pixmaps (logical);
    }

    if (pb) {
        if (!icon_cache) {
            icon_cache = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                                g_object_unref);
        }
        /* Cache holds its own reference; hand the caller a fresh one.
         * Insert transfers ownership of cache_key to the hash table. */
        g_hash_table_insert (icon_cache, cache_key, g_object_ref (pb));
    } else {
        g_free (cache_key);
    }
    g_free (logical);
    return pb;
}

/* ---- Symbolic icons ----------------------------------------------------
 *
 * The icon each classic pixmap stands for under a theme that uses the
 * symbolic set. Standard actions use stock icon names, so they match the
 * rest of the desktop and follow a user's own icon theme; the Hotline
 * vocabulary Adwaita has no icon for (chat, the user list, news,
 * broadcast) is vendored under the app's own prefix in the GResource
 * icon tree — see src/icons/README.md.
 *
 * Only non-legacy Adwaita names: the icon theme's legacy/ set is on its
 * way out. A logical name missing here keeps its classic pixmap. */
#define APP_ICON(n) "com.nasledov.gtkhx-" n "-symbolic"

static const struct {
    const char *logical;
    const char *icon;
} symbolic_icons[] = {
    /* Windows and panels */
    { "chat", APP_ICON ("chat") },
    { "users", APP_ICON ("users") },
    { "news", APP_ICON ("news") },
    { "news_folder", "folder-symbolic" },
    { "files", "folder-remote-symbolic" },
    { "tasks", "view-list-symbolic" },
    { "tracker", "network-workgroup-symbolic" },
    { "broadcast", APP_ICON ("broadcast") },
    { "connect", "network-transmit-receive-symbolic" },
    { "options", "preferences-system-symbolic" },
    { "quit", "application-exit-symbolic" },
    /* People */
    { "message", "mail-unread-symbolic" },
    { "info", "help-about-symbolic" },
    { "kick", "system-log-out-symbolic" },
    { "ban", "action-unavailable-symbolic" },
    { "ignore", "view-conceal-symbolic" },
    { "edit_user", "document-edit-symbolic" },
    { "new_user", "contact-new-symbolic" },
    /* Files and transfers */
    { "refresh", "view-refresh-symbolic" },
    { "mkdir", "folder-new-symbolic" },
    { "preview", "view-reveal-symbolic" },
    { "pencil", "document-edit-symbolic" },
    { "trash", "user-trash-symbolic" },
    { "move", "edit-cut-symbolic" },
    { "download", "folder-download-symbolic" },
    { "upload", "document-send-symbolic" },
    { "start", "media-playback-start-symbolic" },
    { "up", "go-up-symbolic" },
    { "down", "go-down-symbolic" },
    /* News */
    { "news_category", APP_ICON ("news") },
    { "news_post", "text-x-generic-symbolic" },
    { "post_news", "mail-message-new-symbolic" },
    /* File types */
    { "folder", "folder-symbolic" },
    { "folder_dropbox", "folder-download-symbolic" },
    { "file", "text-x-generic-symbolic" },
    { "file_text", "text-x-generic-symbolic" },
    { "file_note", "x-office-document-symbolic" },
    { "file_html", "text-x-generic-symbolic" },
    { "file_image", "image-x-generic-symbolic" },
    { "file_movie", "video-x-generic-symbolic" },
    { "file_app", "application-x-executable-symbolic" },
    { "file_sit", "package-x-generic-symbolic" },
    { "file_zip", "package-x-generic-symbolic" },
    { "file_disk", "media-optical-symbolic" },
    { "file_alias", "insert-link-symbolic" },
    { "file_move", "edit-cut-symbolic" },
};

#undef APP_ICON

/* Does the active theme ship its own PNG for this icon? A theme's own
 * glyph wins in either icon style — that is what the icons/ bundle is
 * for. Checked for existence only; gtkhx_icon_load decodes. */
static gboolean
theme_bundles (const char *theme, const char *logical)
{
    if (strchr (theme, '/') || strchr (theme, '\\')) {
        return FALSE;
    }
    g_autofree char *path = g_strdup_printf (
        "%s/%s/%s/%s/%s.png", gtkhx_config_dir (), GTKHX_ICON_THEME_SUBDIR,
        theme, GTKHX_ICON_ICONS_SUBDIR, logical);
    if (g_file_test (path, G_FILE_TEST_IS_REGULAR)) {
        return TRUE;
    }
    g_autofree char *res
        = g_strdup_printf ("%s%s/%s/%s.png", GTKHX_ICON_THEME_PREFIX, theme,
                           GTKHX_ICON_ICONS_SUBDIR, logical);
    return g_resources_get_info (res, G_RESOURCE_LOOKUP_FLAGS_NONE, NULL, NULL,
                                 NULL);
}

/* symbolic_cache (declared with icon_cache above): "<theme>/<logical>"
 * → the answer, a static icon name or "" for "use the pixmap". Cells
 * ask on every bind, and the bundle check touches the filesystem, so
 * answers are kept until the next theme change. */

const char *
gtkhx_icon_symbolic_name (const char *name_or_path)
{
    const char *icon = NULL;
    const char *theme;
    gsize i;

    if (!name_or_path || gtkhx_theme_classic_icons ()) {
        return NULL;
    }
    g_autofree char *logical = basename_no_png (name_or_path);
    theme = active_theme ();
    g_autofree char *key = g_strdup_printf ("%s/%s", theme, logical);

    if (symbolic_cache) {
        const char *cached = g_hash_table_lookup (symbolic_cache, key);
        if (cached) {
            return *cached ? cached : NULL;
        }
    } else {
        symbolic_cache
            = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    }

    for (i = 0; i < G_N_ELEMENTS (symbolic_icons); i++) {
        if (strcmp (symbolic_icons[i].logical, logical) == 0) {
            icon = symbolic_icons[i].icon;
            break;
        }
    }
    if (icon && theme_bundles (theme, logical)) {
        icon = NULL;
    }
    g_hash_table_insert (symbolic_cache, g_steal_pointer (&key),
                         (gpointer)(icon ? icon : ""));
    return icon;
}
