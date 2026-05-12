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
#include <gtk/gtk.h>
#include <adwaita.h>
#include <netinet/in.h>
#include "hx.h"
#include "network.h"
#include "news.h"
#include "news15.h"
#include "xfers.h"
#include "gtkutil.h"
#include "text_util.h"
#include "tracker.h"
#include "tray.h"
#include "gtkhx.h"
#include "users.h"
#include "chat.h"
#include "tasks.h"
#include "options.h"
#include "connect.h"
#include "files.h"
#include "usermod.h"
#include "about.h"
#include "banner.h"
#include "options.h"
#include "gtkthreads.h"
#include "plugin.h"
#include "toolbar.h"

GtkWidget *toolbar_window, *files_btn, *connect_btn, *post_btn;
GtkWidget *disconnect_btn, *news15_btn, *news_btn;

#ifdef USE_PLUGIN
GtkWidget *plugin_btn;
#endif

/* Phase 5: status_bar is now a GtkLabel. The previous GtkStatusbar
 * was deprecated in GTK 4.10 and we never used its stack-of-messages
 * model — we always replaced the message wholesale. set_status_bar()
 * in gtkutil.c does a single gtk_label_set_text() now. The
 * status_msg / context_status globals are gone with it. */
GtkWidget *status_bar;

/* Phase 5: AdwToastOverlay anchors transient notifications over the
 * toolbar content. set_status_bar() pushes a toast for "Logged in"
 * to give positive feedback at connection time; the persistent
 * status label still shows the current state for ambient awareness.
 * Static — only the toolbar.c-internal wiring touches it directly,
 * external callers go through toolbar_show_toast(). */
static AdwToastOverlay *toolbar_toast;

/* Phase 5: AdwBanner sits above the content row and surfaces
 * connection-issue state that needs user action — typically "Lost
 * connection — Reconnect" after an unexpected disconnect. Hidden by
 * default; toolbar_show_connection_lost / toolbar_hide_banner toggle
 * the "revealed" property. The banner button reconnects to the same
 * server we just lost — connect_reconnect_last replays the cached
 * params from connect.c without showing the dialog (one-click
 * reconnect after a network blip). The cache falls back to opening
 * the dialog if it's empty. */
static AdwBanner *toolbar_banner;

static void
on_banner_button_clicked (AdwBanner *banner, gpointer user_data)
{
	(void) banner; (void) user_data;
	if (toolbar_banner)
		adw_banner_set_revealed (toolbar_banner, FALSE);
	connect_reconnect_last ();
}

/* Phase 5: New User / Edit User used to be standalone toolbar
 * buttons. They're admin actions used rarely (only by sysops),
 * so they fold into an Admin submenu in the hamburger. The
 * GActions below carry the same dispatch the old click handlers
 * did; their enabled state is toggled in setbtns() to mirror
 * the historic per-button sensitivity. */
static void
on_action_user_new (GSimpleAction *action, GVariant *param, gpointer user_data)
{
	(void) action; (void) param; (void) user_data;
	create_useredit_window (0, 1);
}

static void
on_action_user_edit (GSimpleAction *action, GVariant *param, gpointer user_data)
{
	(void) action; (void) param; (void) user_data;
	useredit_open_dialog ();
}

/* Phase 5: app.open_bookmark fires from the AdwSplitButton's
 * dropdown menu. The GVariant parameter carries the bookmark
 * name as a string; connect_open_bookmark_by_name reads the
 * file and dispatches to the existing connect path. */
static void
on_action_open_bookmark (GSimpleAction *action, GVariant *param,
                         gpointer user_data)
{
	const char *name;
	(void) action; (void) user_data;

	if (!param || !g_variant_is_of_type (param, G_VARIANT_TYPE_STRING))
		return;
	name = g_variant_get_string (param, NULL);
	connect_open_bookmark_by_name (name);
}

/* Phase 5: app.connect_builtin fires from the SplitButton's
 * dropdown for one of the hardcoded "well-known" Hotline servers.
 * Index 1..4 — same numbering the connect dialog's built-in combo
 * has used since forever. */
static void
on_action_connect_builtin (GSimpleAction *action, GVariant *param,
                           gpointer user_data)
{
	(void) action; (void) user_data;

	if (!param || !g_variant_is_of_type (param, G_VARIANT_TYPE_INT32))
		return;
	connect_open_builtin_bookmark (g_variant_get_int32 (param));
}

