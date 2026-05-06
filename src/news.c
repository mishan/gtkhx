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


static GtkWidget *post_window;
static GtkWidget *postprompt;

GtkWidget *reloadButton;
GtkWidget *postButton;

/* Phase 5: gtkhx_text_to_utf8 → gtkhx_text_to_utf8 (moved to
 * gtkutil — server names, news bodies, post subjects all want the
 * same Mac Roman / Latin-1 / already-UTF-8 fallback chain). The
 * old name in this TU is gone; callers just include gtkutil.h. */

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
		GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(sess->news_text));
		gtk_text_buffer_set_text(buf, "", -1);
		hx_get_news(&sess->htlc);
	}
}

/* Phase 4.5: GTK 4 close-request on (GtkWindow *, gpointer); FALSE
 * allows default destroy. */
static gboolean close_news_window (GtkWindow *window, gpointer data)
{
	session *sess = data;
	(void) window;

	gtkhx_prefs.geo.news.open = 0;
	gtkhx_prefs.geo.news.init = 0;
	sess->news_window = 0;
	return FALSE;
}

/* Phase 4.5: configure-event is gone in GTK 4. News window size is
 * captured at hx_quit() time alongside position; see gtkhx.c
 * gtkhx_save_window_positions. */

static gboolean close_post_window (GtkWindow *window, gpointer data)
{
	(void) window; (void) data;
	post_window = 0;
	return FALSE;
}

static void post_news (GtkWidget *widget, gpointer data)
{
	char *posttext;
	session *sess = data;
	int len;
	GtkTextBuffer *buf;
	GtkTextIter start, end;

	buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(postprompt));
	gtk_text_buffer_get_start_iter(buf, &start);
	gtk_text_buffer_get_end_iter(buf, &end);
	posttext = gtk_text_buffer_get_text(buf, &start, &end, FALSE);
	len = strlen(posttext);
	LF2CR(posttext, len);
	if (len > 0 && posttext[len - 1] == '\r')
		posttext[len - 1] = 0;
	hx_post_news(&sess->htlc, posttext, len);

	g_free(posttext);
	gtkhx_widget_destroy(post_window);
	post_window = 0;
}

void
create_post_window (GtkWidget *widget, gpointer data)
{
	GtkWidget *okbut;
	GtkWidget *cancbut;
	GtkWidget *vbox, *hbox;
	session *sess = data;

	post_window = gtk_window_new();
	gtk_window_set_title(GTK_WINDOW(post_window), _("Post News"));
	gtk_widget_set_size_request(post_window, 300, 280);
	g_signal_connect(post_window, "close-request",
			   G_CALLBACK(close_post_window), 0);

	postprompt = gtk_text_view_new();
	gtk_text_view_set_editable(GTK_TEXT_VIEW(postprompt), TRUE);
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(postprompt), GTK_WRAP_WORD);
	gtkhx_apply_text_style(postprompt);

	{
		GtkWidget *post_scroll = gtk_scrolled_window_new();
		gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(post_scroll),
		                               GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
		gtkhx_widget_set_child(post_scroll, postprompt);
		gtk_widget_set_size_request(post_scroll, 0, 260);

		vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
		gtkhx_widget_set_child(post_window, vbox);
		gtkhx_box_pack(vbox, post_scroll, 0, 0, 0);
	}

	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtkhx_box_pack(vbox, hbox, 0, 0, 0);

	okbut = gtk_button_new_with_label(_("OK"));
	g_signal_connect(okbut, "clicked",
			   G_CALLBACK(post_news), sess);
	cancbut = gtk_button_new_with_label(_("Cancel"));
	g_signal_connect(cancbut, "clicked",

			   G_CALLBACK(close_post_window), 0);


	gtkhx_box_pack(hbox, okbut, 0, 0, 0);
	gtkhx_box_pack(hbox, cancbut, 0, 0, 0);

	gtk_window_present(GTK_WINDOW(post_window));
	gtk_widget_grab_focus(postprompt);
}

