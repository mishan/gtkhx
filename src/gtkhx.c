/*
 * Copyright (C) 2000-2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General
 * Public License along with this program; if not, write to the
 * Free Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <gtk/gtk.h>
#include <adwaita.h>
#include <libpanel.h>
#include <gdk/gdkkeysyms.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <time.h>
#include <sys/wait.h>
#include <errno.h>
#include <signal.h>
#include <termios.h>
#include <ctype.h>
#include <locale.h>
#ifdef HAVE_LIBINTL_H
#include <libintl.h> /* bindtextdomain, bind_textdomain_codeset, textdomain */
#endif
#include <pwd.h>
#include <getopt.h>
#include "hx.h"
#include "macres.h"
#include "cicn.h"
#include "news.h"
#include "news15.h"
#include "news_browser.h"
#include "files_remote_provider.h"
#include "users.h"
#include "gif_icons.h"  /* hx_icon_get — re-fetch on ICON_CHANGE */
#include "gif_avatar.h" /* gtkhx_avatar_update / _clear_all */
#include "files.h"
#include "tasks.h"
#include "network.h"
#include "gtkutil.h"
#include "gtkhx_theme.h" /* gtkhx_theme_get_color, GTKHX_PAL_* */
#include "debug.h"
#include "toolbar.h"
#include "dock_layout.h"
#include "chat.h"
#include "msg.h"
#include "gtkhx_session.h"
#include "gtkhx_icon.h"
#include "notify.h"
#include "tracker.h"
#ifdef HAVE_VOICE
#include "voice_runtime.h"
#include "voice_model.h"
#endif
#include "tray.h"
#include "xtext.h"
#include "sound_events.h"
#include "options.h"
#include "xfers.h"
#include "commands.h"
#include "log.h"
#include "gtkhx.h"

char last_msg_nick[32];
char *g_user_colors[4] = { WHITE_BOLD, WHITE, RED_BOLD, RED };

struct ifn user_icon_files;
struct ifn icon_files;
GdkRGBA fg_col;
GdkRGBA bg_col;

PangoFontDescription *gtkhx_font_desc;

static GtkCssProvider *gtkhx_css_provider = NULL;
static GtkCssProvider *gtkhx_userlist_css_provider = NULL;