/* Phase 4.5: GTK 4 close-request signature is (GtkWindow *, gpointer)
 * returning gboolean. Returning TRUE inhibits the default destroy.
 *
 * Phase 5+: when the tray icon is enabled AND a tray host is around
 * to render it, X-button closes hide all windows instead of
 * quitting — the conventional "minimize-to-tray" pattern. We require
 * an actual host (not just the pref) so users with the toggle on
 * but no AppIndicator extension installed don't end up unable to
 * exit the app. Without an SNI host, fall through to hx_quit() as
 * before. */
static gboolean close_toolbar_window (GtkWindow *window, gpointer data)
{
	(void) window; (void) data;
	if (gtkhx_tray_is_enabled () && gtkhx_tray_host_available ()) {
		gtkhx_tray_hide_all_windows ();
		return TRUE;     /* inhibit destroy */
	}
	hx_quit();
	return FALSE;
}

void disconnect_clicked (void)
{
	if(!connected) {
		kill_threads();
		setbtns(&the_session, 0);
		set_status_bar(0);
		set_disconnect_btn(&the_session, 0);
		conn_task_update(&the_session, 2);
		if(the_session.htlc.gdk_input) {
			hxd_fd_clr(the_session.htlc.fd, FDR|FDW);
			close(the_session.htlc.fd);
			the_session.htlc.gdk_input = 0;
		}
		hx_printf_prefix(&the_session.htlc, 0, INFOPREFIX, "%s: %s\n", server_addr, _("connection closed"));
	}

	else if (the_session.htlc.fd) {
		hx_htlc_close(&the_session.htlc, 1);
	}
}

/* Phase 5: app-level GActions backing the AdwHeaderBar's hamburger
 * menu. The actions just dispatch to the existing window-creation
 * functions so the menu items stay one g_action_map_add_action_entries
 * call away from working alongside the historic toolbar buttons. The
 * GAction approach also means the menu items pick up keyboard
 * accelerators for free if we ever wire any (gtk_application_set_accels_for_action).
 *
 * The action callbacks defer their real work to g_idle_add — when the
 * action fires from a hamburger menu item, the popover is mid-dismiss
 * and the GdkSurface for it is still alive on the click stack. Building
 * a new top-level dialog (especially the AdwPreferencesWindow with its
 * 9 pages and the icon-picker GtkHList that walks main-loop iterations)
 * inside that callstack hit a Heisenbug — segfault on bare run, no
 * crash under gdb. Letting the click chain unwind first via the idle
 * source side-steps it cleanly. */
static gboolean
defer_open_settings (gpointer data)
{
	create_options_window (NULL, data);
	return G_SOURCE_REMOVE;
}

static gboolean
defer_open_about (gpointer data)
{
	(void) data;
	create_about_window (NULL, NULL);
	return G_SOURCE_REMOVE;
}

static gboolean
defer_quit (gpointer data)
{
	(void) data;
	hx_quit ();
	return G_SOURCE_REMOVE;
}

static void
on_action_settings (GSimpleAction *action, GVariant *param, gpointer user_data)
{
	(void) action; (void) param;
	g_idle_add (defer_open_settings, user_data);
}

static void
on_action_about (GSimpleAction *action, GVariant *param, gpointer user_data)
{
	(void) action; (void) param; (void) user_data;
	g_idle_add (defer_open_about, NULL);
}

static void
on_action_quit (GSimpleAction *action, GVariant *param, gpointer user_data)
{
	(void) action; (void) param; (void) user_data;
	g_idle_add (defer_quit, NULL);
}

static const GActionEntry app_actions[] = {
	{ .name = "settings",        .activate = on_action_settings        },
	{ .name = "about",           .activate = on_action_about           },
	{ .name = "user_new",        .activate = on_action_user_new        },
	{ .name = "user_edit",       .activate = on_action_user_edit       },
	{ .name = "open_bookmark",   .activate = on_action_open_bookmark,
	  .parameter_type = "s" },
	{ .name = "connect_builtin", .activate = on_action_connect_builtin,
	  .parameter_type = "i" },
	{ .name = "quit",            .activate = on_action_quit            },
};

