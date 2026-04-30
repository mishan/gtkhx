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
#include <stdlib.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <netinet/in.h>
#include "hx.h"
#include "chat.h"
#include "gtkhx.h"
#include "gtkutil.h"
#include "history.h"
#include "xtext.h"
#include "plugin.h"
#include "rcv.h"
#include "tasks.h"
#include "connect.h"

void
hx_send_msg (struct htlc_conn *htlc, guint16 uid, const char *msg, guint16 len, void *data)
{
	uid = htons(uid);
	task_new(htlc, rcv_task_msg, data, 0, data ? data : "msg");
	hlwrite(htlc, HTLC_HDR_MSG, 0, 2,
		HTLC_DATA_UID, 2, &uid,
		HTLC_DATA_MSG, len, msg);
}

struct msgwin *msg_list;
void msg_output (char *name, guint16 uid, char *buf);

static void msgwin_delete (struct msgwin *msg)
{
	if (msg->next)
		msg->next->prev = msg->prev;
	if (msg->prev)
		msg->prev->next = msg->next;
	if (msg == msg_list)
		msg_list = msg->prev;

	g_free(msg->name);
	g_free(msg->uid);
	g_free(msg);
}


struct msgwin *msgwin_with_uid (guint16 uid)
{
	struct msgwin *msg;


	for (msg = msg_list; msg; msg = msg->prev) {
		if (*(msg->uid) == uid)
			return msg;

	}


	return 0;
}

static void msg_input_activate (GtkWidget *widget, gpointer data);

static gboolean msg_input_key_press (GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
	GtkTextView *text;
	GtkTextBuffer *buf;
	guint k, s;
	HIST_ENTRY *hent = NULL;
	struct msgwin *msg = g_object_get_data(G_OBJECT(widget), "msg");

	text = GTK_TEXT_VIEW(widget);
	buf = gtk_text_view_get_buffer(text);

	k = event->keyval; s = event->state;
	/* handle this weird bug */
	if (s & GDK_CONTROL_MASK) {
		switch(k) {
		case 'k':
		case 'K':
			create_connect_window(0,&the_session);
			break;
		}
	}
	else if (s & GDK_SHIFT_MASK && k == GDK_Return) {
		/* insert a linebreak if shift is held — let GtkTextView default */
		return FALSE;
	}
	else if (k == GDK_Return) {
		GtkTextIter start, end;
		char *line;

		gtk_text_buffer_get_start_iter(buf, &start);
		gtk_text_buffer_get_end_iter(buf, &end);
		line = gtk_text_buffer_get_text(buf, &start, &end, FALSE);
		add_history(msg->history, line);
		using_history(msg->history);
		g_free(line);

		msg_input_activate(widget, msg->uid);
		g_signal_stop_emission_by_name(widget, "key_press_event");
		return TRUE;
	}
	else if (k == GDK_Up) {
		hent = previous_history(msg->history);
	}
	else if (k == GDK_Down) {
		hent = next_history(msg->history);
	}

	if (hent) {
		GtkTextIter end;

		gtk_text_buffer_set_text(buf, hent->line, strlen(hent->line));
		gtk_text_buffer_get_end_iter(buf, &end);
		gtk_text_buffer_place_cursor(buf, &end);
		g_signal_stop_emission_by_name(widget, "key_press_event");
		return TRUE;
	}

	return FALSE;
}

static void msg_update_trans (GtkWidget *win, GdkEventConfigure *event, gpointer data)
{
	GtkWidget *xtext = data;

	if(gtkhx_prefs.trans_xtext) {
		gtk_xtext_refresh(GTK_XTEXT(xtext));
	}

}

