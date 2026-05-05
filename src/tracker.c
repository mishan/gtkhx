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
#include "regex.h"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include "hx.h"
#include "gtkhx.h"
#include "gtkutil.h"
#include "connect.h"
#include "gtk_hlist.h"
#include "dfa.h"
#include "chat.h"

static GtkWidget *tracker_window;
static GtkWidget *tracker_list;
static GtkWidget *lbl_found, *lbl_total;
static int num_found, num_total;
static struct dfa *current_search;

struct tracker_server {
	char *name;
	char *desc;

	guint16 nusers;
	guint16 port;

	struct in_addr addr;
	struct tracker_server *left, *right;
};


struct tracker_server *tracker_server_tree = NULL;

static void tracker_list_destroy(struct tracker_server *root)
{
	if(!root) {
		return;
	}

	tracker_list_destroy(root->left);
	tracker_list_destroy(root->right);

	if(root->name)
		g_free(root->name);
	if(root->desc)
		g_free(root->desc);
	g_free(root);

	tracker_server_tree = NULL;
}

void tracker_clear (void)
{
	gtk_hlist_clear(GTK_HLIST(tracker_list));
}

static void
close_tracker_window (GtkWidget *widget, gpointer data)
{
	tracker_clear();
	gtkhx_widget_destroy(widget);
	tracker_window = 0;
	tracker_list = 0;

	tracker_list_destroy(tracker_server_tree);
	dfafree(current_search);
	current_search = NULL;
}

pthread_t track_tid = 0;

void tracker_kill_threads(void)
{
	if(track_tid) {
#ifdef USING_DARWIN
		kill(track_tid, SIGUSR1);
#else
		pthread_kill(track_tid, SIGUSR1);
#endif
		track_tid = 0;
	}
}

void tracker_sighandler (int signum)
{
	pthread_exit(0);
}

static void tracker_getlist_thread(void *data)
{
	int i;
	session *sess = data;

	signal(SIGUSR1, &tracker_sighandler);

#ifdef USING_DARWIN
	track_tid = getpid();
#endif


	for(i = 0; i < gtkhx_prefs.num_tracker; i++) {
		hx_tracker_list(sess, gtkhx_prefs.tracker[i], HTRK_TCPPORT);
	}
}

static void
tracker_getlist (GtkWidget *widget, gpointer data)
{
	session *sess = data;
	pthread_attr_t attr;


	if(track_tid) {
		pthread_cancel(track_tid);
		track_tid = 0;
	}

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

	tracker_clear();
	num_found = 0;
	num_total = 0;

	tracker_list_destroy(tracker_server_tree);


	gtk_label_set_text(GTK_LABEL(lbl_found), "  0");
	gtk_label_set_text(GTK_LABEL(lbl_total), " / 0");

	pthread_create(&track_tid, &attr, (void*(*)(void*))tracker_getlist_thread, sess);
}


static void tracker_search_tree (struct dfa *preg, struct tracker_server *root)
{
	int row;
	char nusersstr[8], portstr[8], *text[5];
	int namelen, desclen;
	char flag;
	GdkRGBA col = {0, 0, 0, 1};

	if(!root) {
		return;
	}
	/* Defensive: the worker thread feeds new servers through here as
	 * tracker_server_create runs, so a stray result that arrives before
	 * current_search is set up (or after teardown) would deref NULL in
	 * dfaexec. The window's first-open path now initializes preg before
	 * spawning the worker, but keep the guard for paranoia. */
	if (!preg)
		return;

	namelen = strlen(root->name);
	desclen = strlen(root->desc);
	flag = dfaexec(preg, root->name, &root->name[namelen], 0, NULL, NULL)
		|| dfaexec(preg, root->desc, &root->desc[desclen], 0, NULL, NULL);
	root->name[namelen] = '\0';
	root->desc[desclen] = '\0';
	if (flag) {
		snprintf(nusersstr, sizeof(nusersstr), "%u", root->nusers);
		snprintf(portstr, sizeof(portstr), "%u", root->port);

		text[0] = root->name;
		text[1] = nusersstr;
		text[2] = g_malloc(HOSTLEN);

		inet_ntop(AF_INET, &root->addr, text[2], HOSTLEN);

		text[3] = portstr;
		text[4] = root->desc;


		row = gtk_hlist_append(GTK_HLIST(tracker_list), text);
		gtk_hlist_set_foreground(GTK_HLIST(tracker_list), row, &col);
		g_free(text[2]);
		gtk_hlist_set_row_data(GTK_HLIST(tracker_list), row, root);
		num_found++;
	}

	tracker_search_tree(preg, root->left);
	tracker_search_tree(preg, root->right);
}

