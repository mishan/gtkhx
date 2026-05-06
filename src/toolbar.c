/*
 * Copyright (C) 2000-2002 Misha Nasledov <misha@nasledov.com>
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
#include "tracker.h"
#include "gtkhx.h"
#include "users.h"
#include "chat.h"
#include "tasks.h"
#include "options.h"
#include "connect.h"
#include "files.h"
#include "usermod.h"
#include "about.h"
#include "options.h"
#include "gtkthreads.h"
#include "plugin.h"
#include "toolbar.h"

GtkWidget *toolbar_window, *files_btn, *connect_btn, *post_btn;
GtkWidget *disconnect_btn, *usermod_btn, *usernew_btn, *news15_btn;

#ifdef USE_PLUGIN
GtkWidget *plugin_btn;
#endif

/* Phase 5: status_bar is now a GtkLabel. The previous GtkStatusbar
 * was deprecated in GTK 4.10 and we never used its stack-of-messages
 * model — we always replaced the message wholesale. set_status_bar()
 * in gtkutil.c does a single gtk_label_set_text() now. The
 * status_msg / context_status globals are gone with it. */
GtkWidget *status_bar;

static void create_new_user (void)
{
	create_useredit_window(0,1);
}

/* Phase 4.5: GTK 4 close-request signature is (GtkWindow *, gpointer)
 * returning gboolean. Returning TRUE inhibits the default destroy —
 * we always want to call hx_quit() (which calls exit()), so the
 * return value never actually flows back. */
static gboolean close_toolbar_window (GtkWindow *window, gpointer data)
{
	(void) window; (void) data;
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
 * accelerators for free if we ever wire any (gtk_application_set_accels_for_action). */
static void
on_action_settings (GSimpleAction *action, GVariant *param, gpointer user_data)
{
	(void) action; (void) param;
	create_options_window (NULL, user_data);
}

static void
on_action_about (GSimpleAction *action, GVariant *param, gpointer user_data)
{
	(void) action; (void) param; (void) user_data;
	create_about_window (NULL, NULL);
}

static void
on_action_quit (GSimpleAction *action, GVariant *param, gpointer user_data)
{
	(void) action; (void) param; (void) user_data;
	hx_quit ();
}

static const GActionEntry app_actions[] = {
	{ .name = "settings", .activate = on_action_settings },
	{ .name = "about",    .activate = on_action_about    },
	{ .name = "quit",     .activate = on_action_quit     },
};

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
	g_return_if_fail (app != NULL);

	g_action_map_add_action_entries (G_ACTION_MAP (app),
	                                 app_actions,
	                                 G_N_ELEMENTS (app_actions),
	                                 sess);
}

/* Phase 5: build the GtkMenuButton + GMenuModel that hangs off the
 * end of the AdwHeaderBar. Three entries — Settings, About, Quit —
 * since those are the global, server-independent actions. The
 * connection-specific buttons stay on the content row where the
 * user clicks them often. */
static GtkWidget *
build_hamburger (void)
{
	GMenu *menu;
	GtkWidget *btn;

	menu = g_menu_new ();
	g_menu_append (menu, _("Settings"),    "app.settings");
	g_menu_append (menu, _("About GtkHx"), "app.about");
	g_menu_append (menu, _("Quit"),        "app.quit");

	btn = gtk_menu_button_new ();
	gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (btn), "open-menu-symbolic");
	gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (btn), G_MENU_MODEL (menu));
	gtk_widget_set_tooltip_text (btn, _("Main menu"));
	g_object_unref (menu);
	return btn;
}

/* Helper: build one of the legacy pixmap-icon buttons for the content
 * row. Centralizes the gdk_pixbuf_new_from_resource + set_child + tooltip
 * dance so adding a new toolbar action is one line at the call site. */
static GtkWidget *
make_pixmap_button (const char *resource_name,
                    const char *tooltip,
                    GCallback   cb,
                    gpointer    user_data)
{
	GtkWidget *btn = gtk_button_new ();
	GdkPixbuf *pb;
	GtkWidget *image;

	pb = gdk_pixbuf_new_from_resource (resource_name, NULL);
	image = gtkhx_image_new_from_pixbuf (pb);
	gtkhx_widget_set_child (btn, image);
	gtk_widget_set_tooltip_text (btn, tooltip);
	if (cb)
		g_signal_connect (btn, "clicked", cb, user_data);
	g_clear_object (&pb);
	return btn;
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

	connect_btn = gtk_button_new_from_icon_name ("network-transmit-receive-symbolic");
	gtk_widget_add_css_class (connect_btn, "suggested-action");
	gtk_widget_set_tooltip_text (connect_btn, _("Connect"));
	g_signal_connect (connect_btn, "clicked",
	                  G_CALLBACK (create_connect_window), sess);
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
	gtk_box_append (GTK_BOX (hbox),
		make_pixmap_button ("/com/nasledov/gtkhx/pixmaps/news.xpm",
		                    _("News"),
		                    G_CALLBACK (open_news), sess));
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
	usernew_btn = make_pixmap_button ("/com/nasledov/gtkhx/pixmaps/newuser.xpm",
	                                  _("New User"),
	                                  G_CALLBACK (create_new_user), sess);
	gtk_box_append (GTK_BOX (hbox), usernew_btn);
	usermod_btn = make_pixmap_button ("/com/nasledov/gtkhx/pixmaps/edituser.xpm",
	                                  _("Edit User"),
	                                  G_CALLBACK (useredit_open_dialog), sess);
	gtk_box_append (GTK_BOX (hbox), usermod_btn);

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

	vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_append (GTK_BOX (vbox), hbox);
	gtk_box_append (GTK_BOX (vbox), status_bar);
	gtk_window_set_child (GTK_WINDOW (toolbar_window), vbox);

	/* Initial sensitivity: pre-connection, only Connect + the global
	 * menu items are usable. setbtns() flips the rest on at login. */
	gtk_widget_set_sensitive (disconnect_btn, FALSE);
	gtk_widget_set_sensitive (files_btn,      FALSE);
	gtk_widget_set_sensitive (post_btn,       FALSE);
	gtk_widget_set_sensitive (usermod_btn,    FALSE);
	gtk_widget_set_sensitive (usernew_btn,    FALSE);
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
		gtk_widget_set_sensitive (usermod_btn,    TRUE);
		gtk_widget_set_sensitive (usernew_btn,    TRUE);
		gtk_widget_set_sensitive (news15_btn,     TRUE);
		changetitlespecific (toolbar_window, "GtkHx");
	}
	sess->toolbar_window = toolbar_window;
}