/* Phase 5: push a transient AdwToast onto the toolbar window's
 * AdwToastOverlay. No-op until the toolbar is built (toolbar_toast
 * starts NULL), so callers can safely fire toasts during early
 * startup without guarding. The toast auto-dismisses after libadwaita's
 * default timeout (~5s); duplicate text replaces the previous toast
 * cleanly. */
void
toolbar_show_toast (const char *text)
{
	char *safe = NULL;
	const char *body;

	if (!toolbar_toast || !text)
		return;

	/* AdwToast stores the title as a UTF-8 string and the
	 * accessibility layer behind it (libadwaita →
	 * gtk_accessible_announce → g_variant_new_string) abort()s
	 * the process on non-UTF-8 input. Most call sites feed
	 * server-supplied bytes (task error strings, broadcast
	 * messages) that can be MacRoman from old Mac servers, so
	 * defend the choke point: validate, fall back to MacRoman
	 * conversion, finally U+FFFD substitution. */
	if (!g_utf8_validate (text, -1, NULL)) {
		safe = gtkhx_text_to_utf8 (text, strlen (text), NULL);
		body = safe ? safe : "";
	} else {
		body = text;
	}

	adw_toast_overlay_add_toast (toolbar_toast, adw_toast_new (body));
	g_free (safe);
}

/* Phase 5: surface "lost connection" with a Reconnect button. The
 * banner is built once with the toolbar; here we just set the title
 * text to the live server name and reveal it. set_status_bar() drives
 * the show/hide on the 1/2 -> 0 (and 2 -> 0) transitions; a fresh
 * login (status == 2) clears the banner via toolbar_hide_banner. */
void
toolbar_show_connection_lost (const char *server)
{
	char *title;

	if (!toolbar_banner)
		return;
	title = g_strdup_printf (_("Lost connection to %s"), server ? server : "");
	adw_banner_set_title    (toolbar_banner, title);
	adw_banner_set_revealed (toolbar_banner, TRUE);
	g_free (title);
}

void
toolbar_hide_banner (void)
{
	if (toolbar_banner)
		adw_banner_set_revealed (toolbar_banner, FALSE);
}

/* Phase 5: register the hamburger-menu actions on the application.
 * Called from gtkhx_activate AFTER the AdwApplication is constructed
 * — fe_init() runs the toolbar construction earlier (before
 * g_application_run), so gtkhx_app is still NULL at that point and
 * we couldn't have registered the actions then. Splitting this out
 * lets the toolbar build with the actions referenced by name; the
 * actions get wired up in time for the menu's first interaction. */
void
toolbar_register_actions (GApplication *app, session *sess)
{
	GAction *act;

	g_return_if_fail (app != NULL);

	g_action_map_add_action_entries (G_ACTION_MAP (app),
	                                 app_actions,
	                                 G_N_ELEMENTS (app_actions),
	                                 sess);

	/* Admin actions start disabled until login (gtkutil.c setbtns
	 * flips them on with the rest of the connection-gated UI). */
	act = g_action_map_lookup_action (G_ACTION_MAP (app), "user_new");
	if (G_IS_SIMPLE_ACTION (act))
		g_simple_action_set_enabled (G_SIMPLE_ACTION (act), FALSE);
	act = g_action_map_lookup_action (G_ACTION_MAP (app), "user_edit");
	if (G_IS_SIMPLE_ACTION (act))
		g_simple_action_set_enabled (G_SIMPLE_ACTION (act), FALSE);
}

/* Phase 5: build the GtkMenuButton + GMenuModel that hangs off the
 * end of the AdwHeaderBar. The connection-specific feature buttons
 * stay in the content row where the user clicks them often; the
 * menu collects the global / less-frequent actions:
 *
 *   Settings
 *   About GtkHx
 *   Admin >
 *     New User...
 *     Edit User...
 *   Quit
 *
 * Admin is a submenu (separate GMenu wrapped via append_submenu)
 * because New / Edit User are sysop-only — keeping them visible
 * but tucked away from the everyday user-flow. The GActions stay
 * disabled at startup; setbtns() flips them on at login (and only
 * for accounts whose privileges actually grant admin). */