void tracker_search  (GtkWidget *widget, gpointer data)
{
	char *num, *str;

	dfafree(current_search);
	current_search = g_malloc(sizeof(struct dfa));

	num_found = 0;
	str = gtk_editable_get_text(GTK_EDITABLE(widget));
	dfacomp(str, strlen(str), current_search, 1);
	tracker_clear();
	gtk_hlist_freeze(GTK_HLIST(tracker_list));

	tracker_search_tree(current_search, tracker_server_tree);

	gtk_hlist_thaw(GTK_HLIST(tracker_list));
	num = g_strdup_printf("  %d", num_found);
	gtk_label_set_text(GTK_LABEL(lbl_found), num);
	g_free(num);
}


int find_server (struct in_addr addr, guint16 port, struct tracker_server *root)
{
	/* a one-liner to traverse a binary search tree ...
	   kids: don't do this for your computer science course! */
	return (root && (((root->addr.s_addr == addr.s_addr) && (root->port == port)) || (find_server(addr, port, (root->addr.s_addr == addr.s_addr) ? ((root->port > port) ? root->left : root->right) : ((root->addr.s_addr > addr.s_addr) ? root->left : root->right)))));
}

void insert_server (struct tracker_server *server, struct tracker_server *root)
{
	/* tree is null...set this server as the root */
	if(!tracker_server_tree) {
		tracker_server_tree = server;
		server->left = 0;
		server->right = 0;
		return;
	}

	if(root->addr.s_addr == server->addr.s_addr) {
		if(root->port > server->port) {
			if(root->left) {
				insert_server(server, root->left);
				return;
			}
			root->left = server;
			return;
		}
		if(root->right) {
			insert_server(server, root->right);
			return;
		}
		root->right = server;
		return;
	}


	else if(root->addr.s_addr > server->addr.s_addr) {
		if(root->left) {
			insert_server(server, root->left);
			return;
		}
		root->left = server;
		return;
	}

	if(root->right) {
		insert_server(server, root->right);
		return;
	}

	root->right = server;
	return;
}

void
tracker_server_create (struct in_addr addr, guint16 port, guint16 nusers,
		       const char *nam, const char *desc, int total)
{
	struct tracker_server *server;
	char *num;
	int old_num_found;

	if (!tracker_list)
		return;

	if(find_server(addr, port, tracker_server_tree)) {
		return;
	}

	num_total++;
	server = g_malloc(sizeof(struct tracker_server));
	server->addr = addr;
	server->port = port;
	server->nusers = nusers;
	/* Phase 3.x: server names from old Hotline trackers are MacRoman-
	 * encoded. Pango requires UTF-8; without conversion any non-ASCII
	 * byte (very common in Mac-era server names) makes
	 * gtk_label_set_text → pango_layout_set_text print the
	 * "Invalid UTF-8 string passed to pango_layout_set_text" critical.
	 * Convert with fallback so unmappable bytes degrade to '?' rather
	 * than dropping the whole row. */
	server->name = g_convert_with_fallback(nam,   -1, "UTF-8", "MACINTOSH",
	                                       "?", NULL, NULL, NULL);
	if (!server->name)
		server->name = g_strdup(nam ? nam : "");
	server->desc = g_convert_with_fallback(desc,  -1, "UTF-8", "MACINTOSH",
	                                       "?", NULL, NULL, NULL);
	if (!server->desc)
		server->desc = g_strdup(desc ? desc : "");
	server->left = 0;
	server->right = 0;
	insert_server(server, tracker_server_tree);

	old_num_found = num_found;
	tracker_search_tree(current_search, server);

	/* avoid unnecessary redraws */
	if (old_num_found != num_found)
	{
		num = g_strdup_printf("  %d", num_found);
		gtk_label_set_text(GTK_LABEL(lbl_found), num);
		g_free(num);

		num = g_strdup_printf(" / %d", num_total);
		gtk_label_set_text(GTK_LABEL(lbl_total), num);
		g_free(num);
	}
}

