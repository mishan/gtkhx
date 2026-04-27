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
#include <gdk/gdk.h>
#include <sys/time.h>
#include <time.h>
#include <netinet/in.h>

#include "hx.h"
#include "network.h"
#include "gtkutil.h"
#include "gtkhx.h"
#include "tasks.h"
#include "rcv.h"

#include "pixmaps/postnews.xpm"
#include "pixmaps/refresh.xpm"

static GtkWidget *post_window;
static GtkWidget *postprompt;

GtkWidget *reloadButton;
GtkWidget *postButton;

void hx_get_news (struct htlc_conn *htlc)
{
	task_new(htlc, rcv_task_news_file, 0, 0, "news");
	hlwrite(htlc, HTLC_HDR_NEWS_GETFILE, 0, 0);
}

void hx_post_news (struct htlc_conn *htlc, const char *news, guint16 len)
{
	task_new(htlc, 0, 0, 0, "post");
	hlwrite(htlc, HTLC_HDR_NEWS_POST, 0, 1,
		HTLC_DATA_NEWS_POST, len, news);
}

void reload_news (GtkWidget *widget, gpointer data)
{
	session *sess = data;

	if(gtkhx_prefs.geo.news.open) {
		gtk_editable_delete_text(GTK_EDITABLE(sess->news_text), 0, -1);
		hx_get_news(&sess->htlc);
	}
}

static void close_news_window (GtkWidget *widget, gpointer data)
{
	session *sess = data;

	gtk_widget_destroy(widget);
	gtkhx_prefs.geo.news.open = 0;
	gtkhx_prefs.geo.news.init = 0;
	sess->news_window = 0;
}

static void news_move(GtkWidget *w, GdkEventConfigure *e, gpointer data)
{
	int x, y, width, height;
	session *sess = data;

	gdk_window_get_root_origin(sess->news_window->window, &x, &y);
	gdk_window_get_size(sess->news_window->window, &width, &height);

	if(e->send_event) { /* Is a position event */
		gtkhx_prefs.geo.news.xpos = x;
		gtkhx_prefs.geo.news.ypos = y;
	}

	else { /* Is a size event */
		gtkhx_prefs.geo.news.xsize = width;
		gtkhx_prefs.geo.news.ysize = height;
	}
}

static void close_post_window (GtkWidget *widget, gpointer data)
{
	gtk_widget_destroy(post_window);
	post_window = 0;
}

static void post_news (GtkWidget *widget, gpointer data)
{
	char *posttext;
	session *sess = data;
	int len;

	posttext = gtk_editable_get_chars(GTK_EDITABLE(postprompt), 0, -1);
	len = strlen(posttext);
	LF2CR(posttext, len);
	if (posttext[len - 1] == '\r')
		posttext[len - 1] = 0;
	hx_post_news(&sess->htlc, posttext, len);

	g_free(posttext);
	gtk_widget_destroy(post_window);
	post_window = 0;
}

void
create_post_window (GtkWidget *widget, gpointer data)
{
	GtkWidget *okbut;
	GtkWidget *cancbut;
	GtkWidget *vbox, *hbox;
	session *sess = data;

	post_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_wmclass(GTK_WINDOW(post_window), "post", "GtkHx");
	gtk_window_set_title(GTK_WINDOW(post_window), _("Post News"));
	gtk_widget_set_usize(post_window, 300, 280);
	gtk_signal_connect(GTK_OBJECT(post_window), "delete_event",
			   GTK_SIGNAL_FUNC(close_post_window), 0);

	postprompt = gtk_text_new(0, 0);
	gtk_text_set_editable(GTK_TEXT(postprompt), 1);
	gtk_text_set_word_wrap(GTK_TEXT(postprompt), 1);
	gtk_widget_set_style(postprompt, gtktext_style);
	gtk_widget_set_usize(postprompt, 0, 260);


	vbox = gtk_vbox_new(0, 0);
	gtk_container_add(GTK_CONTAINER(post_window), vbox);
	gtk_box_pack_start(GTK_BOX(vbox), postprompt, 0, 0, 0);

	hbox = gtk_hbox_new(0, 0);
	gtk_box_pack_start(GTK_BOX(vbox), hbox, 0, 0, 0);

	okbut = gtk_button_new_with_label(_("OK"));
	gtk_signal_connect(GTK_OBJECT(okbut), "clicked",
			   GTK_SIGNAL_FUNC(post_news), sess);
	cancbut = gtk_button_new_with_label(_("Cancel"));
	gtk_signal_connect(GTK_OBJECT(cancbut), "clicked",

			   GTK_SIGNAL_FUNC(close_post_window), 0);


	gtk_box_pack_start(GTK_BOX(hbox), okbut, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(hbox), cancbut, 0, 0, 0);

	gtk_widget_show_all(post_window);
	gtk_widget_grab_focus(postprompt);
}