static GtkWidget *
build_hamburger (void)
{
	GMenu *menu, *admin_menu;
	GtkWidget *btn;

	admin_menu = g_menu_new ();
	g_menu_append (admin_menu, _("New User…"),  "app.user_new");
	g_menu_append (admin_menu, _("Edit User…"), "app.user_edit");

	menu = g_menu_new ();
	g_menu_append (menu, _("Settings"),    "app.settings");
	g_menu_append (menu, _("About GtkHx"), "app.about");
	g_menu_append_submenu (menu, _("Admin"), G_MENU_MODEL (admin_menu));
	g_menu_append (menu, _("Quit"),        "app.quit");
	g_object_unref (admin_menu);

	btn = gtk_menu_button_new ();
	gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (btn), "open-menu-symbolic");
	gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (btn), G_MENU_MODEL (menu));
	gtk_widget_set_tooltip_text (btn, _("Main menu"));
	g_object_unref (menu);
	return btn;
}

/* Phase 5: 2x scale on the toolbar pixmap buttons. The historic
 * 16x16 XPMs read as tiny pixel-art runes at modern desktop sizes;
 * the gtkhx_pixmap_button helper in gtkutil.c upscales them with
 * nearest-neighbor before rendering, keeping the crisp blocky look
 * but at a more visually-prominent 32x32. */
#define TOOLBAR_ICON_SCALE 2

static GtkWidget *
make_pixmap_button (const char *resource_name,
                    const char *tooltip,
                    GCallback   cb,
                    gpointer    user_data)
{
	return gtkhx_pixmap_button (resource_name, tooltip,
	                            TOOLBAR_ICON_SCALE, cb, user_data);
}

/* Phase 5: rebuild the AdwSplitButton's bookmark menu. Called from
 * connect.c after a successful Save Bookmark so newly-saved
 * entries appear in the dropdown without restarting the app. The
 * earlier lazy-rebuild-on-popover-show approach (a "show" signal
 * hook on the popover) had a chicken-and-egg problem: AdwSplitButton
 * creates its popover lazily, so on the first dropdown click the
 * popover didn't yet exist when we connected the signal, and the
 * menu wouldn't update. An explicit refresh call from the save path
 * is simpler and behaves predictably. */
void
toolbar_refresh_bookmarks (void)
{
	GMenu *menu;
	if (!connect_btn || !ADW_IS_SPLIT_BUTTON (connect_btn))
		return;
	menu = connect_build_bookmark_menu ();
	adw_split_button_set_menu_model (ADW_SPLIT_BUTTON (connect_btn),
	                                 G_MENU_MODEL (menu));
	g_object_unref (menu);
}

/* Phase 4.5: configure-event is gone in GTK 4 and the toolbar window
 * has no resizable size to save anyway. Position is captured at
 * hx_quit() in gtkhx.c gtkhx_save_window_positions. */

