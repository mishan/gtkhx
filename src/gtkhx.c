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
#include <pwd.h>
#include <getopt.h>
#include "hx.h"
#include "gtk_hlist.h"
#include "macres.h"
#include "cicn.h"
#include "news.h"
#include "news15.h"
#include "news_browser.h"
#include "files_remote_provider.h"
#include "users.h"
#include "files.h"
#include "tasks.h"
#include "network.h"
#include "gtkutil.h"
#include "debug.h"
#include "toolbar.h"
#include "chat.h"
#include "msg.h"
#include "gtkhx_session.h"
#include "notify.h"
#include "tracker.h"
#include "tray.h"
#include "xtext.h"
#include "gtkthreads.h"
#include "options.h"
#include "toolbar.h"
#include "xfers.h"
#include "plugin.h"
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

/*
 * Phase 3.5: gtk_widget_modify_font/text/base were deprecated in GTK 3.0
 * and removed in GTK 4. Replace them with a single screen-wide
 * GtkCssProvider whose body is rebuilt whenever the font or colors
 * change; the per-widget step then collapses to "tag the widget with
 * a class name so the provider's selector matches."
 *
 * The provider is attached at GTK_STYLE_PROVIDER_PRIORITY_APPLICATION,
 * which beats the user's theme but sits below !important inline rules.
 * That matches the precedence the old gtk_widget_modify_* family had.
 */
static GtkCssProvider *gtkhx_css_provider = NULL;
static GtkCssProvider *gtkhx_userlist_css_provider = NULL;

static void
ensure_provider_attached (GtkCssProvider *prov)
{
    /* Phase 4.4: GdkScreen / add_provider_for_screen are gone in GTK 4.
	 * Attach to the default GdkDisplay instead — under Wayland there is
	 * no per-screen partition anyway. */
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

    if (!gtkhx_css_provider) {
        gtkhx_css_provider = gtk_css_provider_new ();
        ensure_provider_attached (gtkhx_css_provider);
    }

    fontprops = pango_to_css_props (gtkhx_font_desc);

    /* The .gtkhx-text rule covers GtkEntry / GtkLabel / etc. directly,
	 * and the descendant ".gtkhx-text text" rule reaches GtkTextView's
	 * inner "text" CSS node so the input area picks up the same font.
	 *
	 * We deliberately do NOT set color / background-color / caret-color
	 * here. The early Phase-5 version did, copying fg_col / bg_col from
	 * the xtext output styling onto the chat input, subject entry,
	 * private-message editor, and news viewer — which forced light-grey-
	 * on-black on those widgets regardless of the user's Light / Dark
	 * theme choice. xtext's chat-output area still uses fg_col / bg_col
	 * directly (it draws its own contents with cairo), so the in-chat
	 * line styling is unaffected; only the surrounding input / subject /
	 * news widgets follow the system theme now, which is what the user
	 * expects. */
    css = g_strdup_printf (".gtkhx-text, .gtkhx-text text {"
                           "  %s"
                           "}",
                           fontprops);

    /* Phase 4.13: gtk_css_provider_load_from_data is deprecated in
	 * GTK 4.12 in favor of gtk_css_provider_load_from_string (which
	 * takes a NUL-terminated str rather than (str, len)). Parse errors
	 * still come via the `parsing-error` signal — for these hardcoded
	 * strings a parse failure is a developer bug, not a runtime issue
	 * we need to log. */
    gtk_css_provider_load_from_string (gtkhx_css_provider, css);

    g_free (css);
    g_free (fontprops);
}