void create_news_window (session *sess)
{
	GtkWidget *vscrollbar;
	GtkWidget *hbox, *vbox;
	GtkWidget *posthbox;
	GtkWidget *btn_frame, *news_frame;
	GtkWidget *news_text;
	GtkWidget *news_window;
	GtkWidget *postButton, *reloadButton;
 	GtkStyle *style;
	GdkPixmap *icon;
	GtkWidget *pix;
	GdkBitmap *mask;
	GtkTooltips *tooltips;


	if (gtkhx_prefs.geo.news.open) {
		gdk_window_raise(sess->news_window->window);
		return;
	}

	news_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_wmclass(GTK_WINDOW(news_window), "news", "GtkHx");


	gtk_widget_realize(news_window);
	style = gtk_widget_get_style(news_window);

	tooltips = gtk_tooltips_new();

	btn_frame = gtk_frame_new(0);
	gtk_widget_set_usize(btn_frame, -2, 30);
	gtk_frame_set_shadow_type(GTK_FRAME(btn_frame), GTK_SHADOW_OUT);

	news_frame = gtk_frame_new(0);
	gtk_frame_set_shadow_type(GTK_FRAME(news_frame), GTK_SHADOW_IN);

	posthbox = gtk_hbox_new(0,0);

	gtk_container_add(GTK_CONTAINER(btn_frame), posthbox);

	postButton = gtk_button_new();
	icon = gdk_pixmap_create_from_xpm_d(news_window->window, &mask, 
										&style->bg[GTK_STATE_NORMAL], 
										postnews_xpm);
	pix = gtk_pixmap_new(icon, mask);
	gtk_container_add(GTK_CONTAINER(postButton), pix);
	gtk_tooltips_set_tip(tooltips, postButton, _("Post News"), 0);
	icon = 0, pix = 0, mask = 0;

	reloadButton = gtk_button_new();
	icon = gdk_pixmap_create_from_xpm_d(news_window->window, &mask, 
										&style->bg[GTK_STATE_NORMAL], 
										refresh_xpm);
	pix = gtk_pixmap_new(icon, mask);
	gtk_container_add(GTK_CONTAINER(reloadButton), pix);
	gtk_tooltips_set_tip(tooltips, reloadButton, _("Reload News"), 0);
	icon = 0, pix = 0, mask = 0;

	gtk_window_set_policy(GTK_WINDOW(news_window), 1, 1, 0);

	gtk_window_set_title(GTK_WINDOW(news_window), _("News"));
	gtk_widget_set_usize(news_window, 412, 384);
	gtk_signal_connect(GTK_OBJECT(news_window), "delete_event",
			   GTK_SIGNAL_FUNC(close_news_window), sess);
	gtk_signal_connect(GTK_OBJECT(postButton), "clicked",

					   GTK_SIGNAL_FUNC(create_post_window), sess);
	gtk_signal_connect(GTK_OBJECT(reloadButton), "clicked",

					   GTK_SIGNAL_FUNC(reload_news), sess);
	vbox = gtk_vbox_new(0, 0);
	gtk_container_add(GTK_CONTAINER(news_window), vbox);
	hbox = gtk_hbox_new(0, 0);

	gtk_container_add(GTK_CONTAINER(news_frame), hbox);

	gtk_widget_set_usize(hbox, 512, 384);
	gtk_box_pack_start(GTK_BOX(vbox), btn_frame, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(posthbox), postButton, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(posthbox), reloadButton, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(vbox), news_frame, 1, 1, 0);

	news_text = gtk_text_new(0, 0);
	gtk_editable_set_editable(GTK_EDITABLE(news_text), 0);
	gtk_text_set_word_wrap(GTK_TEXT(news_text), 1);
	gtk_widget_set_style(news_text, gtktext_style);
	gtk_box_pack_start(GTK_BOX(hbox), news_text, 1, 1, 0);

	vscrollbar = gtk_vscrollbar_new(GTK_TEXT(news_text)->vadj);
	gtk_box_pack_start(GTK_BOX(hbox), vscrollbar, 0, 0, 0);
	gtk_widget_set_sensitive(postButton, FALSE);
	gtk_widget_set_sensitive(reloadButton, FALSE);
	gtk_signal_connect(GTK_OBJECT(news_window), "configure_event", 
					   GTK_SIGNAL_FUNC(news_move), sess);
	gtk_widget_set_usize(news_window, gtkhx_prefs.geo.news.xsize, 
						 gtkhx_prefs.geo.news.ysize);
	gtk_widget_set_uposition(news_window, gtkhx_prefs.geo.news.xpos, 
							 gtkhx_prefs.geo.news.ypos);

	gtk_widget_show_all(news_window);

	if(connected == 1) {
		changetitlespecific(news_window, _("News"));
		gtk_widget_set_sensitive(postButton, TRUE);
		gtk_widget_set_sensitive(reloadButton, TRUE);
	}

	init_keyaccel(news_window);
	gtkhx_prefs.geo.news.open = 1;
	gtkhx_prefs.geo.news.init = 1;

	sess->news_window = news_window;
	sess->news_text = news_text;
	sess->postButton = postButton;
	sess->reloadButton = reloadButton;
}

void open_news (GtkWidget *widget, gpointer data)
{
	session *sess = data;

	if(!gtkhx_prefs.geo.news.open) {
		create_news_window(sess);
		if(connected) {
			hx_get_news(&sess->htlc);
		}
	}
	else {
		gdk_window_raise(sess->news_window->window);
		gtk_widget_grab_focus(sess->news_window);
	}
}

void output_news_post (struct htlc_conn *htlc, char *news, guint16 len)
{
	session *sess;


	if (!gtkhx_prefs.geo.news.open) {
		return;
	}

	sess = sess_from_htlc(htlc);
	if(!sess) {
		return;
	}

	gtk_text_set_point(GTK_TEXT(sess->news_text), 0);
	gtk_text_insert(GTK_TEXT(sess->news_text), 0, 0, 0, news, len);
}

void output_news_file (struct htlc_conn *htlc, char *news, guint16 len)
{
	session *sess;

	if(!gtkhx_prefs.geo.news.open)
		return;

	sess = sess_from_htlc(htlc);
	if(!sess) {
		return;
	}

	gtk_text_insert(GTK_TEXT(sess->news_text), 0, 0, 0, news, len);
}



