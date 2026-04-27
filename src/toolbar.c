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
#include "pixmaps/connect.xpm"
#include "pixmaps/options.xpm"
#include "pixmaps/tracker.xpm"
#include "pixmaps/kick.xpm"
#include "pixmaps/tasks.xpm"
#include "pixmaps/quit.xpm"
#include "pixmaps/news.xpm"
#include "pixmaps/files.xpm"
#include "pixmaps/info.xpm"
#include "pixmaps/users.xpm"
#include "pixmaps/chat.xpm"
#include "pixmaps/edituser.xpm"
#include "pixmaps/newuser.xpm"
#include "pixmaps/postnews.xpm"
#include "pixmaps/newscat.xpm"

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
		setbtns(&sessions[0], 0);
		set_status_bar(0);
		set_disconnect_btn(&sessions[0], 0);
		conn_task_update(&sessions[0], 2);
		if(sessions[0].htlc.gdk_input) {
			hxd_fd_clr(sessions[0].htlc.fd, FDR|FDW);
			close(sessions[0].htlc.fd);
			sessions[0].htlc.gdk_input = 0;
		}
		hx_printf_prefix(&sessions[0].htlc, 0, INFOPREFIX, "%s: %s\n", server_addr, _("connection closed"));
	}
	
	else if (sessions[0].htlc.fd) {
		hx_htlc_close(&sessions[0].htlc, 1);
	}
}