void create_toolbar_window (session *sess)
{
	GtkWidget *header;
	GtkWidget *hbox, *vbox;

	/* Phase 5: stay on plain GtkWindow rather than AdwApplicationWindow.
	 * fe_init() runs the toolbar construction BEFORE g_application_run
	 * (so gtkhx_app is still NULL here — confirmed by an earlier
	 * AdwApplicationWindow attempt that hit a NULL-app assertion at
	 * this point). The Phase 3.6 toplevel sweep in gtkhx_activate
	 * registers this window with GtkApplication later, and the
	 * AdwHeaderBar slotted in via gtk_window_set_titlebar gives us
	 * the same "no double title bar" appearance AdwApplicationWindow
	 * would have. The hamburger actions live on the application and
	 * get registered from gtkhx_activate via toolbar_register_actions. */
	toolbar_window = gtk_window_new ();
	gtk_window_set_title (GTK_WINDOW (toolbar_window), "GtkHx");
	gtk_window_set_resizable (GTK_WINDOW (toolbar_window), FALSE);

	/* ------------- header bar (top) ------------- */
	header = adw_header_bar_new ();

	/* Phase 5: AdwSplitButton — primary click opens the connect
	 * dialog; the dropdown chevron exposes a menu of saved
	 * bookmarks targeting app.open_bookmark with the bookmark name
	 * as parameter. Refresh of the menu happens from the bookmark
	 * save path via toolbar_refresh_bookmarks(). */
	connect_btn = adw_split_button_new ();
	adw_split_button_set_icon_name (ADW_SPLIT_BUTTON (connect_btn),
	                                "network-transmit-receive-symbolic");
	gtk_widget_add_css_class (connect_btn, "suggested-action");
	gtk_widget_set_tooltip_text (connect_btn, _("Connect"));
	g_signal_connect (connect_btn, "clicked",
	                  G_CALLBACK (create_connect_window), sess);
	{
		GMenu *menu = connect_build_bookmark_menu ();
		adw_split_button_set_menu_model (ADW_SPLIT_BUTTON (connect_btn),
		                                 G_MENU_MODEL (menu));
		g_object_unref (menu);
	}
	adw_header_bar_pack_start (ADW_HEADER_BAR (header), connect_btn);

	disconnect_btn = gtk_button_new_from_icon_name ("network-offline-symbolic");
	gtk_widget_set_tooltip_text (disconnect_btn, _("Disconnect"));
	g_signal_connect (disconnect_btn, "clicked",
	                  G_CALLBACK (disconnect_clicked), sess);
	adw_header_bar_pack_start (ADW_HEADER_BAR (header), disconnect_btn);

	adw_header_bar_pack_end (ADW_HEADER_BAR (header), build_hamburger ());

	/* ------------- content (feature button row) ------------- */
	hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);
	gtk_widget_set_margin_start  (hbox, 6);
	gtk_widget_set_margin_end    (hbox, 6);
	gtk_widget_set_margin_top    (hbox, 6);
	gtk_widget_set_margin_bottom (hbox, 6);
	/* Phase 5: AdwToolbarView's content slot fills vertically, which
	 * stretches a single row of icon buttons into uncomfortably tall
	 * rectangles. Pin the row to its natural height and center it
	 * vertically so the toolbar reads as a strip of buttons rather
	 * than a wall of them. */
	gtk_widget_set_valign (hbox, GTK_ALIGN_CENTER);
	gtk_widget_set_vexpand (hbox, FALSE);

	gtk_box_append (GTK_BOX (hbox),
		make_pixmap_button ("/com/nasledov/gtkhx/pixmaps/tracker.xpm",
		                    _("Tracker"),
		                    G_CALLBACK (create_tracker_window), sess));
	news_btn = make_pixmap_button ("/com/nasledov/gtkhx/pixmaps/news.xpm",
	                               _("News"),
	                               G_CALLBACK (open_news), sess);
	gtk_box_append (GTK_BOX (hbox), news_btn);
	post_btn = make_pixmap_button ("/com/nasledov/gtkhx/pixmaps/postnews.xpm",
	                               _("Post"),
	                               G_CALLBACK (create_post_window), sess);
	gtk_box_append (GTK_BOX (hbox), post_btn);
	news15_btn = make_pixmap_button ("/com/nasledov/gtkhx/pixmaps/newscat.xpm",
	                                 _("News (1.5+)"),
	                                 G_CALLBACK (open_news15), sess);
	gtk_box_append (GTK_BOX (hbox), news15_btn);
	files_btn = make_pixmap_button ("/com/nasledov/gtkhx/pixmaps/files.xpm",
	                                _("Files"),
	                                G_CALLBACK (open_files), sess);
	gtk_box_append (GTK_BOX (hbox), files_btn);
	gtk_box_append (GTK_BOX (hbox),
		make_pixmap_button ("/com/nasledov/gtkhx/pixmaps/users.xpm",
		                    _("Users"),
		                    G_CALLBACK (create_users_window), sess));
	gtk_box_append (GTK_BOX (hbox),
		make_pixmap_button ("/com/nasledov/gtkhx/pixmaps/chat.xpm",
		                    _("Chat"),
		                    G_CALLBACK (create_chat_window), sess));
	gtk_box_append (GTK_BOX (hbox),
		make_pixmap_button ("/com/nasledov/gtkhx/pixmaps/tasks.xpm",
		                    _("Tasks"),
		                    G_CALLBACK (create_tasks_window), sess));
	/* Phase 5: New User / Edit User used to be toolbar buttons.
	 * They've moved into the hamburger menu's Admin submenu — sysop
	 * actions don't need to occupy primary real estate. */

#ifdef USE_PLUGIN
	plugin_btn = gtk_button_new_with_label ("[ P ]");
	gtk_widget_set_tooltip_text (plugin_btn, _("Plugin Manager"));
	g_signal_connect (plugin_btn, "clicked",
	                  G_CALLBACK (create_plugin_manager), 0);
	gtk_box_append (GTK_BOX (hbox), plugin_btn);