void
gtkhx_refresh_userlist_css (PangoFontDescription *fd)
{
    gchar *fontprops;
    gchar *css;

    if (!gtkhx_userlist_css_provider) {
        gtkhx_userlist_css_provider = gtk_css_provider_new ();
        ensure_provider_attached (gtkhx_userlist_css_provider);
    }

    fontprops = pango_to_css_props (fd);
    css = g_strdup_printf (".gtkhx-userlist { %s }", fontprops);

    gtk_css_provider_load_from_string (gtkhx_userlist_css_provider, css);

    g_free (css);
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

    /* Phase 4.5: gtk_style_context_add_class is deprecated in GTK 4.10
	 * and was the source of a gtk_css_node_insert_after assertion when
	 * adding the class on a widget whose CSS node hadn't been parented
	 * yet. gtk_widget_add_css_class is the modern, safer path. */
    if (!gtk_widget_has_css_class (w, "gtkhx-text")) {
        gtk_widget_add_css_class (w, "gtkhx-text");
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
static GtkWidget *agreetext;
static struct timer *timer_list;

static int rinput_tags[1024];
static int winput_tags[1024];

const char *INFOPREFIX = " \00310[\00303hx\00310]\003 ";

session the_session;

/* Forward declaration of the application object owned by loop(). hx_quit()
 * needs to call g_application_quit on it, but loop() comes much later
 * in this TU and gtkhx_app's definition lives there.
 *
 * Phase 5: AdwApplication subclasses GtkApplication, so the existing
 * gtk_application_* call sites keep working unchanged. The upgrade
 * gives us libadwaita's AdwStyleManager (color-scheme tracking,
 * Adwaita stylesheet) for free. */
static AdwApplication *gtkhx_app;

/* Phase 5: the dialog code paths in gtkutil.c (error_dialog) and
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

/* Phase 5 settings management:
 *
 * Resolve the per-user config root, in priority order:
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

/*
 * Phase 3.x: capture each toplevel's final position into prefs before
 * prefs_write.
 *
 * Wayland caveat: the protocol deliberately does not expose absolute
 * toplevel positions to clients, and gtk_window_move is a documented
 * no-op there too. gtk_window_get_position therefore returns (0, 0)
 * on every Wayland session, so saving it would overwrite legitimately-
 * useful X11-era prefs with garbage. Detect Wayland by GdkDisplay
 * type name and skip the position save entirely on that backend —
 * size persistence still works because gtk_window_get_size returns
 * the real current allocation on both backends.
 *
 * On X11 / Quartz the read is honest, so the captured values match
 * what the user actually lived with by quit time.
 */
static gboolean
gtkhx_backend_supports_position (void)
{
    GdkDisplay *display = gdk_display_get_default ();
    const char *name;

    if (!display) {
        return TRUE;
    }
    name = G_OBJECT_TYPE_NAME (display);
    if (name && g_str_has_prefix (name, "GdkWayland")) {
        return FALSE;
    }
    return TRUE;
}

/* Phase 4.5: also capture size at quit. Previously the configure-event
 * handlers in chat/users/tasks/news did the per-resize size save and
 * this function only did position. GTK 4 has no configure-event on
 * widgets, so we move size into the quit-time pass too. */
static void
save_geo (GtkWidget *w, Window_Geo *geo, gboolean save_pos_too)
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
    (void)save_pos_too; /* position save is no longer possible client-side
	                       * under Wayland, and gtk_window_get_position is
	                       * gone entirely in GTK 4 — keep the existing
	                       * prefs values from a prior X11 session. */
}

static void
gtkhx_save_window_positions (void)
{
    gboolean want_pos = gtkhx_backend_supports_position ();

    save_geo (toolbar_window, &gtkhx_prefs.geo.tool, want_pos);
    save_geo (the_session.chat_window, &gtkhx_prefs.geo.chat, want_pos);
    save_geo (the_session.users_window, &gtkhx_prefs.geo.users, want_pos);
    save_geo (the_session.tasks_window, &gtkhx_prefs.geo.tasks, want_pos);
    save_geo (the_session.news_window, &gtkhx_prefs.geo.news, want_pos);
}

void
hx_quit (void)
{
    gtkhx_save_window_positions ();
    prefs_write ();
    xfers_delete_all ();
    tracker_kill_threads ();

    if (the_session.htlc.fd) {
        hx_htlc_close (&the_session.htlc, 1);
    }

#if 0 /* XXX */
	close_logs();
#endif

    /* Phase 3.6: g_application_quit() ends g_application_run() in loop().
	 * Phase 4.x: gtk_main_quit() is gone in GTK 4 — there is no main-loop
	 * fallback. If hx_quit fires before the GtkApplication is constructed
	 * we just fall through to exit(). */
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

    for (timer = timer_list; timer; timer = timer->prev) {
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
        hx_printf_prefix (&the_session.htlc, 0, INFOPREFIX,
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
        hx_printf_prefix (&the_session.htlc, 0, INFOPREFIX,
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
    /* Phase 3.10: GdkRGBA all the way down. The GTK 1.2-era manual
	 * .pixel computation is gone for good — paletted colormaps have
	 * been gone since GTK 3, and cairo + Pango consume the float
	 * channels directly. Initializer literals via the RGB16 macro
	 * preserve the historic 16-bit channel values used by the rest
	 * of the codebase. */
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

/* Phase 5: scan a directory for *.rsrc files and append each one to
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

    /* Phase 5: build the list of icon resource files from auto-discovery
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

extern void reinit_gtktexts (session *sess);

static void
fe_init (void)
{
    GtkWidget *widg = gtk_button_new ();

    generate_colors (widg);
    gtkhx_widget_destroy (widg);
    init_variables ();

    memset (&icon_files, 0, sizeof (icon_files));
    prefs_read ();
    /* Phase 3.5: prep the screen-wide CSS provider once prefs are loaded
	 * so the very first widget that gets gtkhx_apply_text_style() picks
	 * up the right look on the first paint. */
    gtkhx_refresh_css ();
    init_icons ();

    /* initialize some pointers with linked list
	   this will be handled somewhere else in case of
	   multiconnection support */

    /* Phase 5+: hashtable-backed session collections. chats_init
	 * additionally seeds the table with the public chat (cid=0),
	 * which must always exist while the table does. */
    chats_init (&the_session);
    gchats_init (&the_session);
    tasks_init (&the_session);
    msg_windows_init (&the_session);

    /* Phase 3+: connect the view-side handlers to the
	 * GtkhxSession signal emitter. As Phase 3 progresses, each
	 * vtable notification gets a g_signal_connect line here and
	 * the corresponding output_functions member goes away. The
	 * signal-handler signature (instance + signal args + user_data,
	 * with guint16 widened to guint by the marshaller) doesn't
	 * match the legacy vtable-function signatures exactly, so we
	 * route each one through a small static adapter declared
	 * below — `on_*_signal`. */
    gtkhx_connect_signals (gtkhx_session_get_default ());

    create_toolbar_window (&the_session);
    init_colors (toolbar_window);

    create_chat (&the_session);
    if (gtkhx_prefs.geo.chat.init == 1) {
        create_chat_window (0, &the_session);
    }
    if (gtkhx_prefs.geo.news.init == 1) {
        create_news_window (&the_session);
    }
    if (gtkhx_prefs.geo.users.init == 1) {
        create_users_window (0, &the_session);
    }
    create_tasks (&the_session);
    if (gtkhx_prefs.geo.tasks.init == 1) {
        create_tasks_window (0, &the_session);
    }

    reinit_gtktexts (&the_session);
}

/*
 * Phase 3.6: drive the main loop through a GtkApplication instead of
 * a bare gtk_main(). gtk_main() is gone in GTK 4; GtkApplication is the
 * portable replacement and gives us a well-defined "active" lifecycle,
 * a primary window registration, and a place to hang DBus actions
 * later.
 *
 * The existing two-step init/loop split is preserved deliberately:
 * fe_init() (UI construction) and the optional CLI auto-connect that
 * happens between them in hotline_client_init() both run BEFORE we
 * enter the loop, so by the time gtkhx_activate fires the toolbar
 * window already exists. The activate handler just registers it with
 * the app so closing it terminates g_application_run cleanly.
 */
/* AdwStyleManager::notify::dark trampoline — reads the new dark
 * state off the manager and pushes it into the xtext palette plus
 * every live chat-output widget. Connected once from
 * gtkhx_activate. */
static void
on_style_manager_dark_changed (GObject *object, GParamSpec *pspec,
                               gpointer user_data)
{
    AdwStyleManager *sm = ADW_STYLE_MANAGER (object);
    (void)pspec;
    (void)user_data;
    gtkhx_apply_theme_palette (adw_style_manager_get_dark (sm));
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

    /* Phase 5: register the toolbar's hamburger-menu actions
	 * (app.settings / app.about / app.quit). The actions can't be
	 * added during create_toolbar_window because that runs in
	 * fe_init() — before g_application_run, so before the
	 * AdwApplication exists. Now that we're in the activate handler
	 * the application is alive, so wire them in. */
    toolbar_register_actions (G_APPLICATION (app), &the_session);

    /* Phase 5: bind Ctrl+Q (and Ctrl+K) to GApplication actions so the
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

    /* Phase 5+: StatusNotifierItem tray icon. Reads the TRAY pref at
	 * register-time; the changed_tray cfgvar callback flips it on/off
	 * as the user toggles the Setting. */
    gtkhx_tray_init (app);

    /* Phase 5+: desktop notifications. Each event-class entry
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
	 * gtkhx_apply_theme_palette to refresh every open xtext. */
    {
        AdwStyleManager *sm = adw_style_manager_get_default ();
        gtkhx_apply_theme_palette (adw_style_manager_get_dark (sm));
        g_signal_connect (sm, "notify::dark",
                          G_CALLBACK (on_style_manager_dark_changed), NULL);
    }
}

static void
loop (void)
{
    /* Phase 5: AdwApplication wraps GtkApplication and additionally
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

    /* Phase 5: parse the GTKHX_DEBUG env var into the categorised
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
    /* Phase 3.3: gdk_threads_init() is gone in GTK 4 and deprecated since
	 * GTK 3.6. The worker threads still need a serializing lock against
	 * the main thread; gtkthreads.c now provides one via GRecMutex +
	 * a custom GMainContext poll function (see gtkthreads.c).
	 * Phase 4.x: gtk_init() in GTK 4 takes no arguments — argc/argv
	 * parsing is the application's job (we don't use any GTK-owned flags
	 * anyway). */
    gtk_init ();
    fe_init ();
}

static void
output_user_info (guint16 uid, const char *nam, const char *info, guint16 len)
{
    if (len > 0) {
        GtkWidget *info_window;
        GtkWidget *info_text;
        GtkWidget *info_scroll;
        GtkTextBuffer *info_buf;
        char infotitle[45];

        info_window = gtk_window_new ();
        gtk_widget_set_size_request (info_window, 260, 250);

        g_snprintf (infotitle, sizeof (infotitle), _ ("User Info: %s (%u)"),
                    nam, uid);
        gtk_window_set_title (GTK_WINDOW (info_window), infotitle);

        info_text = gtk_text_view_new ();
        gtk_text_view_set_editable (GTK_TEXT_VIEW (info_text), FALSE);
        gtk_text_view_set_cursor_visible (GTK_TEXT_VIEW (info_text), FALSE);
        info_buf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (info_text));
        gtk_text_buffer_set_text (info_buf, info, len);

        info_scroll = gtk_scrolled_window_new ();
        gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (info_scroll),
                                        GTK_POLICY_AUTOMATIC,
                                        GTK_POLICY_AUTOMATIC);
        gtkhx_widget_set_child (info_scroll, info_text);
        gtkhx_widget_set_child (info_window, info_scroll);

        init_keyaccel (info_window);
        gtk_window_present (GTK_WINDOW (info_window));
    }
}

/* Forward declarations for the file-local view functions the
 * Phase 3 adapter handlers below call. The functions themselves
 * are defined later in this file; the extern-linkage ones
 * (output_chat_subject, output_news_*, msg_output, etc.) are
 * already declared in their respective headers.
 *
 * Phase 5+ (chat-event refactor): the chat-signal path bypasses
 * the gtkhx.c output_chat stub — the renderer is
 * chat.c::output_chat_from_event, which takes the pre-parsed
 * HxChatEvent directly. */
static void output_agreement (session *sess, const char *agreement,
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
                              guint cid, gpointer entries_p,
                              gboolean has_more, gpointer user_data)
{
    GPtrArray *entries = entries_p;
    (void) emitter;
    (void) user_data;
    output_chat_history_batch (htlc, (guint32) cid, entries, has_more);
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

static void
on_agreement_signal (GtkhxSession *emitter, gpointer sess, gpointer agreement,
                     guint len, gpointer user_data)
{
    (void)emitter;
    (void)user_data;
    output_agreement ((session *)sess, (const char *)agreement, (guint16)len);
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
                       guint icon, guint color, gpointer user_data)
{
    (void)emitter;
    (void)user_data;
    user_create (htlc, chat, user, (const char *)nam, (guint16)icon,
                 (guint16)color);
}

static void
on_user_delete_signal (GtkhxSession *emitter, struct htlc_conn *htlc,
                       struct chat *chat, struct hx_user *user,
                       gpointer user_data)
{
    (void)emitter;
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
    /* Phase 5: only the new files browser remains; route the
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
on_tracker_server_create_signal (GtkhxSession *emitter, guint addr_u32,
                                 guint port, guint nusers, gpointer nam,
                                 gpointer desc, gint total, gpointer user_data)
{
    struct in_addr addr;
    (void)emitter;
    (void)user_data;
    addr.s_addr = (in_addr_t)addr_u32;
    tracker_server_create (addr, (guint16)port, (guint16)nusers,
                           (const char *)nam, (const char *)desc, total);
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
    session *sess = &the_session;
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
    g_signal_connect (emitter, "task-update",
                      G_CALLBACK (on_task_update_signal), NULL);
    g_signal_connect (emitter, "chat-log-line",
                      G_CALLBACK (chat_log_line_handler), NULL);
    g_signal_connect (emitter, "connection-state-changed",
                      G_CALLBACK (on_connection_state_changed_signal), NULL);
}

static void
concurrence (GtkWidget *widget, gpointer data)
{
    session *sess = data;
    (void)widget;

    /* Phase 5+: deliver NAME + ICON to the server when the user
	 * clicks Agree. AGREEMENTAGREE carries both — same payload
	 * regardless of whether login was already auto-completed (1.9-
	 * style: SELFINFO before AGREEMENT) or gated on this very click
	 * (mhxd-style: AGREEMENT before SELFINFO).
	 *
	 *   - mhxd-style: the server calls finish_login from inside
	 *     rcv_agreementagree, completing login here.
	 *   - 1.9-style: flags.in_login on the server side is already
	 *     0 by this point, so finish_login is skipped — but the
	 *     server still reads NAME + ICON from the chunks (sets us
	 *     under the right nick) AND emits HTLS_HDR_BANNER from the
	 *     same handler on banner-configured servers.
	 *
	 * Earlier code split this into AGREEMENTAGREE vs. USER_CHANGE
	 * gated on flags.logged_in, on the theory that 1.9 servers
	 * disconnect on AGREEMENTAGREE for an already-logged-in session.
	 * Re-reading mhxd's rcv_agreementagree, that diagnosis was almost
	 * certainly wrong — there's no disconnect path on already-logged-
	 * in. The split was also breaking banner delivery on every 1.9
	 * server, because no AGREEMENTAGREE means no banner trigger. */
    if (sess->htlc.fd) {
        hx_send_agreement_agree (&sess->htlc);
    }

    gtkhx_widget_destroy (sess->agreementwin);
    sess->agreementwin = 0;
}

static void
disagreement (GtkWidget *widget, gpointer data)
{
    session *sess = data;

    if (sess->htlc.fd) {
        hx_htlc_close (&sess->htlc, 1);
    }
}

static void
output_agreement (session *sess, const char *agreement, guint16 len)
{
    GtkWidget *agreementwin;
    GtkWidget *agreebtn;
    GtkWidget *disagreebtn;
    GtkWidget *vbox;
    GtkWidget *hbox;
    GtkWidget *agree_scroll;
    GtkTextBuffer *agree_buf;

    agreementwin = gtk_window_new ();
    /* Phase 5: AdwHeaderBar across all GtkHx windows for visual
	 * consistency (chat, news, files, tasks, users, preview, this). */
    gtk_window_set_titlebar (GTK_WINDOW (agreementwin), adw_header_bar_new ());
    gtk_window_set_default_size (GTK_WINDOW (agreementwin), 460, 540);
    gtk_window_set_title (GTK_WINDOW (agreementwin), _ ("Agreement"));

    agreetext = gtk_text_view_new ();
    gtk_text_view_set_editable (GTK_TEXT_VIEW (agreetext), FALSE);
    gtk_text_view_set_cursor_visible (GTK_TEXT_VIEW (agreetext), FALSE);
    gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (agreetext), GTK_WRAP_WORD);
    gtk_text_view_set_left_margin (GTK_TEXT_VIEW (agreetext), 12);
    gtk_text_view_set_right_margin (GTK_TEXT_VIEW (agreetext), 12);
    gtk_text_view_set_top_margin (GTK_TEXT_VIEW (agreetext), 12);
    gtk_text_view_set_bottom_margin (GTK_TEXT_VIEW (agreetext), 12);
    agree_buf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (agreetext));
    gtk_text_buffer_set_text (agree_buf, agreement, len);

    agree_scroll = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (agree_scroll),
                                    GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtkhx_widget_set_child (agree_scroll, agreetext);

    /* Phase 5: Adwaita action-class buttons. .suggested-action paints
	 * Agree in the accent colour, .destructive-action paints Disagree
	 * red — the visual treatment HIG-compliant Adwaita apps use for
	 * a primary / dismissive button pair. */
    agreebtn = gtk_button_new_with_label (_ ("Agree"));
    gtk_widget_add_css_class (agreebtn, "suggested-action");
    gtk_widget_add_css_class (agreebtn, "pill");

    disagreebtn = gtk_button_new_with_label (_ ("Disagree"));
    gtk_widget_add_css_class (disagreebtn, "destructive-action");
    gtk_widget_add_css_class (disagreebtn, "pill");

    vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
    /* Right-align the button row, with margins for breathing room.
	 * The previous layout packed both buttons left-aligned and
	 * flush against each other — visually cramped and not where
	 * users expect dialog buttons. */
    gtk_widget_set_halign (hbox, GTK_ALIGN_END);
    gtk_widget_set_margin_start (hbox, 12);
    gtk_widget_set_margin_end (hbox, 12);
    gtk_widget_set_margin_top (hbox, 12);
    gtk_widget_set_margin_bottom (hbox, 12);

    g_signal_connect (agreebtn, "clicked", G_CALLBACK (concurrence), sess);
    g_signal_connect (disagreebtn, "clicked", G_CALLBACK (disagreement), sess);

    gtkhx_widget_set_child (agreementwin, vbox);
    gtkhx_box_pack (vbox, agree_scroll, TRUE, TRUE, 0);
    gtkhx_box_pack (vbox, hbox, FALSE, FALSE, 0);
    /* Disagree on the left, Agree on the right (HIG-conventional;
	 * the affirmative action sits where Enter lives). */
    gtkhx_box_pack (hbox, disagreebtn, FALSE, FALSE, 0);
    gtkhx_box_pack (hbox, agreebtn, FALSE, FALSE, 0);

    /* Phase 5: pressing Enter activates Agree. The text view is
	 * read-only so it doesn't consume Return; setting the default
	 * widget here is what makes Enter trigger the affirmative
	 * action when nothing else has focus. */
    gtk_widget_set_receives_default (agreebtn, TRUE);
    gtk_window_set_default_widget (GTK_WINDOW (agreementwin), agreebtn);

    init_keyaccel (agreementwin);
    gtk_window_present (GTK_WINDOW (agreementwin));
    sess->agreementwin = agreementwin;
}

/* Phase 3.6: hx_output is gone. Every notification it used to
 * carry is now a signal on GtkhxSession (see gtkhx_session.{c,h}).
 * The two lifecycle hooks (init, loop) only ever had one
 * implementation, so they're called by name from fe_init. */

char **hxd_environ = 0;

int hxd_open_max = 0;
struct hxd_file *hxd_files = 0;

/* qbuf_set / qbuf_add moved to src/qbuf.c so both the GUI binary and
 * the Tier 3 integration harness link the same implementation. */

extern void hlclient_reap_pid (pid_t pid, int status);

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
        if (EMIT_SIGNAL (XP_SND_CHAT, &the_session, str, &cid, 0, 0, 0) == 1) {
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
        hx_connect (&the_session.htlc, server, port, login ? login : "guest",
                    pass ? pass : "", 0);
        g_free (server);
        g_free (login);
        g_free (pass);
    } else if (bookmark) {
        connect_bookmark_name (bookmark);
        g_free (bookmark);
    }

    loop ();
}