static void tool_move(GtkWidget *w, GdkEventConfigure *e, gpointer data)
{
	int x, y;

	gdk_window_get_root_origin(toolbar_window->window, &x, &y);


	if(e->send_event) { /* Is a position event */
		gtkhx_prefs.geo.tool.xpos = x;
		gtkhx_prefs.geo.tool.ypos = y;
	}
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
	GtkStyle *style;
	GdkBitmap *mask;
	GtkWidget *pix;
	GdkPixmap *icon;
	GtkWidget *vbox;

	toolbar_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_wmclass(GTK_WINDOW(toolbar_window), "toolbar", "GtkHx");
	gtk_window_set_title(GTK_WINDOW(toolbar_window), "GtkHx");
	gtk_window_set_policy(GTK_WINDOW(toolbar_window), 0, 0, 0);
	gtk_signal_connect(GTK_OBJECT(toolbar_window), "delete_event",
			   GTK_SIGNAL_FUNC(close_toolbar_window), 0);

	gtk_widget_realize(toolbar_window);
	style = gtk_widget_get_style(toolbar_window);

	status_bar = gtk_statusbar_new();
	context_status = gtk_statusbar_get_context_id((GtkStatusbar *)status_bar, 
												  "foobar");
	status_msg = gtk_statusbar_push((GtkStatusbar *)status_bar, context_status,
									_("Not Connected"));

	connect_btn = gtk_button_new();
	gtk_signal_connect(GTK_OBJECT(connect_btn), "clicked", 
					   GTK_SIGNAL_FUNC(create_connect_window), sess);
	icon = gdk_pixmap_create_from_xpm_d (toolbar_window->window, &mask, 
										 &style->bg[GTK_STATE_NORMAL], 
										 connect_xpm);
    pix = gtk_pixmap_new(icon, mask);
    gtk_container_add (GTK_CONTAINER (connect_btn), pix);
	gtk_tooltips_set_tip(tooltips, connect_btn, _("Connect"), 0);
	icon = 0, pix = 0, mask = 0;

	tracker_btn = gtk_button_new();
	gtk_signal_connect(GTK_OBJECT(tracker_btn), "clicked", 
					   GTK_SIGNAL_FUNC(create_tracker_window), sess);
	icon = gdk_pixmap_create_from_xpm_d (toolbar_window->window, 
										 &mask, 
										 &style->bg[GTK_STATE_NORMAL], 
										 tracker_xpm);
    pix = gtk_pixmap_new(icon, mask);
    gtk_container_add (GTK_CONTAINER (tracker_btn), pix);
	gtk_tooltips_set_tip(tooltips, tracker_btn, _("Tracker"), 0);
	icon = 0, pix = 0, mask = 0;

	options_btn = gtk_button_new();
	gtk_signal_connect(GTK_OBJECT(options_btn), "clicked", 
					   GTK_SIGNAL_FUNC(create_options_window), sess);
	icon = gdk_pixmap_create_from_xpm_d (toolbar_window->window, &mask, 
										 &style->bg[GTK_STATE_NORMAL], 
										 options_xpm);
    pix = gtk_pixmap_new(icon, mask);
    gtk_container_add (GTK_CONTAINER (options_btn), pix);
	gtk_tooltips_set_tip(tooltips, options_btn, _("Options"), 0);
	icon = 0, pix = 0, mask = 0;

	news_btn = gtk_button_new();
	gtk_signal_connect(GTK_OBJECT(news_btn), "clicked",
					   GTK_SIGNAL_FUNC(open_news), sess);
	icon = gdk_pixmap_create_from_xpm_d(toolbar_window->window, 
										&mask, 
										&style->bg[GTK_STATE_NORMAL], 
										news_xpm);
	pix = gtk_pixmap_new(icon, mask);
	gtk_container_add(GTK_CONTAINER(news_btn), pix);
	gtk_tooltips_set_tip(tooltips, news_btn, _("News"), 0);
	icon = 0, pix = 0, mask = 0;

	news15_btn = gtk_button_new();
	gtk_signal_connect(GTK_OBJECT(news15_btn), "clicked",
					   GTK_SIGNAL_FUNC(open_news15), sess);
	icon = gdk_pixmap_create_from_xpm_d(toolbar_window->window, 
										&mask, 
										&style->bg[GTK_STATE_NORMAL], 
										newscat_xpm);
	pix = gtk_pixmap_new(icon, mask);
	gtk_container_add(GTK_CONTAINER(news15_btn), pix);
	gtk_tooltips_set_tip(tooltips, news15_btn, _("News (1.5+)"), 0);
	icon = 0, pix = 0, mask = 0;

	files_btn = gtk_button_new();
	gtk_signal_connect(GTK_OBJECT(files_btn), "clicked", 
					   GTK_SIGNAL_FUNC(open_files), sess);
	icon = gdk_pixmap_create_from_xpm_d(toolbar_window->window, &mask, 
										&style->bg[GTK_STATE_NORMAL], 
										files_xpm);
	pix = gtk_pixmap_new(icon, mask);
	gtk_container_add(GTK_CONTAINER(files_btn), pix);
	gtk_tooltips_set_tip(tooltips, files_btn, _("Files"), 0);
	icon = 0, pix = 0, mask = 0;

	userlist_btn = gtk_button_new();
	gtk_signal_connect(GTK_OBJECT(userlist_btn), "clicked", 
					   GTK_SIGNAL_FUNC(create_users_window), sess);
	icon = gdk_pixmap_create_from_xpm_d(toolbar_window->window, &mask, 
										&style->bg[GTK_STATE_NORMAL], 
										users_xpm);
	pix = gtk_pixmap_new(icon, mask);
	gtk_container_add(GTK_CONTAINER(userlist_btn), pix);
	gtk_tooltips_set_tip(tooltips, userlist_btn, _("Users"), 0);
	icon = 0, pix = 0, mask = 0;

	chat_btn = gtk_button_new();
	gtk_signal_connect(GTK_OBJECT(chat_btn), "clicked", 
					   GTK_SIGNAL_FUNC(create_chat_window), sess);
	icon = gdk_pixmap_create_from_xpm_d(toolbar_window->window, &mask, 
										&style->bg[GTK_STATE_NORMAL], chat_xpm);
	pix = gtk_pixmap_new(icon, mask);
	gtk_container_add(GTK_CONTAINER(chat_btn), pix);
	gtk_tooltips_set_tip(tooltips, chat_btn, _("Chat"), 0);
	icon = 0, pix = 0, mask = 0;

	post_btn = gtk_button_new();
	gtk_signal_connect(GTK_OBJECT(post_btn), "clicked", 
					   GTK_SIGNAL_FUNC(create_post_window), sess);
	icon = gdk_pixmap_create_from_xpm_d(toolbar_window->window, 
										&mask, 
										&style->bg[GTK_STATE_NORMAL], 
										postnews_xpm);
	pix = gtk_pixmap_new(icon, mask);
	gtk_container_add(GTK_CONTAINER(post_btn), pix);
	gtk_tooltips_set_tip(tooltips, post_btn, _("Post"), 0);
	icon = 0, pix = 0, mask = 0;

	tasks_btn = gtk_button_new();
	gtk_signal_connect(GTK_OBJECT(tasks_btn), "clicked", 
					   GTK_SIGNAL_FUNC(create_tasks_window), sess);
	icon = gdk_pixmap_create_from_xpm_d (toolbar_window->window, &mask, 
										 &style->bg[GTK_STATE_NORMAL], 
										 tasks_xpm);
    pix = gtk_pixmap_new(icon, mask);
    gtk_container_add (GTK_CONTAINER (tasks_btn), pix);
	gtk_tooltips_set_tip(tooltips, tasks_btn, _("Tasks"), 0);
	icon = 0, pix = 0, mask = 0;

	about_btn = gtk_button_new();
	gtk_signal_connect(GTK_OBJECT(about_btn), "clicked", 
					   GTK_SIGNAL_FUNC(create_about_window), 0);
	icon = gdk_pixmap_create_from_xpm_d(toolbar_window->window, &mask, 
										&style->bg[GTK_STATE_NORMAL], 
										info_xpm);
	pix = gtk_pixmap_new(icon, mask);
	gtk_container_add(GTK_CONTAINER(about_btn), pix);
	gtk_tooltips_set_tip(tooltips, about_btn, _("About"), 0);
	icon = 0, pix = 0, mask = 0;

	disconnect_btn = gtk_button_new();
	gtk_signal_connect(GTK_OBJECT(disconnect_btn), "clicked",
					   GTK_SIGNAL_FUNC(disconnect_clicked), sess);
	icon = gdk_pixmap_create_from_xpm_d (toolbar_window->window, &mask, 
										 &style->bg[GTK_STATE_NORMAL], 
										 kick_xpm);
	pix = gtk_pixmap_new(icon, mask);
    gtk_container_add (GTK_CONTAINER (disconnect_btn),  pix);
	gtk_tooltips_set_tip(tooltips, disconnect_btn, _("Disconnect"), 0);
	icon = 0, pix = 0, mask = 0;

	quit_btn = gtk_button_new();
	gtk_signal_connect(GTK_OBJECT(quit_btn), "clicked", 
					   GTK_SIGNAL_FUNC(hx_quit), 0);
	icon = gdk_pixmap_create_from_xpm_d (toolbar_window->window, &mask, 
										 &style->bg[GTK_STATE_NORMAL], 
										 quit_xpm);
    pix = gtk_pixmap_new(icon, mask);
    gtk_container_add (GTK_CONTAINER(quit_btn), pix);
	gtk_tooltips_set_tip(tooltips, quit_btn, _("Quit"), 0);
	icon = 0, pix = 0, mask = 0;


	usernew_btn = gtk_button_new();
	gtk_signal_connect(GTK_OBJECT(usernew_btn), "clicked", 
					   GTK_SIGNAL_FUNC(create_new_user), sess);
	icon = gdk_pixmap_create_from_xpm_d (toolbar_window->window, &mask, 
										 &style->bg[GTK_STATE_NORMAL], 
										 newuser_xpm);
	pix = gtk_pixmap_new(icon, mask);
    gtk_container_add (GTK_CONTAINER (usernew_btn),  pix);
	gtk_tooltips_set_tip(tooltips, usernew_btn, _("New User"), 0);
	icon = 0, pix = 0, mask = 0;

	usermod_btn = gtk_button_new();
	gtk_signal_connect(GTK_OBJECT(usermod_btn), "clicked", 
					   GTK_SIGNAL_FUNC(useredit_open_dialog), sess);
	icon = gdk_pixmap_create_from_xpm_d (toolbar_window->window, &mask, 
										 &style->bg[GTK_STATE_NORMAL], 
										 edituser_xpm);
	pix = gtk_pixmap_new(icon, mask);
    gtk_container_add (GTK_CONTAINER (usermod_btn),  pix);
	gtk_tooltips_set_tip(tooltips, usermod_btn, _("Edit User"), 0);
	icon = 0, pix = 0, mask = 0;

#ifdef USE_PLUGIN
	plugin_btn = gtk_button_new_with_label("[ P ]");
	gtk_signal_connect(GTK_OBJECT(plugin_btn), "clicked", 
					   GTK_SIGNAL_FUNC(create_plugin_manager), 0);
	gtk_tooltips_set_tip(tooltips, plugin_btn, _("Plugin Manager"), 0);
#endif

	vbox = gtk_vbox_new(0, 0);
	hbox = gtk_hbox_new(0, 2);
	gtk_container_set_border_width(GTK_CONTAINER(hbox), 2);
	gtk_container_add(GTK_CONTAINER(toolbar_window), vbox);
	gtk_box_pack_start(GTK_BOX(vbox), hbox, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(hbox), connect_btn, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(hbox), disconnect_btn, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(hbox), tracker_btn, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(hbox), options_btn, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(hbox), news_btn, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(hbox), post_btn, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(hbox), news15_btn, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(hbox), files_btn, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(hbox), userlist_btn, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(hbox), chat_btn, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(hbox), tasks_btn, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(hbox), usernew_btn, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(hbox), usermod_btn, 0, 0, 0);
#ifdef USE_PLUGIN
	gtk_box_pack_start(GTK_BOX(hbox), plugin_btn, 0, 0, 0);

#endif
	gtk_box_pack_start(GTK_BOX(hbox), about_btn, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(hbox), quit_btn, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(vbox), status_bar, 0, 0, 0);

	gtk_widget_set_sensitive(disconnect_btn, FALSE);
	gtk_widget_set_sensitive(files_btn, FALSE);
	gtk_widget_set_sensitive(post_btn, FALSE);
	gtk_widget_set_sensitive(usermod_btn, FALSE);
	gtk_widget_set_sensitive(usernew_btn, FALSE);
	gtk_widget_set_sensitive(news15_btn, FALSE);

	gtk_signal_connect(GTK_OBJECT(toolbar_window), "configure_event", GTK_SIGNAL_FUNC(tool_move), 0);
	gtk_signal_connect(GTK_OBJECT(toolbar_window), "delete_event", GTK_SIGNAL_FUNC(quit_btn), 0);

	gtk_widget_set_uposition(toolbar_window, gtkhx_prefs.geo.tool.xpos, gtkhx_prefs.geo.tool.ypos);

	gtk_widget_show_all(toolbar_window);
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