static void msg_input_activate (GtkWidget *widget, gpointer data)
{
	GtkTextView *text;
	GtkTextBuffer *buf;
	GtkTextIter start, end;
	guint len;
	char *termed_buf = NULL;
	guint16 *uid = data;

	text = GTK_TEXT_VIEW(widget);
	buf = gtk_text_view_get_buffer(text);
	gtk_text_buffer_get_start_iter(buf, &start);
	gtk_text_buffer_get_end_iter(buf, &end);
	termed_buf = gtk_text_buffer_get_text(buf, &start, &end, FALSE);

	gtk_text_buffer_delete(buf, &start, &end);

	if(termed_buf[0] == 0) {
		g_free(termed_buf);
		return;
	}

	/* send the plugins information that we're sending a private message
	   with content termed_buf to uid */
#ifdef USE_PLUGIN
	if(EMIT_SIGNAL(XP_SND_MSG, &the_session, termed_buf, &uid, 0, 0, 0) == 1) {
		return;
	}
#endif
	len = strlen(termed_buf);
	msg_output(the_session.htlc.name, *uid, termed_buf);
	LF2CR(termed_buf, len);
	hx_send_msg(&the_session.htlc, *uid, termed_buf, len, 0);
	g_free(termed_buf);
}


static struct msgwin *create_msg (guint16 _uid, char *name)
{
	struct msgwin *msg;
	guint16 *uid = g_malloc(sizeof(guint16));
	*uid = _uid;

 	msg = g_malloc(sizeof(struct msgwin));

	msg->next = 0;
	msg->prev = msg_list;
	if(msg_list) {
		msg_list->next = msg;
	}
	msg->name = g_strdup(name);
	msg->uid = uid;
	
	msg->history = history_new();

	msg->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	{
		gchar *fontname = pango_font_description_to_string (gtkhx_font_desc);
		msg->outputbuf = gtk_xtext_new (colors, 0);
		gtk_xtext_set_font (GTK_XTEXT (msg->outputbuf), fontname);
		g_free (fontname);
	}
	GTK_XTEXT(msg->outputbuf)->wordwrap = gtkhx_prefs.word_wrap;
	GTK_XTEXT(msg->outputbuf)->urlcheck_function = word_check;
	GTK_XTEXT(msg->outputbuf)->max_lines = gtkhx_prefs.xbuf_max;

	gtk_xtext_set_background(GTK_XTEXT(msg->outputbuf), NULL);
	msg->vscroll = gtk_vscrollbar_new(GTK_XTEXT(msg->outputbuf)->adj);
	msg->inputbuf = gtk_text_view_new();

	gtkhx_apply_text_style(msg->inputbuf);
	gtk_text_view_set_editable(GTK_TEXT_VIEW(msg->inputbuf), TRUE);
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(msg->inputbuf), GTK_WRAP_WORD);

	g_object_set_data(G_OBJECT(msg->inputbuf), "msg", msg);
	g_object_set_data(G_OBJECT(msg->inputbuf), "sess", &the_session);
	/* Note: GtkTextView has no "activate" signal — Return is dispatched
	   from msg_input_key_press, which calls msg_input_activate(). */
	g_signal_connect(msg->inputbuf, "key_press_event",
					   G_CALLBACK(msg_input_key_press), uid);
	g_signal_connect(msg->window, "configure_event",
					   G_CALLBACK(msg_update_trans), msg->outputbuf);

	msg_list = msg;
	return msg;
}

void destroy_msgwin (GtkWidget *widget, gpointer data)
{
	struct msgwin *msg = g_object_get_data(G_OBJECT(widget), "msg");
	msgwin_delete(msg);
	gtk_widget_destroy(widget);
}


struct msgwin *create_msgwin (guint16 uid, char *name)
{
	GtkWidget *hbox;
	GtkWidget *outputframe, *inputframe;
	GtkWidget *vpane;
	struct msgwin *msg;
	char *title;

	msg = create_msg(uid, name);

	title = g_strdup_printf("%s (%u)", name, uid);
	gtk_window_set_wmclass(GTK_WINDOW(msg->window), "msg", "GtkHx");
	gtk_window_set_title(GTK_WINDOW(msg->window), title);
	g_free(title);

	gtk_widget_set_size_request(msg->window, 412, 280);
	gtk_window_set_policy(GTK_WINDOW(msg->window), 1, 1, 0);
	gtk_container_set_border_width(GTK_CONTAINER(msg->window), 0);
	hbox = gtk_hbox_new(0,0);
	gtk_widget_set_size_request(hbox, 500, 400);