static int tracker_storow;
static int tracker_stocol;

/* Phase 4.5: button-press-event is gone in GTK 4. The tracker list's
 * single/double-click handling lives on a GtkGestureClick controller
 * now; the "pressed" signal fires with widget-local x/y, and n_press
 * gates the double-click connect. */
static void
tracker_pressed (GtkGestureClick *gesture, int n_press,
                 double x, double y, gpointer data)
{
	GtkWidget *widget = gtk_event_controller_get_widget (
		GTK_EVENT_CONTROLLER (gesture));
	(void) data;

	gtk_hlist_get_selection_info (GTK_HLIST (widget), (int) x, (int) y,
	                              &tracker_storow, &tracker_stocol);

	if (n_press == 2) {
		struct tracker_server *server;
		char buf[HOSTLEN];

		server = gtk_hlist_get_row_data (GTK_HLIST (tracker_list),
		                                 tracker_storow);
		if (!server)
			return;
		inet_ntop (AF_INET, &server->addr, buf, HOSTLEN);
#ifdef CONFIG_COMPRESS
		memset (the_session.htlc.compressalg, 0,
		        sizeof (the_session.htlc.compressalg));
#endif
#ifdef CONFIG_CIPHER
		memset (the_session.htlc.cipheralg, 0,
		        sizeof (the_session.htlc.cipheralg));
#endif
		hx_connect (&the_session.htlc, buf, server->port, "", "", 0);
	}
}

static void tracker_connect (void)
{
	struct tracker_server *server;

	server = gtk_hlist_get_row_data(GTK_HLIST(tracker_list), tracker_storow);
	if (server) {
		char buf[HOSTLEN];

		create_connect_window(0, &the_session);
		inet_ntop(AF_INET, &server->addr, buf, HOSTLEN);
		connect_set_entries(buf, 0, 0, server->port);
	}
}

