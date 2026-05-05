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

GtkWidget *toolbar_window, *files_btn, *connect_btn, *post_btn, *quit_btn, *disconnect_btn, *usermod_btn, *usernew_btn, *news15_btn;

#ifdef USE_PLUGIN
GtkWidget *plugin_btn;
#endif

GtkWidget *status_bar;
guint status_msg;
guint context_status;

static void create_new_user ()
{
	create_useredit_window(0,1);
}

static void close_toolbar_window (GtkWidget *widget, gpointer data)
{
	hx_quit();
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

/* Phase 3.x: position is now captured at hx_quit() time (see
 * gtkhx_save_window_positions in gtkhx.c) and the toolbar has no
 * resizable size to save, so this handler is a no-op kept for the
 * signal connection symmetry. */
static gboolean tool_move(GtkWidget *w, GdkEventConfigure *e, gpointer data)
{
	(void) w; (void) e; (void) data;
	return FALSE;
}

void create_toolbar_window (session *sess)
{
	GtkWidget *hbox;
	GtkWidget *tracker_btn;
	GtkWidget *options_btn;
	GtkWidget *news_btn;
	GtkWidget *userlist_btn;
	GtkWidget *chat_btn;
	GtkWidget *about_btn;
	GtkWidget *tasks_btn;
	GdkBitmap *mask;
	GtkWidget *pix;
	GdkPixmap *icon;
	GtkWidget *vbox;

	toolbar_window = gtk_window_new();
	gtk_window_set_title(GTK_WINDOW(toolbar_window), "GtkHx");
	gtk_window_set_resizable(GTK_WINDOW(toolbar_window), FALSE);
	g_signal_connect(toolbar_window, "delete_event",
			   G_CALLBACK(close_toolbar_window), 0);

	/* Phase 3.x: dropped GTK 1.2-era realize+get_style pair (style unused). */

	status_bar = gtk_statusbar_new();
	context_status = gtk_statusbar_get_context_id((GtkStatusbar *)status_bar, 
												  "foobar");
	status_msg = gtk_statusbar_push((GtkStatusbar *)status_bar, context_status,
									_("Not Connected"));

	connect_btn = gtk_button_new();
	g_signal_connect(connect_btn, "clicked", 
					   G_CALLBACK(create_connect_window), sess);
	icon = (GdkPixmap *)gdk_pixbuf_new_from_resource("/com/nasledov/gtkhx/pixmaps/connect.xpm", NULL);
    pix = gtk_image_new_from_pixbuf((GdkPixbuf *)icon);
    gtkhx_widget_set_child(connect_btn, pix);
	gtk_widget_set_tooltip_text(connect_btn, _("Connect"));
	icon = 0, pix = 0, mask = 0;

	tracker_btn = gtk_button_new();
	g_signal_connect(tracker_btn, "clicked", 
					   G_CALLBACK(create_tracker_window), sess);
	icon = (GdkPixmap *)gdk_pixbuf_new_from_resource("/com/nasledov/gtkhx/pixmaps/tracker.xpm", NULL);
    pix = gtk_image_new_from_pixbuf((GdkPixbuf *)icon);
    gtkhx_widget_set_child(tracker_btn, pix);
	gtk_widget_set_tooltip_text(tracker_btn, _("Tracker"));
	icon = 0, pix = 0, mask = 0;

	options_btn = gtk_button_new();
	g_signal_connect(options_btn, "clicked", 
					   G_CALLBACK(create_options_window), sess);
	icon = (GdkPixmap *)gdk_pixbuf_new_from_resource("/com/nasledov/gtkhx/pixmaps/options.xpm", NULL);
    pix = gtk_image_new_from_pixbuf((GdkPixbuf *)icon);
    gtkhx_widget_set_child(options_btn, pix);
	gtk_widget_set_tooltip_text(options_btn, _("Options"));
	icon = 0, pix = 0, mask = 0;

	news_btn = gtk_button_new();
	g_signal_connect(news_btn, "clicked",
					   G_CALLBACK(open_news), sess);
	icon = (GdkPixmap *)gdk_pixbuf_new_from_resource("/com/nasledov/gtkhx/pixmaps/news.xpm", NULL);
	pix = gtk_image_new_from_pixbuf((GdkPixbuf *)icon);
	gtkhx_widget_set_child(news_btn, pix);
	gtk_widget_set_tooltip_text(news_btn, _("News"));
	icon = 0, pix = 0, mask = 0;

	news15_btn = gtk_button_new();
	g_signal_connect(news15_btn, "clicked",
					   G_CALLBACK(open_news15), sess);
	icon = (GdkPixmap *)gdk_pixbuf_new_from_resource("/com/nasledov/gtkhx/pixmaps/newscat.xpm", NULL);
	pix = gtk_image_new_from_pixbuf((GdkPixbuf *)icon);
	gtkhx_widget_set_child(news15_btn, pix);
	gtk_widget_set_tooltip_text(news15_btn, _("News (1.5+)"));
	icon = 0, pix = 0, mask = 0;

	files_btn = gtk_button_new();
	g_signal_connect(files_btn, "clicked", 
					   G_CALLBACK(open_files), sess);
	icon = (GdkPixmap *)gdk_pixbuf_new_from_resource("/com/nasledov/gtkhx/pixmaps/files.xpm", NULL);
	pix = gtk_image_new_from_pixbuf((GdkPixbuf *)icon);
	gtkhx_widget_set_child(files_btn, pix);
	gtk_widget_set_tooltip_text(files_btn, _("Files"));
	icon = 0, pix = 0, mask = 0;

	userlist_btn = gtk_button_new();
	g_signal_connect(userlist_btn, "clicked", 
					   G_CALLBACK(create_users_window), sess);
	icon = (GdkPixmap *)gdk_pixbuf_new_from_resource("/com/nasledov/gtkhx/pixmaps/users.xpm", NULL);
	pix = gtk_image_new_from_pixbuf((GdkPixbuf *)icon);
	gtkhx_widget_set_child(userlist_btn, pix);
	gtk_widget_set_tooltip_text(userlist_btn, _("Users"));
	icon = 0, pix = 0, mask = 0;

	chat_btn = gtk_button_new();
	g_signal_connect(chat_btn, "clicked", 
					   G_CALLBACK(create_chat_window), sess);
	icon = (GdkPixmap *)gdk_pixbuf_new_from_resource("/com/nasledov/gtkhx/pixmaps/chat.xpm", NULL);
	pix = gtk_image_new_from_pixbuf((GdkPixbuf *)icon);
	gtkhx_widget_set_child(chat_btn, pix);
	gtk_widget_set_tooltip_text(chat_btn, _("Chat"));
	icon = 0, pix = 0, mask = 0;

	post_btn = gtk_button_new();
	g_signal_connect(post_btn, "clicked", 
					   G_CALLBACK(create_post_window), sess);
	icon = (GdkPixmap *)gdk_pixbuf_new_from_resource("/com/nasledov/gtkhx/pixmaps/postnews.xpm", NULL);
	pix = gtk_image_new_from_pixbuf((GdkPixbuf *)icon);
	gtkhx_widget_set_child(post_btn, pix);
	gtk_widget_set_tooltip_text(post_btn, _("Post"));
	icon = 0, pix = 0, mask = 0;

	tasks_btn = gtk_button_new();
	g_signal_connect(tasks_btn, "clicked", 
					   G_CALLBACK(create_tasks_window), sess);
	icon = (GdkPixmap *)gdk_pixbuf_new_from_resource("/com/nasledov/gtkhx/pixmaps/tasks.xpm", NULL);
    pix = gtk_image_new_from_pixbuf((GdkPixbuf *)icon);
    gtkhx_widget_set_child(tasks_btn, pix);
	gtk_widget_set_tooltip_text(tasks_btn, _("Tasks"));
	icon = 0, pix = 0, mask = 0;

	about_btn = gtk_button_new();
	g_signal_connect(about_btn, "clicked", 
					   G_CALLBACK(create_about_window), 0);
	icon = (GdkPixmap *)gdk_pixbuf_new_from_resource("/com/nasledov/gtkhx/pixmaps/info.xpm", NULL);
	pix = gtk_image_new_from_pixbuf((GdkPixbuf *)icon);
	gtkhx_widget_set_child(about_btn, pix);
	gtk_widget_set_tooltip_text(about_btn, _("About"));
	icon = 0, pix = 0, mask = 0;

	disconnect_btn = gtk_button_new();
	g_signal_connect(disconnect_btn, "clicked",
					   G_CALLBACK(disconnect_clicked), sess);
	icon = (GdkPixmap *)gdk_pixbuf_new_from_resource("/com/nasledov/gtkhx/pixmaps/kick.xpm", NULL);
	pix = gtk_image_new_from_pixbuf((GdkPixbuf *)icon);
    gtkhx_widget_set_child(disconnect_btn, pix);
	gtk_widget_set_tooltip_text(disconnect_btn, _("Disconnect"));
	icon = 0, pix = 0, mask = 0;

	quit_btn = gtk_button_new();
	g_signal_connect(quit_btn, "clicked", 
					   G_CALLBACK(hx_quit), 0);
	icon = (GdkPixmap *)gdk_pixbuf_new_from_resource("/com/nasledov/gtkhx/pixmaps/quit.xpm", NULL);
    pix = gtk_image_new_from_pixbuf((GdkPixbuf *)icon);
    gtkhx_widget_set_child(quit_btn, pix);
	gtk_widget_set_tooltip_text(quit_btn, _("Quit"));
	icon = 0, pix = 0, mask = 0;


	usernew_btn = gtk_button_new();
	g_signal_connect(usernew_btn, "clicked", 
					   G_CALLBACK(create_new_user), sess);
	icon = (GdkPixmap *)gdk_pixbuf_new_from_resource("/com/nasledov/gtkhx/pixmaps/newuser.xpm", NULL);
	pix = gtk_image_new_from_pixbuf((GdkPixbuf *)icon);
    gtkhx_widget_set_child(usernew_btn, pix);
	gtk_widget_set_tooltip_text(usernew_btn, _("New User"));
	icon = 0, pix = 0, mask = 0;

	usermod_btn = gtk_button_new();
	g_signal_connect(usermod_btn, "clicked", 
					   G_CALLBACK(useredit_open_dialog), sess);
	icon = (GdkPixmap *)gdk_pixbuf_new_from_resource("/com/nasledov/gtkhx/pixmaps/edituser.xpm", NULL);
	pix = gtk_image_new_from_pixbuf((GdkPixbuf *)icon);
    gtkhx_widget_set_child(usermod_btn, pix);
	gtk_widget_set_tooltip_text(usermod_btn, _("Edit User"));
	icon = 0, pix = 0, mask = 0;

#ifdef USE_PLUGIN
	plugin_btn = gtk_button_new_with_label("[ P ]");
	g_signal_connect(plugin_btn, "clicked", 
					   G_CALLBACK(create_plugin_manager), 0);
	gtk_widget_set_tooltip_text(plugin_btn, _("Plugin Manager"));
#endif

	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
	(gtk_widget_set_margin_start(hbox, 2), gtk_widget_set_margin_end(hbox, 2), gtk_widget_set_margin_top(hbox, 2), gtk_widget_set_margin_bottom(hbox, 2));
	gtkhx_widget_set_child(toolbar_window, vbox);
	gtkhx_box_pack(vbox, hbox, 0, 0, 0);
	gtkhx_box_pack(hbox, connect_btn, 0, 0, 0);
	gtkhx_box_pack(hbox, disconnect_btn, 0, 0, 0);
	gtkhx_box_pack(hbox, tracker_btn, 0, 0, 0);
	gtkhx_box_pack(hbox, options_btn, 0, 0, 0);
	gtkhx_box_pack(hbox, news_btn, 0, 0, 0);
	gtkhx_box_pack(hbox, post_btn, 0, 0, 0);
	gtkhx_box_pack(hbox, news15_btn, 0, 0, 0);
	gtkhx_box_pack(hbox, files_btn, 0, 0, 0);
	gtkhx_box_pack(hbox, userlist_btn, 0, 0, 0);
	gtkhx_box_pack(hbox, chat_btn, 0, 0, 0);
	gtkhx_box_pack(hbox, tasks_btn, 0, 0, 0);
	gtkhx_box_pack(hbox, usernew_btn, 0, 0, 0);
	gtkhx_box_pack(hbox, usermod_btn, 0, 0, 0);
#ifdef USE_PLUGIN
	gtkhx_box_pack(hbox, plugin_btn, 0, 0, 0);

#endif
	gtkhx_box_pack(hbox, about_btn, 0, 0, 0);
	gtkhx_box_pack(hbox, quit_btn, 0, 0, 0);
	gtkhx_box_pack(vbox, status_bar, 0, 0, 0);

	gtk_widget_set_sensitive(disconnect_btn, FALSE);
	gtk_widget_set_sensitive(files_btn, FALSE);
	gtk_widget_set_sensitive(post_btn, FALSE);
	gtk_widget_set_sensitive(usermod_btn, FALSE);
	gtk_widget_set_sensitive(usernew_btn, FALSE);
	gtk_widget_set_sensitive(news15_btn, FALSE);

	g_signal_connect(toolbar_window, "configure_event", G_CALLBACK(tool_move), 0);
	/* Phase 3.x: this used to be G_CALLBACK(quit_btn) — but quit_btn is
	 * a GtkWidget pointer, not a function. Calling a widget address as
	 * code did nothing useful (and tripped CFI on hardened builds), so
	 * closing the toolbar via the WM's X button skipped hx_quit and
	 * with it the prefs_write + position-save pass. The app exited
	 * because the last GtkApplication window was gone, but no prefs
	 * survived. Wire this to close_toolbar_window, which calls
	 * hx_quit() properly. */
	g_signal_connect(toolbar_window, "delete_event",
	                 G_CALLBACK(close_toolbar_window), 0);

	gtk_window_move(GTK_WINDOW(toolbar_window), gtkhx_prefs.geo.tool.xpos, gtkhx_prefs.geo.tool.ypos);

	gtk_widget_show(toolbar_window);
	init_keyaccel(toolbar_window);

	if(connected) {
		gtk_widget_set_sensitive(disconnect_btn, TRUE);
		gtk_widget_set_sensitive(files_btn, TRUE);
		gtk_widget_set_sensitive(post_btn, TRUE);
		gtk_widget_set_sensitive(usermod_btn, TRUE);
		gtk_widget_set_sensitive(usernew_btn, TRUE);
		gtk_widget_set_sensitive(news15_btn, TRUE);
		changetitlespecific(toolbar_window, "GtkHx");
	}
	sess->toolbar_window = toolbar_window;
}