	outputframe = gtk_frame_new(0);
	gtk_frame_set_shadow_type(GTK_FRAME(outputframe), GTK_SHADOW_IN);
	gtk_container_add(GTK_CONTAINER(outputframe), hbox);
	gtk_box_pack_start(GTK_BOX(hbox), msg->outputbuf, 1, 1, 0);
	gtk_box_pack_start(GTK_BOX(hbox), msg->vscroll, 0, 0, 0);

	inputframe = gtk_frame_new(0);
	gtk_frame_set_shadow_type(GTK_FRAME(inputframe), GTK_SHADOW_IN);
	gtk_container_add(GTK_CONTAINER(inputframe), msg->inputbuf);
	gtk_widget_set_size_request(inputframe, 0, 40);
	gtk_widget_set_size_request(msg->inputbuf, 0, 40);

	vpane = gtk_vpaned_new();
	gtk_paned_pack1(GTK_PANED(vpane), outputframe, 0, 1);
	gtk_paned_pack2(GTK_PANED(vpane), inputframe, 0, 1);
	gtk_paned_set_position(GTK_PANED(vpane), 230);
	gtk_container_set_border_width(GTK_CONTAINER(vpane), 5);


	gtk_container_add(GTK_CONTAINER(msg->window), vpane);


	gtk_widget_show_all(msg->window);

	g_object_set_data(G_OBJECT(msg->window), "msg", msg);
	g_signal_connect(msg->window, "delete_event", G_CALLBACK(destroy_msgwin), 0);
	init_keyaccel(msg->window);

	gtk_widget_grab_focus(msg->inputbuf);


	if(gtkhx_prefs.showback) {
		gdk_window_lower(msg->window->window);
	}

	return msg;
}


void msg_output (char *name, guint16 uid, char *buf)
{
	struct msgwin *msg;
	char *text;
	char *ptr;
	char *cr;
	int brack_col;


	brack_col = !(strcmp(name, the_session.htlc.name)) ? 13: 12;


	text = g_strdup_printf("\003%d<\003%s\003%d>\003 %s", brack_col, name, brack_col, buf);

	msg = msgwin_with_uid(uid);
	if(!msg) {
		msg = create_msgwin(uid, name);
	}
	ptr = text;

	cr = strchr(text, '\n');
	if(cr) {
		while(1) {
			xprintline(msg->outputbuf, text, cr-text);


			text = cr + 1;
			if(*text == 0) {
				break;
			}
			cr = strchr(text, '\n');
			if(!cr) {
				xprintline(msg->outputbuf, text, -1);
				break;
			}
		}
	}
	else {
		xprintline(msg->outputbuf, text, -1);
	}

	g_free(ptr);
}


void broadcastok(GtkWidget *widget, gpointer data)
{
	GtkWidget *dialog = (GtkWidget *)g_object_get_data(G_OBJECT(widget), "dialog");
	gtk_widget_destroy(dialog);
}

void broadcastmsg(char *text)
{

	GtkWidget *dialog;
	GtkWidget *okbtn;
	GtkWidget *textbox;
	GtkWidget *scroll;
	GtkTextBuffer *tbuf;


	dialog = gtk_dialog_new();
	okbtn = gtk_button_new_with_label(_("OK"));
    gtk_widget_set_can_default(okbtn, TRUE);

	textbox = gtk_text_view_new();
	gtk_text_view_set_editable(GTK_TEXT_VIEW(textbox), FALSE);
	gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(textbox), FALSE);
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(textbox), GTK_WRAP_WORD);
	tbuf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textbox));
	gtk_text_buffer_set_text(tbuf, text, strlen(text));

	scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
	                               GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(scroll), textbox);

	gtk_widget_set_size_request(dialog, 300, 250);
    gtk_window_set_title(GTK_WINDOW(dialog), _("Broadcast"));

    gtk_box_pack_start (GTK_BOX (GTK_DIALOG (dialog)->vbox), scroll, TRUE, TRUE , 0);
	gtk_box_pack_start (GTK_BOX (GTK_DIALOG(dialog) ->action_area), okbtn, TRUE, TRUE, 0);
    gtk_widget_grab_default (okbtn);
	g_object_set_data(G_OBJECT(okbtn), "dialog", dialog);
	g_signal_connect(okbtn, "clicked", G_CALLBACK(broadcastok), 0);
	init_keyaccel(dialog);
	gtk_widget_show_all(dialog);
}