#endif

	/* ------------- bottom bar (status label) ------------- */
	status_bar = gtk_label_new (_("Not Connected"));
	gtk_widget_add_css_class (status_bar, "dim-label");
	gtk_widget_set_halign (status_bar, GTK_ALIGN_START);
	gtk_widget_set_margin_start  (status_bar, 8);
	gtk_widget_set_margin_end    (status_bar, 8);
	gtk_widget_set_margin_top    (status_bar, 4);
	gtk_widget_set_margin_bottom (status_bar, 4);

	/* ------------- compose ------------- */
	/* Phase 5: gtk_window_set_titlebar installs the AdwHeaderBar AS
	 * the window's title bar (no GTK default chrome on top of it),
	 * which is what AdwApplicationWindow does implicitly. Content is
	 * a plain vertical GtkBox holding the button row and status
	 * label; the window's non-resizable flag collapses it to the
	 * natural height of those two children. */
	gtk_window_set_titlebar (GTK_WINDOW (toolbar_window), header);

	/* Phase 5: AdwBanner sits above the content row for "lost
	 * connection" state with an actionable Reconnect button.
	 * Hidden by default; toolbar_show_connection_lost() reveals
	 * it from set_status_bar() on the 1/2 -> 0 transition. */
	toolbar_banner = ADW_BANNER (adw_banner_new (""));
	adw_banner_set_button_label (toolbar_banner, _("Reconnect"));
	adw_banner_set_revealed (toolbar_banner, FALSE);
	g_signal_connect (toolbar_banner, "button-clicked",
	                  G_CALLBACK (on_banner_button_clicked), sess);

	vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_append (GTK_BOX (vbox), GTK_WIDGET (toolbar_banner));
	gtk_box_append (GTK_BOX (vbox), hbox);
	/* Phase 5: server banner row (banner.c). Hidden until an
	 * HTLS_HDR_BANNER message arrives. Sits above the status bar
	 * so it doesn't push the persistent connection state out of
	 * sight. */
	gtk_box_append (GTK_BOX (vbox), banner_widget_new ());
	gtk_box_append (GTK_BOX (vbox), status_bar);

	/* Phase 5: AdwToastOverlay wraps the content so toolbar_show_toast()
	 * can push transient notifications over the button row. Toasts
	 * surface as a sliding banner at the bottom of the overlay; the
	 * persistent status label stays visible underneath for ambient
	 * connection state. */
	toolbar_toast = ADW_TOAST_OVERLAY (adw_toast_overlay_new ());
	adw_toast_overlay_set_child (toolbar_toast, vbox);
	gtk_window_set_child (GTK_WINDOW (toolbar_window), GTK_WIDGET (toolbar_toast));

	/* Initial sensitivity: pre-connection, only Connect + the global
	 * menu items are usable. setbtns() flips the rest on at login,
	 * including the Admin submenu's app.user_new / app.user_edit
	 * GActions. */
	gtk_widget_set_sensitive (disconnect_btn, FALSE);
	gtk_widget_set_sensitive (files_btn,      FALSE);
	gtk_widget_set_sensitive (post_btn,       FALSE);
	gtk_widget_set_sensitive (news15_btn,     FALSE);

	/* Phase 3.x: this used to be G_CALLBACK(quit_btn) — but quit_btn is
	 * a GtkWidget pointer, not a function. Calling a widget address as
	 * code did nothing useful (and tripped CFI on hardened builds), so
	 * closing the toolbar via the WM's X button skipped hx_quit and
	 * with it the prefs_write + position-save pass. The app exited
	 * because the last GtkApplication window was gone, but no prefs
	 * survived. Wire this to close_toolbar_window, which calls
	 * hx_quit() properly. */
	g_signal_connect (toolbar_window, "close-request",
	                  G_CALLBACK (close_toolbar_window), 0);

	/* Phase 4.2: gtk_window_move removed (Wayland) */

	gtk_window_present (GTK_WINDOW (toolbar_window));
	init_keyaccel (toolbar_window);

	if (connected) {
		gtk_widget_set_sensitive (disconnect_btn, TRUE);
		gtk_widget_set_sensitive (files_btn,      TRUE);
		gtk_widget_set_sensitive (post_btn,       TRUE);
		gtk_widget_set_sensitive (news15_btn,     TRUE);
		changetitlespecific (toolbar_window, "GtkHx");
	}
	sess->toolbar_window = toolbar_window;
}