static void
ensure_provider_attached (GtkCssProvider *prov)
{
    GdkDisplay *display = gdk_display_get_default ();
    if (!display) {
        return;
    }
    gtk_style_context_add_provider_for_display (
        display, GTK_STYLE_PROVIDER (prov),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

/* Map a Pango font description to a CSS properties block. We can't use
 * the GTK CSS `font: <pango-string>;` shorthand because GTK 3.20+ flags
 * the unit-less point size as a parse error ("not a number"). Emit the
 * properties individually so the standard CSS parser is happy. */
static gchar *
pango_to_css_props (const PangoFontDescription *fd)
{
    const char *family;
    gint size_pt;
    gboolean is_abs;
    PangoWeight weight;
    PangoStyle style;
    const char *weight_s;
    const char *style_s;

    if (!fd) {
        return g_strdup ("");
    }

    family = pango_font_description_get_family (fd);
    size_pt = pango_font_description_get_size (fd);
    is_abs = pango_font_description_get_size_is_absolute (fd);
    weight = pango_font_description_get_weight (fd);
    style = pango_font_description_get_style (fd);

    switch (weight) {
    case PANGO_WEIGHT_THIN:
        weight_s = "100";
        break;
    case PANGO_WEIGHT_ULTRALIGHT:
        weight_s = "200";
        break;
    case PANGO_WEIGHT_LIGHT:
        weight_s = "300";
        break;
    case PANGO_WEIGHT_BOOK:
        weight_s = "380";
        break;
    case PANGO_WEIGHT_NORMAL:
        weight_s = "400";
        break;
    case PANGO_WEIGHT_MEDIUM:
        weight_s = "500";
        break;
    case PANGO_WEIGHT_SEMIBOLD:
        weight_s = "600";
        break;
    case PANGO_WEIGHT_BOLD:
        weight_s = "700";
        break;
    case PANGO_WEIGHT_ULTRABOLD:
        weight_s = "800";
        break;
    case PANGO_WEIGHT_HEAVY:
        weight_s = "900";
        break;
    case PANGO_WEIGHT_ULTRAHEAVY:
        weight_s = "1000";
        break;
    default:
        weight_s = "400";
        break;
    }

    switch (style) {
    case PANGO_STYLE_ITALIC:
        style_s = "italic";
        break;
    case PANGO_STYLE_OBLIQUE:
        style_s = "oblique";
        break;
    case PANGO_STYLE_NORMAL:
    default:
        style_s = "normal";
        break;
    }

    if (size_pt <= 0) {
        return g_strdup_printf ("font-family: \"%s\";"
                                "font-weight: %s;"
                                "font-style: %s;",
                                family ? family : "Monospace", weight_s,
                                style_s);
    }

    if (is_abs) {
        return g_strdup_printf ("font-family: \"%s\";"
                                "font-size: %dpx;"
                                "font-weight: %s;"
                                "font-style: %s;",
                                family ? family : "Monospace",
                                size_pt / PANGO_SCALE, weight_s, style_s);
    }

    return g_strdup_printf ("font-family: \"%s\";"
                            "font-size: %dpt;"
                            "font-weight: %s;"
                            "font-style: %s;",
                            family ? family : "Monospace",
                            size_pt / PANGO_SCALE, weight_s, style_s);
}

void
gtkhx_refresh_css (void)
{
    gchar *fontprops;
    gchar *css;
    AdwStyleManager *sm = adw_style_manager_get_default ();
    gboolean dark = adw_style_manager_get_dark (sm);
    GdkRGBA fg = gtkhx_theme_get_color (GTKHX_PAL_FG, dark);
    GdkRGBA bg = gtkhx_theme_get_color (GTKHX_PAL_BG, dark);

    if (!gtkhx_css_provider) {
        gtkhx_css_provider = gtk_css_provider_new ();
        ensure_provider_attached (gtkhx_css_provider);
    }

    fontprops = pango_to_css_props (gtkhx_font_desc);

    /* Two themed rules:
	 *
	 *   .gtkhx-text   — read-only text surfaces (agreement window, news
	 *                   bodies, broadcast viewer, gchat subject entry).
	 *                   Gets the active theme's font AND fg/bg/caret
	 *                   so themed surfaces match the chat output.
	 *
	 *   .gtkhx-input  — editable text views (chat / PM / pchat input
	 *                   boxes). Gets ONLY the theme's fg/bg/caret —
	 *                   font stays on the built-in .monospace class
	 *                   (applied via gtkhx_apply_input_font). Setting
	 *                   font on the input via .gtkhx-text-style CSS
	 *                   triggered an ascender-ink clip on newly typed
	 *                   glyphs at small Monospace sizes (Phase-5 bug);
	 *                   colors don't have that problem.
	 *
	 * The descendant ".gtkhx-{text,input} text" rule reaches
	 * GtkTextView's inner "text" CSS node so the actual body picks
	 * up the same look (GtkTextView's outer node is just chrome).
	 *
	 * Earlier we deliberately omitted color/background-color here
	 * because the source was a *single* hardcoded (fg_col, bg_col)
	 * pair that forced light-grey-on-black on those widgets
	 * regardless of the user's Light/Dark choice. The theme model
	 * fixes that: each theme has explicit light + dark palette
	 * variants, and we pick the variant via AdwStyleManager's `dark`
	 * property — same dispatch the xtext palette uses — so the
	 * applied colors always match the active light/dark mode. */
    gchar *fghex = g_strdup_printf ("#%02x%02x%02x",
                                    (int) (fg.red   * 255.0 + 0.5),
                                    (int) (fg.green * 255.0 + 0.5),
                                    (int) (fg.blue  * 255.0 + 0.5));
    gchar *bghex = g_strdup_printf ("#%02x%02x%02x",
                                    (int) (bg.red   * 255.0 + 0.5),
                                    (int) (bg.green * 255.0 + 0.5),
                                    (int) (bg.blue  * 255.0 + 0.5));

    /* .gtkhx-listview — color theming for list-shaped surfaces
	 * (GtkColumnView, GtkListView, GtkListBox): tracker, tasks,
	 * files browser, news browser.
	 *
	 * Only emitted for *non-default* themes. The built-in "default"
	 * theme is supposed to feel like vanilla GtkHx + system colors
	 * — its [palette] values are tuned for the chat output, not for
	 * sidebar lists, and forcing #fafafa on a user's GNOME dark
	 * desktop's listview was visually wrong. A user who picks
	 * Solarized signed up for the cream-on-cream look across every
	 * surface; the default theme stays out of the way.
	 *
	 * Header rows (.gtkhx-listview > header on GtkColumnView) are
	 * never themed — column titles always render in the system
	 * theme so they stay legible on any background.
	 *
	 * Two cases to cover, because the class is sometimes applied
	 * to a parent of the listview/list node and sometimes directly
	 * to it:
	 *
	 *   GtkColumnView (tracker, files panels): class sits on the
	 *     `columnview` node, so the rows are .gtkhx-listview
	 *     descendants — `listview > row` / `> row > cell` match.
	 *   GtkListView (news_browser): class sits on the `listview`
	 *     node itself; rows are direct children → `.gtkhx-listview
	 *     > row` matches.
	 *   GtkListBox (tasks): class sits on the `list` node itself;
	 *     rows are direct children → same direct-child rule.
	 *
	 * Both descendant and direct-child selectors are emitted so a
	 * caller can apply .gtkhx-listview at either layer without
	 * worrying about the node tree. Headers (.gtkhx-listview >
	 * header) stay system-themed so column titles are always
	 * legible. Selection (row:selected) keeps the system accent.
	 *
	 * The chat-shaped surfaces (.gtkhx-text / .gtkhx-input) ALWAYS
	 * get painted — chat is the surface the palette was designed
	 * for, and built-in defaults are tuned for it.
	 *
	 * The listview-shaped surfaces only get painted when the active
	 * theme has *explicitly* set FG or BG. That gates the "force
	 * chat colors onto sidebar lists" behavior to themes that opted
	 * in: Solarized's palette.{light,dark} fg/bg keys trigger it; a
	 * theme that omits FG/BG (the shipped default, post-cleanup)
	 * leaves the listview surfaces at the system theme. */
    GString *css_buf = g_string_new (NULL);
    g_string_append_printf (
        css_buf,
        ".gtkhx-text, .gtkhx-text text {"
        "  %s"
        "  color: %s;"
        "  background-color: %s;"
        "  caret-color: %s;"
        "}"
        ".gtkhx-input, .gtkhx-input text {"
        "  color: %s;"
        "  background-color: %s;"
        "  caret-color: %s;"
        "}",
        fontprops, fghex, bghex, fghex,
        fghex, bghex, fghex);
    if (gtkhx_theme_palette_role_is_set (GTKHX_PAL_FG, dark)
        || gtkhx_theme_palette_role_is_set (GTKHX_PAL_BG, dark)) {
        /* Row selectors carry :not(:hover):not(:active) so the
		 * system theme's hover overlay and click-feedback rules
		 * still win for those pseudo-classes — without that
		 * exclusion our PRIORITY_APPLICATION background paints
		 * over the system's :hover overlay and the user sees no
		 * feedback when mousing over rows. The outer columnview /
		 * listview / list nodes (no row state) and the cells (read
		 * row state via parent matching) keep the theme bg in the
		 * steady state. */
        g_string_append_printf (
            css_buf,
            ".gtkhx-listview,"
            ".gtkhx-listview listview,"
            ".gtkhx-listview listview > row:not(:selected):not(:hover):not(:active),"
            ".gtkhx-listview listview > row:not(:selected):not(:hover):not(:active) > cell,"
            ".gtkhx-listview list,"
            ".gtkhx-listview list > row:not(:selected):not(:hover):not(:active),"
            ".gtkhx-listview > row:not(:selected):not(:hover):not(:active),"
            ".gtkhx-listview > row:not(:selected):not(:hover):not(:active) > cell {"
            "  color: %s;"
            "  background-color: %s;"
            "}",
            fghex, bghex);
    }
    css = g_string_free (css_buf, FALSE);

    gtk_css_provider_load_from_string (gtkhx_css_provider, css);

    g_free (css);
    g_free (fghex);
    g_free (bghex);
    g_free (fontprops);
}

void
gtkhx_refresh_userlist_css (PangoFontDescription *fd)
{
    gchar *fontprops;
    gchar *css;
    AdwStyleManager *sm = adw_style_manager_get_default ();
    gboolean dark = adw_style_manager_get_dark (sm);
    GdkRGBA fg = gtkhx_theme_get_color (GTKHX_PAL_FG, dark);
    GdkRGBA bg = gtkhx_theme_get_color (GTKHX_PAL_BG, dark);
    gchar *fghex;
    gchar *bghex;

    if (!gtkhx_userlist_css_provider) {
        gtkhx_userlist_css_provider = gtk_css_provider_new ();
        ensure_provider_attached (gtkhx_userlist_css_provider);
    }

    fontprops = pango_to_css_props (fd);
    fghex = g_strdup_printf ("#%02x%02x%02x",
                             (int) (fg.red   * 255.0 + 0.5),
                             (int) (fg.green * 255.0 + 0.5),
                             (int) (fg.blue  * 255.0 + 0.5));
    bghex = g_strdup_printf ("#%02x%02x%02x",
                             (int) (bg.red   * 255.0 + 0.5),
                             (int) (bg.green * 255.0 + 0.5),
                             (int) (bg.blue  * 255.0 + 0.5));

    /* Font selector stays on .gtkhx-userlist itself (the users-list
	 * font pref) regardless of theme — separate user pref the
	 * userlist always honors. Theme fg/bg are painted across the
	 * column-view's inner nodes ONLY when the active theme has
	 * explicitly set FG or BG; themes that didn't opt in (and the
	 * shipped default after the default.ini cleanup) leave the
	 * userlist at system colors. Headers always keep system styling
	 * so column titles stay legible. Selection (row:selected)
	 * keeps the system accent. */
    GString *css_buf = g_string_new (NULL);
    g_string_append_printf (css_buf, ".gtkhx-userlist { %s }", fontprops);
    if (gtkhx_theme_palette_role_is_set (GTKHX_PAL_FG, dark)
        || gtkhx_theme_palette_role_is_set (GTKHX_PAL_BG, dark)) {
        /* :not(:hover):not(:active) so the system theme's hover
		 * overlay + click feedback still paint over our theme bg.
		 * Same reasoning as the .gtkhx-listview rule in
		 * gtkhx_refresh_css. */
        g_string_append_printf (
            css_buf,
            ".gtkhx-userlist,"
            ".gtkhx-userlist listview,"
            ".gtkhx-userlist listview > row:not(:selected):not(:hover):not(:active),"
            ".gtkhx-userlist listview > row:not(:selected):not(:hover):not(:active) > cell {"
            "  color: %s;"
            "  background-color: %s;"
            "}",
            fghex, bghex);
    }
    css = g_string_free (css_buf, FALSE);

    gtk_css_provider_load_from_string (gtkhx_userlist_css_provider, css);

    g_free (css);
    g_free (fghex);
    g_free (bghex);
    g_free (fontprops);
}

void
gtkhx_apply_text_style (GtkWidget *w)
{
    if (!w) {
        return;
    }
    if (!gtkhx_css_provider) {
        gtkhx_refresh_css ();
    }

    /* gtk_style_context_add_class is deprecated in GTK 4.10
	 * and was the source of a gtk_css_node_insert_after assertion when
	 * adding the class on a widget whose CSS node hadn't been parented
	 * yet. gtk_widget_add_css_class is the modern, safer path. */
    if (!gtk_widget_has_css_class (w, "gtkhx-text")) {
        gtk_widget_add_css_class (w, "gtkhx-text");
    }
}

void
gtkhx_apply_input_font (GtkWidget *w)
{
    if (!w || !GTK_IS_TEXT_VIEW (w)) {
        return;
    }
    /* gtk_text_view_set_monospace() sets the built-in .monospace CSS
     * class which the theme handles natively (usually picks Source Code
     * Pro or similar at the theme's UI size). We can't honor the user's
     * size pick from Settings this way — but every other mechanism we
     * tried (CSS .gtkhx-text class, PangoContext font_description, font
     * via GtkTextTag) triggered an ascender-ink clip on newly typed
     * glyphs at small sizes (Monospace 9 / 10), and at this point
     * monospace-at-theme-size is a strictly better outcome than the
     * clip. ASCII-art preservation (the main reason to want monospace)
     * still works. */
    gtk_text_view_set_monospace (GTK_TEXT_VIEW (w), TRUE);
}

void
gtkhx_apply_input_style (GtkWidget *w)
{
    if (!w) {
        return;
    }
    if (!gtkhx_css_provider) {
        gtkhx_refresh_css ();
    }
    /* Color-only sibling of .gtkhx-text — paints the active theme's
     * fg / bg / caret on chat / PM / pchat input boxes without
     * touching the font. See gtkhx_refresh_css for the carve-out
     * rationale and the per-class CSS payload. */
    if (!gtk_widget_has_css_class (w, "gtkhx-input")) {
        gtk_widget_add_css_class (w, "gtkhx-input");
    }
}

void
gtkhx_apply_listview_style (GtkWidget *w)
{
    if (!w) {
        return;
    }
    if (!gtkhx_css_provider) {
        gtkhx_refresh_css ();
    }
    /* List-shaped sibling of .gtkhx-text / .gtkhx-input — paints
     * theme fg/bg on the column-view's listview, non-selected rows,
     * and cells. See gtkhx_refresh_css for the CSS payload. */
    if (!gtk_widget_has_css_class (w, "gtkhx-listview")) {
        gtk_widget_add_css_class (w, "gtkhx-listview");
    }
}

void
gtkhx_apply_userlist_style (GtkWidget *w)
{
    if (!w) {
        return;
    }
    if (!gtkhx_userlist_css_provider) {
        gtkhx_refresh_userlist_css (NULL);
    }

    if (!gtk_widget_has_css_class (w, "gtkhx-userlist")) {
        gtk_widget_add_css_class (w, "gtkhx-userlist");
    }
}
static struct timer *timer_list;

static int rinput_tags[1024];
static int winput_tags[1024];

const char *INFOPREFIX = " \00310[\00303hx\00310]\003 ";

session the_session;

/*
 * hx_active_session — the currently-focused session (multi-conn seam,
 * see docs/multi-connection-scoping.md phase M0). N == 1 today, so this
 * returns the single session. When the connection tab strip lands this
 * becomes a lookup of the focused tab's session; every UI call site that
 * routes through here follows automatically.
 */
session *
hx_active_session (void)
{
    return &the_session;
}

/* Forward declaration of the application object owned by loop(). hx_quit()
 * needs to call g_application_quit on it, but loop() comes much later
 * in this TU and gtkhx_app's definition lives there.
 *
 * AdwApplication subclasses GtkApplication, so the existing
 * gtk_application_* call sites keep working unchanged. The upgrade
 * gives us libadwaita's AdwStyleManager (color-scheme tracking,
 * Adwaita stylesheet) for free. */
static AdwApplication *gtkhx_app;

/* the dialog code paths in gtkutil.c (error_dialog) and
 * options.c need a transient parent to satisfy GTK 4's "GtkDialog
 * mapped without a transient parent" warning. Expose the application's
 * currently-active window so callers without a widget context can
 * still parent their dialogs correctly. Returns NULL during early
 * startup before the application is constructed. */
GtkWindow *
gtkhx_active_window (void)
{
    if (!gtkhx_app) {
        return NULL;
    }
    return gtk_application_get_active_window (GTK_APPLICATION (gtkhx_app));
}

GApplication *
gtkhx_get_application (void)
{
    return G_APPLICATION (gtkhx_app);
}

/* Resolve the per-user config root, in priority order:
 *   1. $GTKHX_PATH if set (escape hatch / portable installs)
 *   2. $XDG_CONFIG_HOME/gtkhx
 *   3. $HOME/.config/gtkhx
 *
 * The directory is created with mkdir -p semantics on first call so
 * downstream callers never have to worry about ENOENT on the parent.
 * The returned string is owned by this function — caller must not
 * free. Subsequent calls return the cached path.
 *
 * Subdirectories used by the rest of the codebase:
 *   $CONFIG/gtkhxrc      — main prefs file
 *   $CONFIG/bookmarks/   — connect.c bookmarks (replaces ~/.hx/bookmarks)
 *   $CONFIG/logs/        — log.c (replaces ~/.hx/logs)
 *   $CONFIG/icons/       — auto-discovered Mac classic icon resources (.rsrc)
 *   $CONFIG/sounds/      — user-supplied chat sound effects
 */
const char *
gtkhx_config_dir (void)
{
    static char *cached;
    const char *override;
    const char *xdg;
    const char *home;

    if (cached) {
        return cached;
    }

    override = g_getenv ("GTKHX_PATH");
    if (override && *override) {
        cached = g_strdup (override);
    } else {
        xdg = g_getenv ("XDG_CONFIG_HOME");
        if (xdg && *xdg) {
            cached = g_build_filename (xdg, "gtkhx", NULL);
        } else {
            home = g_getenv ("HOME");
            if (!home || !*home) {
                home = g_get_home_dir ();
            }
            cached = g_build_filename (home ? home : "/", ".config", "gtkhx",
                                       NULL);
        }
    }

    if (g_mkdir_with_parents (cached, 0700) != 0) {
        g_warning ("gtkhx_config_dir: mkdir %s: %s", cached,
                   g_strerror (errno));
    }

    return cached;
}

// Capture size at quit
static void
save_geo (GtkWidget *w, Window_Geo *geo)
{
    int width, height;
    if (!w || !gtk_widget_get_realized (w)) {
        return;
    }
    gtk_window_get_default_size (GTK_WINDOW (w), &width, &height);
    if (width > 0) {
        geo->xsize = width;
    }
    if (height > 0) {
        geo->ysize = height;
    }
}

static void
gtkhx_save_window_positions (void)
{
    save_geo (toolbar_window, &gtkhx_prefs.geo.tool);
    /* Chat / Users / Tasks / News
     * panels live inside the toolbar window; per-panel size
     * persistence is Phase 4 layout restore. */
}

void
hx_quit (void)
{
    gtkhx_save_window_positions ();
    prefs_write ();
    dock_layout_shutdown ();   /* flush pending debounced save */
    xfers_delete_all ();
    tracker_kill_threads ();

    if (the_session.htlc.fd) {
        hx_htlc_close (&the_session.htlc, 1);
    }

#if 0 /* XXX */
	close_logs();
#endif

    if (gtkhx_app) {
        g_application_quit (G_APPLICATION (gtkhx_app));
    }
    exit (0);
}

void
timer_add_secs (time_t secs, int (*fn) (void *), void *ptr)
{
    struct timer *timer;
    guint id;

    id = g_timeout_add (secs * 1000, (GSourceFunc)fn, ptr);

    timer = g_malloc (sizeof (struct timer));
    timer->next = 0;
    timer->prev = timer_list;
    if (timer_list) {
        timer_list->next = timer;
    }
    timer_list = timer;
    timer->id = id;
    timer->fn = fn;
    timer->ptr = ptr;
}

void
timer_delete_ptr (void *ptr)
{
    struct timer *timer;

    for (timer = timer_list; timer;) {
        struct timer *prev = timer->prev;

        if (timer->ptr == ptr) {
            if (timer->next) {
                timer->next->prev = timer->prev;
            }
            if (timer->prev) {
                timer->prev->next = timer->next;
            }
            if (timer == timer_list) {
                timer_list = timer->next;
            }
            g_source_remove (timer->id);
            g_free (timer); /* bring out yer dead! */
        }

        timer = prev;
    }
}

static gboolean
hxd_gtk_read (GIOChannel *source, GIOCondition cond, struct hxd_file *file)
{
    if (file->ready_read) {
        file->ready_read (file->fd);
    }
    return TRUE;
}

static gboolean
hxd_gtk_write (GIOChannel *source, GIOCondition cond, struct hxd_file *file)
{
    if (file->ready_write) {
        file->ready_write (file->fd);
    }
    return TRUE;
}

void
hxd_fd_set (int fd, int rw)
{
    int tag, type = 0;
    GIOChannel *channel;

    if (fd >= 1024) {
        hx_printf_prefix (&hx_active_session ()->htlc, 0, INFOPREFIX,
                          "gtkhx: fd %d >= 1024", fd);
        hx_quit ();
    }

    channel = g_io_channel_unix_new (fd);
    if (rw & FDR) {
        /*		printf("adding fd %d for reading\n", fd); */
        if (rinput_tags[fd] != -1) {
            return;
        }
        type |= G_IO_IN | G_IO_HUP | G_IO_ERR;
        tag = g_io_add_watch (channel, type, (GIOFunc)hxd_gtk_read,
                              &hxd_files[fd]);
        rinput_tags[fd] = tag;
    }
    if (rw & FDW) {
        /*		printf("adding fd %d for writing\n", fd); */
        if (winput_tags[fd] != -1) {
            return;
        }
        type |= G_IO_OUT | G_IO_ERR;
        tag = g_io_add_watch (channel, type, (GIOFunc)hxd_gtk_write,
                              &hxd_files[fd]);
        winput_tags[fd] = tag;
    }
}

void
hxd_fd_clr (int fd, int rw)
{
    int tag;

    if (fd >= 1024) {
        hx_printf_prefix (&hx_active_session ()->htlc, 0, INFOPREFIX,
                          "gtkhx: fd %d >= 1024", fd);
        hx_quit ();
    }
    /* The arrays are pre-zeroed to -1 (see init()), so a clear request
	 * for a fd that was never set up — e.g. cleanup at exit on a
	 * connection that only ever had a read watch — would otherwise call
	 * g_source_remove((guint)-1) and trip GLib's "Source ID 4294967295
	 * was not found" critical. */
    if ((rw & FDR) && rinput_tags[fd] != -1) {
        tag = rinput_tags[fd];
        g_source_remove (tag);
        rinput_tags[fd] = -1;
    }
    if ((rw & FDW) && winput_tags[fd] != -1) {
        tag = winput_tags[fd];
        g_source_remove (tag);
        winput_tags[fd] = -1;
    }
}

static void
init_colors (GtkWidget *widget)
{
    static const GdkRGBA defaults_user_colors[8] = {
        RGB16 (0x0000, 0x0000, 0x0000), RGB16 (0xffff, 0x0000, 0x0000),
        RGB16 (0x0000, 0xffff, 0x0000), RGB16 (0xffff, 0xffff, 0x0000),
        RGB16 (0x0000, 0x0000, 0xffff), RGB16 (0xffff, 0x0000, 0xffff),
        RGB16 (0x0000, 0xffff, 0xffff), RGB16 (0xffff, 0xffff, 0xffff),
    };
    static const GdkRGBA defaults_gdk_user_colors[4] = {
        RGB16 (0x0000, 0x0000, 0x0000), /* black */
        RGB16 (0xa0a0, 0xa0a0, 0xa0a0), /* grey */
        RGB16 (0xffff, 0x0000, 0x0000), /* red */
        RGB16 (0xffff, 0xa7a7, 0xb0b0), /* light pink */
    };

    (void)widget;
    memcpy (user_colors, defaults_user_colors, sizeof user_colors);
    memcpy (gdk_user_colors, defaults_gdk_user_colors, sizeof gdk_user_colors);
}

char *
colorstr (guint16 color)
{
    char *col;

    col = g_user_colors[color % 4];

    return col;
}

/* scan a directory for *.rsrc files and append each one to
 * a GPtrArray of full paths. Skips entries whose path is already in
 * the array, so the same file showing up in both $CONFIG/icons and
 * the system data dir doesn't get loaded twice.
 *
 * Per-directory entries are sorted by base name (g_utf8_collate)
 * before being appended, so the load order — and therefore the
 * lookup precedence in load_icon's first-match-wins walk — is
 * deterministic and matches what the user sees alphabetically in
 * the directory. The cross-directory priority order is preserved
 * by init_icons calling this function in the right sequence
 * ($CONFIG/icons first, then XDG, then PREFIX). */
static gint
collect_rsrc_cmp_basename (gconstpointer a, gconstpointer b)
{
    const char *na = *(const char *const *)a;
    const char *nb = *(const char *const *)b;
    return g_utf8_collate (na, nb);
}

static void
collect_rsrc_files (GPtrArray *out, const char *dir)
{
    GDir *d;
    const char *name;
    GPtrArray *names;
    guint i;

    if (!dir || !*dir) {
        return;
    }
    d = g_dir_open (dir, 0, NULL);
    if (!d) {
        return;
    }

    names = g_ptr_array_new_with_free_func (g_free);
    while ((name = g_dir_read_name (d))) {
        if (!g_str_has_suffix (name, ".rsrc")) {
            continue;
        }
        g_ptr_array_add (names, g_strdup (name));
    }
    g_dir_close (d);

    g_ptr_array_sort (names, collect_rsrc_cmp_basename);

    for (i = 0; i < names->len; i++) {
        char *path;
        guint j;
        gboolean dup = FALSE;

        path = g_build_filename (dir, g_ptr_array_index (names, i), NULL);
        for (j = 0; j < out->len; j++) {
            if (g_strcmp0 (g_ptr_array_index (out, j), path) == 0) {
                dup = TRUE;
                break;
            }
        }
        if (dup) {
            g_free (path);
        } else {
            g_ptr_array_add (out, path);
        }
    }

    g_ptr_array_free (names, TRUE);
}

void
init_icons (void)
{
    int fd, i;
    struct ifn *ifn = &icon_files;
    GPtrArray *paths;
    char *user_dir;

    if (ifn->cicns) {
        for (i = 0; i < (int)ifn->n; i++) {
            if (ifn->cicns[i]) {
                macres_file_delete (ifn->cicns[i]);
                ifn->cicns[i] = 0;
            }
        }
        g_free (ifn->cicns);
        ifn->cicns = 0;
    }

    if (ifn->files) {
        for (i = 0; i < (int)ifn->n; i++) {
            g_free (ifn->files[i]);
        }
        g_free (ifn->files);
    }

    /* build the list of icon resource files from auto-discovery
     * locations. The legacy ICONS pref (a comma-separated list of .rsrc
     * paths) was retired with the path-pref cleanup — drop a file into
     * $CONFIG/icons/ instead. Sources, in priority order:
     *   1. $CONFIG/icons/                            — per-user drop-ins
     *   2. $XDG_DATA_HOME/gtkhx/icons/               — Flatpak / app-style
     *   3. each $XDG_DATA_DIRS/gtkhx/icons/          — distro-shipped
     *      (covers /usr/share, /usr/local/share, snap mounts, etc.)
     *   4. $PREFIX/share/gtkhx/icons/                — build-time fixed path
     *   5. $PREFIX/share/gtkhx/                      — legacy top-level
     *      (autotools-era installs put icons.rsrc directly here),
     *      ONLY if 1-4 turned up nothing
     *
     * collect_rsrc_files de-dupes by absolute path string, so the same
     * file showing up in two of these (e.g. PREFIX matches an XDG dir)
     * doesn't load twice. The legacy top-level scan is special-cased:
     * if the meson install has put icons.rsrc into both
     * $PREFIX/share/gtkhx/icons/icons.rsrc (new) AND
     * $PREFIX/share/gtkhx/icons.rsrc (legacy autotools-era leftover),
     * those are different paths so the path-string de-dupe doesn't
     * catch them — they have the same content though, and you'd see
     * every icon doubled in the Settings picker. So only fall back to
     * the legacy top-level scan if the proper scans found zero files. */
    paths = g_ptr_array_new ();

    user_dir = g_build_filename (gtkhx_config_dir (), "icons", NULL);
    collect_rsrc_files (paths, user_dir);
    g_free (user_dir);

    {
        char *p
            = g_build_filename (g_get_user_data_dir (), "gtkhx", "icons", NULL);
        collect_rsrc_files (paths, p);
        g_free (p);
    }

    {
        const char *const *dirs = g_get_system_data_dirs ();
        for (; dirs && *dirs; dirs++) {
            char *p = g_build_filename (*dirs, "gtkhx", "icons", NULL);
            collect_rsrc_files (paths, p);
            g_free (p);
        }
    }

    collect_rsrc_files (paths, PREFIX "/share/gtkhx/icons");

    if (paths->len == 0) {
        collect_rsrc_files (paths, PREFIX "/share/gtkhx");
    }

    ifn->files = g_malloc (paths->len * sizeof (char *));
    ifn->cicns = g_malloc (paths->len * sizeof (macres_file *));

    for (i = 0; i < (int)paths->len; i++) {
        ifn->files[i] = (char *)g_ptr_array_index (paths, i);
        fd = open (ifn->files[i], O_RDONLY);
        if (fd < 0) {
            g_warning ("%s: %s\n", ifn->files[i], strerror (errno));
            ifn->cicns[i] = 0;
            continue;
        }
        ifn->cicns[i] = macres_file_open (fd);
        close (fd);
    }
    ifn->n = paths->len;
    /* free the GPtrArray shell only — element strings transferred
     * ownership to ifn->files. */
    g_ptr_array_free (paths, FALSE);
}

static void
fe_init (void)
{
    GtkWidget *widg = gtk_button_new ();

    generate_colors (widg);
    gtkhx_widget_destroy (widg);
    init_variables ();

    memset (&icon_files, 0, sizeof (icon_files));
    prefs_read ();
    /* Load the active theme before any widget construction. fe_init
     * builds the toolbar / file-browser / users / etc. windows below;
     * those button helpers subscribe to the theme "changed" signal
     * and read scales live. Loading the theme here means the very
     * first measure pass gets the right factors instead of building
     * at the built-in default and then re-laying-out on the post-
     * fe_init load. See gtkhx_theme.{c,h}. */
    gtkhx_theme_load_active ();
    /* prep the screen-wide CSS provider once prefs are loaded
     * so the very first widget that gets gtkhx_apply_text_style() picks
     * up the right look on the first paint. */
    gtkhx_refresh_css ();
    init_icons ();

    /* Single-session construction site. fe_init (here), hx_quit's
     * teardown, main()'s zero-init, and hotline_client_init's identity
     * setup are the only places that touch the concrete `the_session`
     * storage rather than the sess_from_htlc() / hx_active_session()
     * accessors — because this is where the one session is born and
     * dies. Multi-conn (phase M3) turns these into a session factory
     * over a collection; every accessor-routed call site downstream
     * follows without further edits.
     *
     * hashtable-backed session collections. chats_init additionally
     * seeds the table with the public chat (cid=0), which must always
     * exist while the table does. */
    chats_init (&the_session);
    tasks_init (&the_session);
    msg_windows_init (&the_session);

#ifdef HAVE_VOICE
    /* Voice indicator model. Lives for the whole session lifetime
     * — users_view subscribes once at window construction time and
     * the model survives reconnects (state cleared inside
     * hx_htlc_close). Created here so users_view can connect to its
     * "indicator-changed" signal during fe_init's window-creation
     * sweep below. */
    the_session.voice_model = hx_voice_model_new ();
#endif

    gtkhx_connect_signals (gtkhx_session_get_default ());

    /* Sound-effect subscriber: maps model-side signals (and the voice
     * model's presence chime) to play_sound ids. Connected after
     * gtkhx_connect_signals and after the voice model exists. */
#ifdef HAVE_VOICE
    gtkhx_sound_events_init (gtkhx_session_get_default (),
                             the_session.voice_model);
#else
    gtkhx_sound_events_init (gtkhx_session_get_default (), NULL);
#endif

    /* create_chat / create_tasks
     * build per-session widget state (xtext + chat_hbox, gtask
     * scroll) that the toolbar window's eager-panel-construction
     * path consumes when it runs create_chat_window /
     * create_tasks_window. Order matters: model state first, then
     * the toolbar (which now hosts everything). */
    create_chat  (&the_session);
    create_tasks (&the_session);

    create_toolbar_window (&the_session);
    init_colors (toolbar_window);

    /* Panels are eager-constructed inside create_toolbar_window;
     * these init-bit checks just registry-lookup-hit and raise the
     * corresponding tab, preserving the "open chat when you reconnect"
     * prefs semantics — the panel becomes the focused tab on launch. */
    if (gtkhx_prefs.geo.chat.init == 1) {
        create_chat_window (toolbar_window, &the_session);
    }
    if (gtkhx_prefs.geo.news.init == 1) {
        create_news_window (toolbar_window, &the_session);
    }
    if (gtkhx_prefs.geo.users.init == 1) {
        create_users_window (toolbar_window, &the_session);
    }
    if (gtkhx_prefs.geo.tasks.init == 1) {
        create_tasks_window (toolbar_window, &the_session);
    }

    reinit_gtktexts (&the_session);
}

/* AdwStyleManager::notify::dark trampoline — reads the new dark
 * state off the manager and pushes it into the xtext palette plus
 * every live chat-output widget; then rebuilds the .gtkhx-text /
 * .gtkhx-input CSS provider so the agreement / news / chat-input
 * surfaces re-paint with the dark variant of the active theme's
 * fg/bg. Connected once from gtkhx_activate. */
static void
on_style_manager_dark_changed (GObject *object, GParamSpec *pspec,
                               gpointer user_data)
{
    AdwStyleManager *sm = ADW_STYLE_MANAGER (object);
    (void)pspec;
    (void)user_data;
    gtkhx_apply_theme_palette (adw_style_manager_get_dark (sm));
    gtkhx_refresh_css ();
    /* The users-list provider also encodes theme fg/bg now — keep
     * the existing font (users_font_desc lives in users.c and is
     * the source of truth for the userlist font pref). */
    gtkhx_refresh_userlist_css (users_font_desc);
}

/* GtkhxTheme::changed trampoline — when the active theme file is
 * reloaded (THEMENAME edit, Settings theme picker), re-pull every
 * UI-role slot for the *currently active* light/dark variant and
 * repaint every live xtext; then rebuild the CSS provider for the
 * non-xtext text surfaces (agreement, news, inputs). Connected
 * once from gtkhx_activate alongside the AdwStyleManager
 * subscription. */
static void
on_theme_changed (GtkhxTheme *theme, gpointer user_data)
{
    AdwStyleManager *sm = adw_style_manager_get_default ();
    (void)theme;
    (void)user_data;
    gtkhx_apply_theme_palette (adw_style_manager_get_dark (sm));
    gtkhx_refresh_css ();
    gtkhx_refresh_userlist_css (users_font_desc);
    /* The icon pack is also part of the theme — drop any cached
     * resolved pixbufs so buttons rebuilding via button_load_source
     * pick up the new pack's images. Buttons subscribe to this same
     * signal, so the rebuild happens in lockstep below. */
    gtkhx_icon_invalidate_cache ();
}

static void
gtkhx_activate (GtkApplication *app, gpointer user_data)
{
    GList *toplevels, *l;

    (void)user_data;

    /* Hook the bundled icon up to the icon theme so windows can
     * find it by name, then declare it as the default icon for
     * every window we open. The pixmap is aliased into the
     * gresource at /com/nasledov/gtkhx/icons/16x16/apps/
     * com.nasledov.gtkhx.xpm — i.e., the conventional
     * <prefix>/<size>/<context>/<name>.<ext> layout that
     * gtk_icon_theme_add_resource_path scans. The icon name itself
     * matches our GApplication app-id ("com.nasledov.gtkhx") so a
     * future .desktop file with Icon=com.nasledov.gtkhx and a
     * matching system-installed icon path will Just Work for
     * Wayland compositor / dock integration. */
    {
        GdkDisplay *display = gdk_display_get_default ();
        if (display) {
            GtkIconTheme *theme = gtk_icon_theme_get_for_display (display);
            gtk_icon_theme_add_resource_path (theme,
                                              "/com/nasledov/gtkhx/icons");
        }
        gtk_window_set_default_icon_name ("com.nasledov.gtkhx");
    }

    /* fe_init() ran before g_application_run(), which means every
     * window the auto-open path created (chat / users / tasks / news,
     * plus the toolbar itself) already exists and has had show_all()
     * called on it. Any toplevel that isn't registered with the
     * GtkApplication doesn't get its xdg_toplevel commit serviced
     * during the activate cycle on Wayland — symptom: the chat window
     * stays invisible until the user closes and re-opens it (the
     * second open happens after the app is fully up and works fine).
     *
     * Sweep gtk_window_list_toplevels() and add every GtkWindow the
     * app doesn't already own. Idempotent — add_window() is a no-op
     * if the window is already in the app's list. */
    toplevels = gtk_window_list_toplevels ();
    for (l = toplevels; l; l = l->next) {
        if (GTK_IS_WINDOW (l->data)) {
            gtk_application_add_window (app, GTK_WINDOW (l->data));
        }
    }
    g_list_free (toplevels);

    /* register the toolbar's hamburger-menu actions
     * (app.settings / app.about / app.quit). The actions can't be
     * added during create_toolbar_window because that runs in
     * fe_init() — before g_application_run, so before the
     * AdwApplication exists. Now that we're in the activate handler
     * the application is alive, so wire them in. */
    toolbar_register_actions (G_APPLICATION (app), &the_session);

    /* bind Ctrl+Q (and Ctrl+K) to GApplication actions so the
     * accelerators work from every window without per-window
     * GtkShortcutController plumbing. Previously these were installed
     * only on windows that called init_keyaccel(), so e.g. typing
     * Ctrl+Q with the chat window focused did nothing. Application-
     * level accels work in any window the GtkApplication owns —
     * gtk_application_add_window above brings the existing toplevels
     * into that set. */
    {
        const char *quit_accels[] = { "<Control>q", NULL };
        gtk_application_set_accels_for_action (app, "app.quit", quit_accels);
    }

    /* StatusNotifierItem tray icon. Reads the TRAY pref at
     * register-time; the changed_tray cfgvar callback flips it on/off
     * as the user toggles the Setting. */
    gtkhx_tray_init (app);

    /* desktop notifications. Each event-class entry
     * point consults its NOTIFY_* pref + the
     * notify_omit_focused gate before posting. */
    gtkhx_notify_init (app);

    /* Seed and track the xtext chat-output palette against the
     * AdwStyleManager's dark state. The static colors[] array in
     * chat.c starts with dark-mode XTEXT_FG/XTEXT_BG values, which
     * we'll either keep (manager says dark) or rewrite to the
     * light-mode set (manager says light) before any window opens.
     * Subsequent `notify::dark` fires (e.g. user flips THEME in
     * Settings, or follows-system and system goes dark) re-run
     * gtkhx_apply_theme_palette to refresh every open xtext.
     * The theme's "changed" signal does the same when the active
     * theme file is reloaded (THEMENAME changed, future Settings
     * theme picker), reusing the variant the manager currently
     * reports. */
    {
        AdwStyleManager *sm = adw_style_manager_get_default ();
        gtkhx_apply_theme_palette (adw_style_manager_get_dark (sm));
        /* Re-emit the .gtkhx-text / .gtkhx-input CSS with the now-
         * settled light/dark variant. fe_init's earlier refresh_css
         * fired before AdwStyleManager had its final dark state, so
         * the provider could be holding the wrong variant's fg/bg —
         * that's fine for the toolbar (which doesn't use those
         * classes) but the agreement / news / inputs about to be
         * built in subsequent windows would have inherited the
         * stale colors. */
        gtkhx_refresh_css ();
        g_signal_connect (sm, "notify::dark",
                          G_CALLBACK (on_style_manager_dark_changed), NULL);
        g_signal_connect (gtkhx_theme_get_default (), "changed",
                          G_CALLBACK (on_theme_changed), NULL);
    }
}

static void
loop (void)
{
    /* AdwApplication wraps GtkApplication and additionally
     * calls adw_init() so libadwaita's stylesheet, types, and the
     * AdwStyleManager singleton are available app-wide. The activate
     * signal still fires the same way; existing window-registration
     * logic in gtkhx_activate stays unchanged. */
    gtkhx_app
        = adw_application_new ("com.nasledov.gtkhx", G_APPLICATION_NON_UNIQUE);
    g_signal_connect (gtkhx_app, "activate", G_CALLBACK (gtkhx_activate), NULL);

    /* g_application_run() takes argc/argv only to forward them to a
     * "command-line" handler we don't install — pass 0/NULL so it
     * doesn't try to re-parse our flags. */
    g_application_run (G_APPLICATION (gtkhx_app), 0, NULL);

    g_object_unref (gtkhx_app);
    gtkhx_app = NULL;
}

static void
init (int argc, char **argv)
{
    int i;

    /* parse the GTKHX_DEBUG env var into the categorised
     * debug logger before anything else, so init paths can already
     * call debug_log("startup", ...) etc. The proto_trace module
     * checks debug_category_enabled("proto") on every send/recv
     * hook — a lookup against a (possibly empty) hash table, cheap
     * enough to leave unconditionally. */
    debug_init ();

    for (i = 0; i < 1024; i++) {
        rinput_tags[i] = -1;
        winput_tags[i] = -1;
    }
    /* gtk_set_locale() was removed in GTK 3 — gtk_init() now handles
	 * setlocale() itself. */
    setlocale (LC_ALL, "");
    /* Tell gettext where our message catalogues live and which domain
     * the _() macro should look up. Without these calls dgettext()
     * either consults the wrong domain ("messages") or searches the
     * compiled-in default LOCALEDIR (typically /usr/share/locale on
     * glibc), so a binary installed under /usr/local/share/locale sees
     * zero translations. PACKAGE and PACKAGE_LOCALE_DIR are both
     * defined in config.h via meson; the codeset bind tells libintl to
     * hand us UTF-8 regardless of the user's LC_CTYPE so GTK doesn't
     * trip over Latin-1 in fr_FR / es_ES locale aliases. */
#ifdef HAVE_LIBINTL_H
    bindtextdomain (PACKAGE, PACKAGE_LOCALE_DIR);
    bind_textdomain_codeset (PACKAGE, "UTF-8");
    textdomain (PACKAGE);
#endif
    gtk_init ();
    /* panel_init() registers libpanel's boxed
     * types and CSS provider. It has to run before the first
     * libpanel widget construction; fe_init -> create_toolbar_window
     * (below) builds the dock + grid, so init here. Idempotent and
     * does not require an AdwApplication instance (good, because we
     * don't have one yet — see the long comment in
     * create_toolbar_window in toolbar.c about NULL gtkhx_app at
     * this point in init). */
    panel_init ();
#ifdef HAVE_VOICE
    /* Phase 8.B: initialise GStreamer for the voice runtime. Idempotent;
     * the Rust hxvoice-runtime wraps gst::init which checks its own
     * "already initialised" flag. Order doesn't matter relative to
     * gtk_init (GStreamer doesn't care about display init) but landing
     * it after gtk_init keeps the "init the world in order" reading
     * honest. A 0 return means GStreamer failed to start; voice UI
     * (Phase 8.D) will be disabled rather than crashing. */
    if (gtkhx_voice_init () == 0) {
        g_warning (
            "gtkhx_voice_init failed; voice features will be unavailable");
    }
#endif
    fe_init ();
}

/* output_user_info (the Get-User-Info result window) ported to Rust
 * (gtkhx-ui user_info.rs); the on_user_info_signal adapter below links
 * against the #[no_mangle] export via this forward declaration. */
extern void output_user_info (guint16 uid, const char *nam, const char *info,
                              guint16 len);

/* Forward declarations for the file-local view functions the
 * Phase 3 adapter handlers below call. The functions themselves
 * are defined later in this file; the extern-linkage ones
 * (output_chat_subject, output_news_*, msg_output, etc.) are
 * already declared in their respective headers.
 *
 * the chat-signal path bypasses
 * the gtkhx.c output_chat stub — the renderer is
 * chat.c::output_chat_from_event, which takes the pre-parsed
 * HxChatEvent directly. */
/* Agreement window ported to Rust (gtkhx-ui, agreement.rs). */
extern void gtkhx_show_agreement (session *sess, const char *agreement,
                                  guint16 len);

/* Phase 3+ signal adapters — bridge the GObject marshaller signature
 * (instance, signal args (with guint16 widened to guint), user_data)
 * to the legacy view function signatures. Will get cleaned up once
 * Phase 3 finishes and the legacy signatures change to match. */
static void
on_chat_signal (GtkhxSession *emitter, struct htlc_conn *htlc, gpointer event_p,
                gpointer user_data)
{
    HxChatEvent *event = event_p;
    (void)emitter;
    (void)user_data;
    if (!event) {
        return;
    }
    output_chat_from_event (htlc, event);
    gtkhx_notify_chat (event);
}

static void
on_chat_subject_signal (GtkhxSession *emitter, struct htlc_conn *htlc,
                        guint cid, gpointer subj, gpointer user_data)
{
    (void)emitter;
    (void)user_data;
    output_chat_subject (htlc, (guint32)cid, (char *)subj);
}

static void
on_chat_history_batch_signal (GtkhxSession *emitter, struct htlc_conn *htlc,
                              guint cid, gpointer entries_p, gboolean has_more,
                              gpointer user_data)
{
    GPtrArray *entries = entries_p;
    (void)emitter;
    (void)user_data;
    output_chat_history_batch (htlc, (guint32)cid, entries, has_more);
}

static void
on_chat_invitation_signal (GtkhxSession *emitter, struct htlc_conn *htlc,
                           guint cid, gpointer name, gpointer user_data)
{
    (void)emitter;
    (void)user_data;
    output_chat_invitation (htlc, (guint32)cid, (char *)name);
    gtkhx_notify_pchat_invite ((guint32)cid, (const char *)name);
}

static void
on_msg_signal (GtkhxSession *emitter, gpointer event_p, gpointer user_data)
{
    HxMsgEvent *event = event_p;
    (void)emitter;
    (void)user_data;
    if (!event) {
        return;
    }
    msg_output_from_event (event);
    gtkhx_notify_msg (event);
}

/* "logged-in" — the LOGIN task reply came back successful and the reply
 * has been fully walked (so htlc->version, server_addr, and caps are all
 * settled). Settle the connected-state UI in one shot: window titles
 * (server_addr-dependent), toolbar buttons (news15 gate is version >=
 * 150), and the status bar. The LOGIN chime rides the same signal in
 * sound_events.c. This used to be an inline changetitlesconnected +
 * setbtns + set_status_bar (twice) in rcv_task_login. */
static void
on_logged_in_signal (GtkhxSession *emitter, struct htlc_conn *htlc,
                     gpointer user_data)
{
    session *sess = hx_active_session ();
    (void)emitter;
    (void)htlc;
    (void)user_data;
    changetitlesconnected (sess);
    setbtns (sess, 1);
    set_status_bar (2);
}

/* "self-updated" — our own access bits / uid were (re)parsed from a
 * SELFINFO reply. Refresh toolbar-button sensitivity, which gates on the
 * access bitmap (kick/ban, etc.). Was an inline setbtns in
 * hx_rcv_user_selfinfo. */
static void
on_self_updated_signal (GtkhxSession *emitter, struct htlc_conn *htlc,
                        gpointer user_data)
{
    (void)emitter;
    (void)htlc;
    (void)user_data;
    setbtns (hx_active_session (), 1);
}

static void
on_agreement_signal (GtkhxSession *emitter, gpointer sess, gpointer agreement,
                     guint len, gpointer user_data)
{
    (void)emitter;
    (void)user_data;
    gtkhx_show_agreement ((session *)sess, (const char *)agreement,
                          (guint16)len);
}

static void
on_news_file_signal (GtkhxSession *emitter, struct htlc_conn *htlc,
                     gpointer news, guint len, gpointer user_data)
{
    (void)emitter;
    (void)user_data;
    output_news_file (htlc, (char *)news, (guint16)len);
}

static void
on_news_post_signal (GtkhxSession *emitter, struct htlc_conn *htlc,
                     gpointer news, guint len, gpointer user_data)
{
    (void)emitter;
    (void)user_data;
    output_news_post (htlc, (char *)news, (guint16)len);

    /* News posts can be paragraphs long; pull just the first
	 * line as the notification preview. */
    {
        const char *raw = (const char *)news;
        const char *nl;
        char *first_line;
        gsize use = len;
        nl = memchr (raw, '\n', len);
        if (nl) {
            use = nl - raw;
        }
        nl = memchr (raw, '\r', use);
        if (nl) {
            use = nl - raw;
        }
        first_line = g_strndup (raw, use);
        gtkhx_notify_news (first_line);
        g_free (first_line);
    }
}

/* 1.5 threaded-news replies. The unified browser is the only
 * producer of gnews_folder / gnews_catalog / news_post stubs since
 * the legacy two-window UI was retired, so the browser handlers
 * always own the reply. If the browser closed in flight (handler
 * returns FALSE), the stub is leaked — that's the deliberate
 * trade-off described in news_browser.c's pending-table comment;
 * one-shot 4 kB allocations on a now-defunct window aren't worth
 * tracking. */
static void
on_news_folder_signal (GtkhxSession *emitter, gpointer gfnews,
                       gpointer user_data)
{
    (void)emitter;
    (void)user_data;
    gnews_browser_handle_dirlist (gfnews);
}

static void
on_news_catalog_signal (GtkhxSession *emitter, gpointer gcnews,
                        gpointer user_data)
{
    (void)emitter;
    (void)user_data;
    gnews_browser_handle_catlist (gcnews);
}

static void
on_news_thread_signal (GtkhxSession *emitter, gpointer post, gpointer user_data)
{
    (void)emitter;
    (void)user_data;
    gnews_browser_handle_thread (post);
}

static void
on_user_create_signal (GtkhxSession *emitter, struct htlc_conn *htlc,
                       struct chat *chat, struct hx_user *user, gpointer nam,
                       guint icon, guint color, gboolean incremental,
                       gpointer user_data)
{
    (void)emitter;
    (void)incremental; /* join-chime gate; consumed by sound_events.c */
    (void)user_data;
    user_create (htlc, chat, user, (const char *)nam, (guint16)icon,
                 (guint16)color);
}

static void
on_user_delete_signal (GtkhxSession *emitter, struct htlc_conn *htlc,
                       struct chat *chat, struct hx_user *user,
                       gboolean incremental, gpointer user_data)
{
    (void)emitter;
    (void)incremental; /* part-chime gate; consumed by sound_events.c */
    (void)user_data;
    user_delete (htlc, chat, user);
}

static void
on_user_change_signal (GtkhxSession *emitter, struct htlc_conn *htlc,
                       struct chat *chat, struct hx_user *user, gpointer nam,
                       guint icon, guint color, gpointer user_data)
{
    (void)emitter;
    (void)user_data;
    user_change (htlc, chat, user, (const char *)nam, (guint16)icon,
                 (guint16)color);
}

static void
on_users_clear_signal (GtkhxSession *emitter, struct htlc_conn *htlc,
                       struct chat *chat, gpointer user_data)
{
    (void)emitter;
    (void)user_data;
    users_clear (htlc, chat);
    /* Clearing the public user list is the view-side disconnect
	 * boundary. GIF avatars are per-session server-side, so drop the
	 * whole cache (and cancel any in-flight decodes) — a reconnect
	 * re-probes and re-fetches from scratch. */
    if (chat && chat->cid == 0) {
        gtkhx_avatar_clear_all ();
    }
}

/* GIF-icons extension (Phase 10.B). gif-icon-data carries a user's raw
 * GIF (or empty = cleared); hand it to the avatar cache, which decodes
 * (bounded, async) and refreshes the affected user-list rows. */
static void
on_gif_icon_data_signal (GtkhxSession *emitter, struct htlc_conn *htlc,
                         guint uid, gpointer gif, guint len, gpointer user_data)
{
    (void)emitter;
    (void)htlc;
    (void)user_data;
    gtkhx_avatar_update ((guint16)uid, (const guint8 *)gif, (gsize)len);
}

/* gif-icon-changed carries only a uid (the ICON_CHANGE broadcast). Pull
 * the new avatar; the ICON_GET reply re-emits gif-icon-data, which the
 * handler above caches + renders. */
static void
on_gif_icon_changed_signal (GtkhxSession *emitter, struct htlc_conn *htlc,
                            guint uid, gpointer user_data)
{
    (void)emitter;
    (void)user_data;
    hx_icon_get (htlc, (guint16)uid);
}

static void
on_user_info_signal (GtkhxSession *emitter, guint uid, gpointer nam,
                     gpointer info, guint len, gpointer user_data)
{
    (void)emitter;
    (void)user_data;
    output_user_info ((guint16)uid, (const char *)nam, (const char *)info,
                      (guint16)len);
}

static void
on_file_info_signal (GtkhxSession *emitter, gpointer path, gpointer name,
                     gpointer creator, gpointer type, gpointer comments,
                     gpointer modified, gpointer created, guint64 size,
                     gpointer user_data)
{
    (void)emitter;
    (void)user_data;
    output_file_info ((char *)path, (char *)name, (char *)creator, (char *)type,
                      (char *)comments, (char *)modified, (char *)created,
                      size);
}

static void
on_file_list_signal (GtkhxSession *emitter, gpointer cfl, gpointer fh,
                     gpointer data, gpointer user_data)
{
    (void)emitter;
    (void)user_data;
    /* only the new files browser remains; route the
	 * response to its remote-files-provider. The legacy
	 * output_file_list fallback is gone with the rest of the
	 * legacy gfile_list UI. Responses without a recognised
	 * provider carrier (e.g. stale FILE_LIST tasks from a
	 * closed window) get harmlessly dropped inside the handler. */
    (void)hx_remote_files_provider_handle_file_list (cfl, fh, data);
}

static void
on_file_update_signal (GtkhxSession *emitter, gpointer sess, gpointer htxf,
                       gpointer user_data)
{
    struct htxf_conn *x = (struct htxf_conn *)htxf;
    (void)emitter;
    (void)user_data;
    file_update ((session *)sess, x);

    /* file_update fires repeatedly during a transfer. Detect
	 * "just finished" by total_pos catching up to total_size and
	 * notify once. The xfer worker sets total_pos = total_size
	 * explicitly at end-of-stream and then exits (post_xfer_
	 * cleanup follows), so this state is reached exactly once
	 * per htxf in practice. The notification ID is shared
	 * ("xfer"), so even if it fired twice the second would just
	 * refresh the popup, not stack.
	 *
	 * For folder transfers, prefer the original folder name
	 * (htxf->remotename, set at xfer_new_folder time and never
	 * touched again) over htxf->path: the threshold can be
	 * crossed inside file_recv_one when the LAST file's data
	 * fork completes, at which point htxf->path is the per-file
	 * path, not the folder root. folder_get_thread restores
	 * htxf->path to the folder root before the final
	 * post_file_update, but the in-flight one beats it. Using
	 * remotename sidesteps the timing entirely. */
    if (x && x->total_size > 0 && x->total_pos >= x->total_size) {
        const char *display = NULL;
        if (x->opt.folder && x->remotename_len > 0) {
            display = (const char *)x->remotename;
        } else {
            const char *path = x->path;
            const char *base = path ? strrchr (path, '/') : NULL;
            display = base ? base + 1 : (path ? path : NULL);
        }
        gtkhx_notify_xfer_done (display);
    }
}

static void
on_xfer_queue_signal (GtkhxSession *emitter, gpointer sess, gpointer htxf,
                      gpointer user_data)
{
    (void)emitter;
    (void)user_data;
    output_xfer_queue ((session *)sess, (struct htxf_conn *)htxf);
}

/* "xfer-destroyed" — fires from xfer_remove_from_list right before
 * the xfers[] reference is dropped. The htxf is still alive at
 * this point (the worker thread or queued dispatchers may still
 * hold refs), but it can be freed asynchronously by any of those
 * unrefs once we return. The tasks window's gtask retains a raw
 * pointer to the htxf, so we sever it here — leaving the UI row
 * in place (file_update's pos>=size path already removes finished
 * rows; if we still have a row at this point it's a canceled /
 * errored transfer where the user would rather see a final state
 * than have the row vanish). */
static void
on_xfer_destroyed_signal (GtkhxSession *emitter, gpointer sess, gpointer htxf,
                          gpointer user_data)
{
    (void)emitter;
    (void)user_data;
    gtask_clear_htxf ((session *)sess, (struct htxf_conn *)htxf);
}

static void
on_tracker_server_create_signal (GtkhxSession *emitter, HxTrackerServer *event,
                                 gpointer user_data)
{
    (void)emitter;
    (void)user_data;
    tracker_server_create (event);
}

static void
on_tracker_batch_begin_signal (GtkhxSession *emitter, const char *tracker_url,
                               guchar version, guint expected_count,
                               gpointer user_data)
{
    (void)emitter;
    (void)user_data;
    tracker_batch_begin (tracker_url, (guint8)version, (guint16)expected_count);
}

static void
on_task_update_signal (GtkhxSession *emitter, gpointer sess, gpointer tsk,
                       gpointer user_data)
{
    (void)emitter;
    (void)user_data;
    task_update ((session *)sess, (struct task *)tsk);
}

/* Translates GtkhxConnectionState (high-level FSM) into the per-aspect
 * UI calls that network.c hx_connect / connect_fail / hx_htlc_close
 * used to issue by name. setbtns / set_status_bar / set_disconnect_btn
 * / conn_task_update / changetitlesdisconnected are still view-side
 * helpers callable on their own; this just routes the state-change
 * notifications through the same signal mechanism as everything else. */
static void
on_connection_state_changed_signal (GtkhxSession *emitter, guint state,
                                    gpointer user_data)
{
    session *sess = hx_active_session ();
    (void)emitter;
    (void)user_data;

    switch (state) {
    case GTKHX_CONNECTION_DISCONNECTED:
        setbtns (sess, 0);
        set_status_bar (0);
        set_disconnect_btn (sess, 0);
        conn_task_update (sess, 2);
        changetitlesdisconnected (sess);
        gtkhx_tray_set_connected (FALSE);
        break;
    case GTKHX_CONNECTION_CONNECTING:
        set_status_bar (-1);
        set_disconnect_btn (sess, 1);
        conn_task_update (sess, 0);
        /* Wipe any toasts left over from the previous server (task
         * errors, broadcasts, "Logged in"). Without this the user
         * can be looking at a stale toast from server A while
         * already connecting to server B. */
        toolbar_clear_toasts ();
        break;
    case GTKHX_CONNECTION_TCP_CONNECTED:
        set_status_bar (1);
        conn_task_update (sess, 1);
        gtkhx_tray_set_connected (TRUE);
        break;
    case GTKHX_CONNECTION_HANDSHAKE_DONE:
        conn_task_update (sess, 2);
        break;
    case GTKHX_CONNECTION_LOGIN_READY:
        /* Per-aspect UI (status bar / toolbar / tray) already
         * settled at HANDSHAKE_DONE; LOGIN_READY exists purely as
         * the boundary for late post-login RPCs (the files browser's
         * remote provider is the first consumer). Nothing to do
         * here. */
        break;
    }
}

void
gtkhx_connect_signals (GtkhxSession *emitter)
{
    g_signal_connect (emitter, "chat", G_CALLBACK (on_chat_signal), NULL);
    g_signal_connect (emitter, "chat-subject",
                      G_CALLBACK (on_chat_subject_signal), NULL);
    g_signal_connect (emitter, "chat-invitation",
                      G_CALLBACK (on_chat_invitation_signal), NULL);
    g_signal_connect (emitter, "chat-history-batch",
                      G_CALLBACK (on_chat_history_batch_signal), NULL);
    g_signal_connect (emitter, "msg", G_CALLBACK (on_msg_signal), NULL);
    g_signal_connect (emitter, "logged-in", G_CALLBACK (on_logged_in_signal),
                      NULL);
    g_signal_connect (emitter, "self-updated",
                      G_CALLBACK (on_self_updated_signal), NULL);
    g_signal_connect (emitter, "agreement", G_CALLBACK (on_agreement_signal),
                      NULL);
    g_signal_connect (emitter, "news-file", G_CALLBACK (on_news_file_signal),
                      NULL);
    g_signal_connect (emitter, "news-post", G_CALLBACK (on_news_post_signal),
                      NULL);
    g_signal_connect (emitter, "news-folder",
                      G_CALLBACK (on_news_folder_signal), NULL);
    g_signal_connect (emitter, "news-catalog",
                      G_CALLBACK (on_news_catalog_signal), NULL);
    g_signal_connect (emitter, "news-thread",
                      G_CALLBACK (on_news_thread_signal), NULL);
    g_signal_connect (emitter, "user-create",
                      G_CALLBACK (on_user_create_signal), NULL);
    g_signal_connect (emitter, "user-delete",
                      G_CALLBACK (on_user_delete_signal), NULL);
    g_signal_connect (emitter, "user-change",
                      G_CALLBACK (on_user_change_signal), NULL);
    g_signal_connect (emitter, "users-clear",
                      G_CALLBACK (on_users_clear_signal), NULL);
    g_signal_connect (emitter, "gif-icon-data",
                      G_CALLBACK (on_gif_icon_data_signal), NULL);
    g_signal_connect (emitter, "gif-icon-changed",
                      G_CALLBACK (on_gif_icon_changed_signal), NULL);
    g_signal_connect (emitter, "user-info", G_CALLBACK (on_user_info_signal),
                      NULL);
    g_signal_connect (emitter, "file-info", G_CALLBACK (on_file_info_signal),
                      NULL);
    g_signal_connect (emitter, "file-list", G_CALLBACK (on_file_list_signal),
                      NULL);
    g_signal_connect (emitter, "file-update",
                      G_CALLBACK (on_file_update_signal), NULL);
    g_signal_connect (emitter, "xfer-queue", G_CALLBACK (on_xfer_queue_signal),
                      NULL);
    g_signal_connect (emitter, "xfer-destroyed",
                      G_CALLBACK (on_xfer_destroyed_signal), NULL);
    g_signal_connect (emitter, "tracker-server-create",
                      G_CALLBACK (on_tracker_server_create_signal), NULL);
    g_signal_connect (emitter, "tracker-batch-begin",
                      G_CALLBACK (on_tracker_batch_begin_signal), NULL);
    g_signal_connect (emitter, "task-update",
                      G_CALLBACK (on_task_update_signal), NULL);
    g_signal_connect (emitter, "chat-log-line",
                      G_CALLBACK (chat_log_line_handler), NULL);
    g_signal_connect (emitter, "connection-state-changed",
                      G_CALLBACK (on_connection_state_changed_signal), NULL);
}

/* hx_output is gone. Every notification it used to
 * carry is now a signal on GtkhxSession (see gtkhx_session.{c,h}).
 * The two lifecycle hooks (init, loop) only ever had one
 * implementation, so they're called by name from fe_init. */

char **hxd_environ = 0;

int hxd_open_max = 0;
struct hxd_file *hxd_files = 0;

/* qbuf_set / qbuf_add moved to src/qbuf.c so both the GUI binary and
 * the Tier 3 integration harness link the same implementation. */

static RETSIGTYPE
sig_chld (int sig)
{
    int status, serrno = errno;
    pid_t pid;

#ifndef WAIT_ANY
#define WAIT_ANY -1
#endif

    for (;;) {
        pid = waitpid (WAIT_ANY, &status, WNOHANG);
        if (pid < 0) {
            if (errno == EINTR) {
                continue;
            }
            goto ret;
        }
        if (!pid) {
            goto ret;
        }

        hlclient_reap_pid (pid, status);
    }

ret:
    errno = serrno;
}

static RETSIGTYPE
sig_bus (int sig)
{
    /* do something!! */
    abort ();
}

void hotline_client_init (int argc, char **argv);

#if !defined(_SC_OPEN_MAX) && defined(HAVE_GETRLIMIT)
#include <sys/resource.h>
#endif

static RETSIGTYPE
sig_fpe (int sig)
{
    g_error ("SIGFPE (%d)", sig);
    abort ();
}

int
main (int argc, char **argv, char **envp)
{
    struct sigaction act;

    memset (&the_session, 0, sizeof (session));

    /* Defensively clear the test-only TLS-trust escape hatches at the
     * very start of the production entry point, before any connection
     * (and thus any cert decision) can happen. GTKHX_TLS_AUTO_ACCEPT
     * (auto-pin/accept any cert) and GTKHX_TLS_TEST_PROMPT (substitute
     * the TOFU prompt verdict) exist solely for the headless Tier 3
     * harness — which has its own main() and never reaches here. Wiping
     * them means that even if one leaked into a real user's environment,
     * the shipped app still runs the real cert-trust decision (WebPKI /
     * known-hosts / user prompt) and can't be silently tricked into
     * trusting — or auto-rejecting — a server certificate. */
    g_unsetenv ("GTKHX_TLS_AUTO_ACCEPT");
    g_unsetenv ("GTKHX_TLS_TEST_PROMPT");

    /* Set the GLib program name early so GTK 4 picks it up as the
	 * Wayland xdg_toplevel app_id (and the X11 WM_CLASS) on every
	 * window's first commit. The default would be argv[0]
	 * ("gtkhx"), which is not what the .desktop file declares —
	 * compositors look up the icon and group windows by app_id, so
	 * a mismatch leaves the dock with no icon and no .desktop
	 * association.
	 *
	 * Has to happen before any window is created. fe_init() (called
	 * from hotline_client_init below) constructs the toolbar and
	 * the auto-opened windows synchronously, so this must precede
	 * that. The same value is repeated in adw_application_new in
	 * loop() — both call sites have to match for GTK 4 to be
	 * consistent. */
    g_set_prgname ("com.nasledov.gtkhx");
    g_set_application_name ("GtkHx");

#if defined(_SC_OPEN_MAX)
    hxd_open_max = sysconf (_SC_OPEN_MAX);
#elif defined(RLIMIT_NOFILE)
    {
        struct rlimit rlimit;

        if (getrlimit (RLIMIT_NOFILE, &rlimit)) {
            exit (1);
        }
        hxd_open_max = rlimit.rlim_max;
    }
#elif defined(HAVE_GETDTABLESIZE)
    hxd_open_max = getdtablesize ();
#elif defined(OPEN_MAX)
    hxd_open_max = OPEN_MAX;
#else
    hxd_open_max = 16;
#endif
    if (hxd_open_max > FD_SETSIZE) {
        hxd_open_max = FD_SETSIZE;
    }
    hxd_files = g_malloc0 (hxd_open_max * sizeof (struct hxd_file));

    hxd_environ = envp;

    act.sa_handler = SIG_IGN;
    act.sa_flags = 0;
    sigemptyset (&act.sa_mask);
    sigaction (SIGPIPE, &act, 0);
    sigaction (SIGHUP, &act, 0);
    act.sa_handler = sig_fpe;
    sigaction (SIGFPE, &act, 0);
    act.sa_handler = sig_bus;
    sigaction (SIGBUS, &act, 0);
    act.sa_handler = sig_chld;
    act.sa_flags |= SA_NOCLDSTOP;
    sigaction (SIGCHLD, &act, 0);

    hotline_client_init (argc, argv);

    close (0);
    close (1);

    return 0;
}

char *hxd_commands[] = {
    "0wn",    "access", "away", "alert", "broadcast", "color",   "exec",
    "g0away", "maltbl", "mon",  "users", "version",   "visible",
};

int hxdcmd_len = sizeof (hxd_commands) / sizeof (hxd_commands[0]);

static int
is_hxdcmd (char *str)
{
    int i;
    char *ptr;

    if (!str) {
        return 0;
    }

    ptr = strchr (str, ' ');

    for (i = 0; i < hxdcmd_len; i++) {
        int _len = strlen (hxd_commands[i]);
        int len = ptr - str > _len ? _len : ptr - str;

        if (!strncmp (hxd_commands[i], str, len)) {
            return 1;
        }
    }

    return 0;
}

void
hotline_client_input (struct htlc_conn *htlc, char *str, guint32 cid,
                      guint16 style)
{
    if (*str) {
#ifdef USE_PLUGIN
        if (EMIT_SIGNAL (XP_SND_CHAT, sess_from_htlc (htlc), str, &cid, 0, 0, 0) == 1) {
            return;
        }
#endif

        if (*str == '/' && *++str && *str != '/') {
            if (is_hxdcmd (str)) {
                str--;
                hx_send_chat (htlc, str, cid, style);
            } else {
                hx_command (str, cid);
            }
        } else {
            hx_send_chat (htlc, str, cid, style);
        }
    }
}

static void
get_password (char *buf)
{
    size_t i;
    struct termios termio;
    struct termios tmp;

    tcgetattr (0, &termio);
    tmp.c_lflag = termio.c_lflag;
    termio.c_lflag = (ISIG | ICANON);
    tcsetattr (0, TCSANOW, &termio);

    /* The knights who say "nee" demand a..
	   SHRUBBERY! */
    printf ("Password: ");
    if (!fgets (buf, 128, stdin)) {
        /* EOF or read error on stdin (e.g. tty closed mid-prompt).
		 * Restore the terminal flags before returning so the
		 * caller's shell isn't left with echo disabled. */
        buf[0] = '\0';
        tcsetattr (0, TCSANOW, &termio);
        return;
    }
    printf ("\n");
    termio.c_lflag = tmp.c_lflag;
    tcsetattr (0, TCSANOW, &termio);

    for (i = 0; i < strlen (buf); i++) {
        if (buf[i] == '\n') {
            buf[i] = '\0';
            break;
        }
    }
}

static void
print_help (char *name)
{
    printf (_ ("GtkHx %s, Copyright (C) 2000-2026 Misha Nasledov\n"), VERSION);
    printf (_ ("GtkHx comes with ABSOLUTELY NO WARRANTY.\n"));
    printf (_ ("This is free software, and you are welcome\n"));
    printf (_ ("to redistribute it under certain conditions.\n\n"));

    printf (_ ("usage: %s [options]\n"), name);
    printf (_ ("\nsupported options:\n"));
    printf (_ ("\t--help, -h\t\tPrint this help message out.\n"));
    printf (_ ("\t--server, -s <host>\tConnect to <host>\n"));
    printf (_ ("\t--port, -t <port>\tConnect to <host> on <port>. "
               "(default: 5500)\n"));
    printf (_ ("\t--login, -l <login>\tUse <login> for <host>. "
               "(default: guest)\n"));
    printf (_ ("\t--pass, -p\t\tPrompt for pass of <login>.\n"));
    printf (_ ("\t--bookmark, -b <name>\tConnect using bookmark <name>.\n"));
}

static struct option hx_options[]
    = { { "server", 1, 0, 's' }, { "help", 0, 0, 'h' },
        { "port", 1, 0, 't' },   { "login", 1, 0, 'l' },
        { "pass", 0, 0, 'p' },   { "bookmark", 1, 0, 'b' },
        { 0, 0, 0, 0 } };

extern void connect_bookmark_name (char *name);

void
hotline_client_init (int argc, char **argv)
{
    char *home, *user;
    struct passwd *pwe;
    char opt_char;
    int index = 0;
    char *server = 0;
    char *login = 0;
    char *pass = 0;
    char *bookmark = 0;
    int prompt_pass = 0;
    guint16 port = 5500;

    optind = 0;
    if (argc > 1) {
        while ((opt_char
                = getopt_long (argc, argv, "s:ht:pl:b:", hx_options, &index))
               != -1) {
            if (opt_char == 0) {
                opt_char = hx_options[index].val;
            }
            switch (opt_char) {
            case 't':
                if (optarg) {
                    port = strtoul (optarg, 0, 0);
                }
                break;
            case 's':
                if (optarg) {
                    server = g_strdup (optarg);
                    if (bookmark) {
                        g_free (bookmark);
                    }
                }
                break;
            case 'h':
                print_help (argv[0]);
                exit (0);
                break;
            case 'l':
                if (optarg) {
                    login = g_strdup (optarg);
                }
                break;
            case 'p':
                prompt_pass = 1;
                break;

            case 'b':
                if (optarg) {
                    bookmark = g_strdup (optarg);
                    if (server) {
                        g_free (server);
                    }
                    if (login) {
                        g_free (login);
                    }
                }
                break;
            }
        }
    }
    home = getenv ("HOME");
    user = getenv ("USER");
    if (!home || !user) {
        pwe = getpwuid (getuid ());
        if (!pwe) {
            hx_printf_prefix (&the_session.htlc, 0, INFOPREFIX, "getpwuid: %s",
                              strerror (errno));
        } else {
            if (!home) {
                home = pwe->pw_dir;
            }
            if (!user) {
                user = pwe->pw_name;
            }
        }
    }

    memset (&the_session.htlc, 0, sizeof (struct htlc_conn));
    the_session.htlc.icon = 500;
    if (user) {
        strncpy (the_session.htlc.name, user, 31);
        the_session.htlc.name[31] = '\0';
    } else {
        strcpy (the_session.htlc.name, "Evaluation 0wn3r");
    }

    gen_command_hash ();

    last_msg_nick[0] = 0;

    init (argc, argv);

    if (server) {
        if (prompt_pass) {
            pass = g_malloc (128);
            get_password (pass);
        }
        /* CLI --server bootstrap: tls=0 default. GTKHX_TLS=1 env-var
		 * override applies (Phase 4 adds a --tls CLI flag). */
        hx_connect (&the_session.htlc, server, port, login ? login : "guest",
                    pass ? pass : "", 0, /*tls=*/0);
        g_free (server);
        g_free (login);
        g_free (pass);
    } else if (bookmark) {
        connect_bookmark_name (bookmark);
        g_free (bookmark);
    }

    loop ();
}