void create_news_window (session *sess)
{
	GtkWidget *news_scroll;
	GtkWidget *hbox;
	GtkWidget *news_frame;
	GtkWidget *news_text;
	GtkWidget *news_window;
	GtkWidget *postButton, *reloadButton;


	if (gtkhx_prefs.geo.news.open) {
		gtk_window_present(GTK_WINDOW(sess->news_window));
		return;
	}

	news_window = gtk_window_new();

	/* Phase 3.x: dropped GTK 1.2-era realize+get_style pair (style unused). */

	/* Phase 5: 2x-scaled headerbar buttons via the shared helper. */
	postButton = gtkhx_pixmap_button (
		"/com/nasledov/gtkhx/pixmaps/postnews.xpm",
		_("Post News"), 2, NULL, NULL);
	reloadButton = gtkhx_pixmap_button (
		"/com/nasledov/gtkhx/pixmaps/refresh.xpm",
		_("Reload News"), 2, NULL, NULL);

	gtk_window_set_resizable(GTK_WINDOW(news_window), TRUE);

	gtk_window_set_title(GTK_WINDOW(news_window), _("News"));
	gtk_widget_set_size_request(news_window, 412, 384);
	g_signal_connect(news_window, "close-request",
			   G_CALLBACK(close_news_window), sess);
	g_signal_connect(postButton, "clicked",
					   G_CALLBACK(create_post_window), sess);
	g_signal_connect(reloadButton, "clicked",
					   G_CALLBACK(reload_news), sess);

	/* Phase 5: AdwHeaderBar replaces both the default GtkWindow title
	 * bar and the in-content btn_frame + posthbox row. Post on the
	 * start, Reload on the end. */
	{
		GtkWidget *header = adw_header_bar_new ();
		adw_header_bar_pack_start (ADW_HEADER_BAR (header), postButton);
		adw_header_bar_pack_end   (ADW_HEADER_BAR (header), reloadButton);
		gtk_window_set_titlebar (GTK_WINDOW (news_window), header);
	}

	news_frame = gtk_frame_new(0);
	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtkhx_widget_set_child(news_frame, hbox);
	gtk_widget_set_size_request(hbox, 512, 384);
	gtk_window_set_child(GTK_WINDOW(news_window), news_frame);

	news_text = gtk_text_view_new();
	gtk_text_view_set_editable(GTK_TEXT_VIEW(news_text), FALSE);
	gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(news_text), FALSE);
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(news_text), GTK_WRAP_WORD);
	gtkhx_apply_text_style(news_text);

	news_scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(news_scroll),
	                               GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtkhx_widget_set_child(news_scroll, news_text);
	gtkhx_box_pack(hbox, news_scroll, 1, 1, 0);
	gtk_widget_set_sensitive(postButton, FALSE);
	gtk_widget_set_sensitive(reloadButton, FALSE);
	
	/* Phase 3.x: only apply saved geometry when the prefs file actually
	 * has one (see users.c for rationale — zero-size collapses the
	 * window under GTK 3). */
	if (gtkhx_prefs.geo.news.xsize > 0 && gtkhx_prefs.geo.news.ysize > 0)
		gtk_window_set_default_size(GTK_WINDOW(news_window),
		                            gtkhx_prefs.geo.news.xsize,
		                            gtkhx_prefs.geo.news.ysize);
	if (gtkhx_prefs.geo.news.xpos > 0 || gtkhx_prefs.geo.news.ypos > 0)
		/* Phase 4.2: gtk_window_move removed (Wayland) */

	gtk_window_present(GTK_WINDOW(news_window));

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
		gtk_window_present(GTK_WINDOW(sess->news_window));
		gtk_widget_grab_focus(sess->news_window);
	}
}

void output_news_post (struct htlc_conn *htlc, char *news, guint16 len)
{
	session *sess;


	if (!gtkhx_prefs.geo.news.open) {
		return;
	}

	sess = &the_session;
	if(!sess) {
		return;
	}

	{
		GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(sess->news_text));
		GtkTextIter start;
		gsize utf8_len;
		char *utf8 = gtkhx_text_to_utf8(news, len, &utf8_len);
		gtk_text_buffer_get_start_iter(buf, &start);
		gtk_text_buffer_insert(buf, &start, utf8, (gint) utf8_len);
		g_free(utf8);
	}
}

void output_news_file (struct htlc_conn *htlc, char *news, guint16 len)
{
	session *sess;
	GtkTextBuffer *buf;
	GtkTextIter end;
	gsize utf8_len;
	char *utf8;

	if(!gtkhx_prefs.geo.news.open)
		return;

	sess = &the_session;
	if(!sess) {
		return;
	}

	buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(sess->news_text));
	gtk_text_buffer_get_end_iter(buf, &end);
	utf8 = gtkhx_text_to_utf8(news, len, &utf8_len);
	gtk_text_buffer_insert(buf, &end, utf8, (gint) utf8_len);
	g_free(utf8);
}