void
create_tracker_window (GtkWidget *widget, gpointer data)
{
	GtkWidget *vbox;

	GtkWidget *hbox;
	GtkWidget *searchhbox;
	GtkWidget *searchentry;
	GtkWidget *tracker_window_scroll;
	GtkWidget *refreshbtn;
	GtkWidget *connbtn;
	GdkPixbuf *pb;
	GtkWidget *pix;
	GtkWidget *lbl_search;
	gchar *titles[5];
	session *sess = data;

	titles[0] = _("Name");
	titles[1] = _("Users");
	titles[2] = _("Address");
	titles[3] = _("Port");
	titles[4] = _("Description");

	if (tracker_window)
		return;

	tracker_window = gtk_window_new();
	/* Phase 2 cleanup: don't gtk_widget_realize() the toplevel here — it
	 * trips a GTK_WIDGET_ANCHORED assertion in GTK 2 when called before
	 * gtk_widget_show*. The legacy code did it to obtain a parent
	 * GdkWindow for gdk_pixmap_create_from_xpm_d(); we now build pixbufs
	 * directly via gdk_pixbuf_new_from_xpm_data(), which needs no
	 * drawable. */
	gtk_window_set_title(GTK_WINDOW(tracker_window), _("Tracker"));
	gtk_widget_set_size_request(tracker_window, 640, 410);
	g_signal_connect(tracker_window, "delete_event", G_CALLBACK(close_tracker_window), 0);

	tracker_list = gtk_hlist_new_with_titles(5, titles);
	gtk_widget_set_size_request(tracker_list, 0, 350);
	gtk_hlist_set_column_width(GTK_HLIST(tracker_list), 0, 160);
	gtk_hlist_set_column_width(GTK_HLIST(tracker_list), 1, 40);
	gtk_hlist_set_column_justification(GTK_HLIST(tracker_list), 1, GTK_JUSTIFY_CENTER);
	gtk_hlist_set_column_width(GTK_HLIST(tracker_list), 2, 96);
	gtk_hlist_set_column_width(GTK_HLIST(tracker_list), 3, 40);
	gtk_hlist_set_column_width(GTK_HLIST(tracker_list), 4, 1024);
	{
		/* Phase 4.5: button-press-event is gone — install a gesture
		 * controller for the double-click-to-connect path. */
		GtkGesture *click = gtk_gesture_click_new ();
		gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click),
		                               GDK_BUTTON_PRIMARY);
		g_signal_connect (click, "pressed",
		                  G_CALLBACK (tracker_pressed), NULL);
		gtk_widget_add_controller (tracker_list,
		                           GTK_EVENT_CONTROLLER (click));
	}


	searchentry = gtk_entry_new();
	g_signal_connect(searchentry, "activate", G_CALLBACK(tracker_search), 0);
	lbl_search = gtk_label_new(_("Search: "));
	lbl_found = gtk_label_new("  0");
	lbl_total = gtk_label_new(" / 0");
	num_found = 0;
	num_total = 0;

	refreshbtn = gtk_button_new();
	gtk_widget_set_tooltip_text(refreshbtn, _("Refresh"));
	pb = gdk_pixbuf_new_from_resource("/com/nasledov/gtkhx/pixmaps/refresh.xpm", NULL);
	pix = gtk_image_new_from_pixbuf(pb);
	if (pb) g_object_unref(pb);
	gtkhx_widget_set_child(refreshbtn, pix);
	pix = 0; pb = 0;
	g_signal_connect(refreshbtn, "clicked",
					   G_CALLBACK(tracker_getlist), sess);

	connbtn = gtk_button_new();
	g_signal_connect(connbtn, "clicked",
					   G_CALLBACK(tracker_connect), 0);
	gtk_widget_set_tooltip_text(connbtn, _("Connect"));
	pb = gdk_pixbuf_new_from_resource("/com/nasledov/gtkhx/pixmaps/connect.xpm", NULL);
	pix = gtk_image_new_from_pixbuf(pb);
	if (pb) g_object_unref(pb);
	gtkhx_widget_set_child(connbtn, pix);
	pix = 0; pb = 0;

	tracker_window_scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tracker_window_scroll),
				       GTK_POLICY_AUTOMATIC, GTK_POLICY_ALWAYS);
	gtk_widget_set_size_request(tracker_window_scroll, 640, 350);
	gtkhx_widget_set_child(tracker_window_scroll, tracker_list);

	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_size_request(vbox, 640, 410);
	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	searchhbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtkhx_box_pack(hbox, refreshbtn, 0, 0, 0);
	gtkhx_box_pack(hbox, connbtn, 0, 0, 0);
	gtkhx_box_pack(hbox, lbl_found, 0, 0, 0);
	gtkhx_box_pack(hbox, lbl_total, 0, 0, 0);
	gtkhx_box_pack(vbox, hbox, 0, 0, 0);
	gtkhx_box_pack(searchhbox, lbl_search, 0, 0, 0);
	gtkhx_box_pack(searchhbox, searchentry, 1, 1, 0);
	gtkhx_box_pack(vbox, searchhbox, 0, 0, 0);
	gtkhx_box_pack(vbox, tracker_window_scroll, 1, 1, 0);
	gtkhx_widget_set_child(tracker_window, vbox);
	init_keyaccel(tracker_window);
	gtk_widget_show(tracker_window);

	gtk_widget_grab_focus(searchentry);

	/* Initialize the search filter BEFORE spawning the tracker worker.
	 * Each result the worker streams in calls tracker_server_create →
	 * tracker_search_tree(current_search, ...), which dereferences
	 * current_search inside dfaexec. The legacy ordering raced: the
	 * worker could fire faster than the synchronous init below it. */
	current_search = g_malloc(sizeof(struct dfa));
	dfacomp("", 0, current_search, 1);

	tracker_getlist(0, sess);
}

void dfaerror (const char *mesg)
{
/*	g_warning("%s\n", mesg); */
	hx_printf_prefix(&the_session.htlc, 0, INFOPREFIX, "Tracker Regexps: %s\n", mesg);
}
