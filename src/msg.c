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

static void msg_input_key_press (GtkWidget *widget, GdkEventKey *event)
{
	GtkText *text;
	guint point, len;
	guint k, s;
	HIST_ENTRY *hent = NULL;
	struct msgwin *msg = gtk_object_get_data(GTK_OBJECT(widget), "msg");

	text = GTK_TEXT(widget);

	point = gtk_text_get_point(text);
	len = gtk_text_get_length(text);

	k = event->keyval; s = event->state;
	/* handle this weird bug */
	if (s & GDK_CONTROL_MASK) {
		switch(k) {
		case 'k':
		case 'K':
			create_connect_window(0,&sessions[0]);
			break;
		}
	}
	else if (s & GDK_SHIFT_MASK && k == GDK_Return) {
		/* insert a linebreak if shift is held */
		int position;

		position = text->point.index;
		gtk_editable_insert_text (GTK_EDITABLE(text), "\n", 1, &position);
		gtk_text_set_point(text, position);
		return;
	}
	else if (k == GDK_Return) {
		char *buf;

		gtk_text_set_editable(text, 0);
		buf = gtk_editable_get_chars(GTK_EDITABLE(text), 0, len);

		add_history(msg->history, buf);
		using_history(msg->history);

		g_free(buf);
	}
	else if (k == GDK_Up) {
		hent = previous_history(msg->history);
	}
	else if (k == GDK_Down) {
		hent = next_history(msg->history);
	}

	if (hent) {
		gtk_text_freeze(text);
		gtk_text_set_point(text, 0);
		len = gtk_text_get_length(text);
		gtk_text_forward_delete(text, len);


		len = strlen(hent->line);
		gtk_text_insert(text, 0, 0, 0, hent->line, len);
		gtk_text_thaw(text);

		len = gtk_text_get_length(text);
		gtk_editable_set_position(GTK_EDITABLE(text), len);

	}
}

static void msg_update_trans (GtkWidget *win, GdkEventConfigure *event, gpointer data)
{
	GtkWidget *xtext = data;

	if(gtkhx_prefs.trans_xtext) {
		gtk_xtext_refresh(GTK_XTEXT(xtext), 1);
	}

}

static void msg_input_activate (GtkWidget *widget, gpointer data)
{
	GtkText *text;
	guint point, len;
	char *termed_buf = NULL;
	guint16 *uid = data;

	text = GTK_TEXT(widget);
	point = gtk_text_get_point(text);
	len = gtk_text_get_length(text);
	termed_buf = gtk_editable_get_chars(GTK_EDITABLE(text), 0, len);

	gtk_text_set_point(text, 0);
	gtk_text_forward_delete(text, len);
	gtk_text_set_editable(text, 1);

	if(termed_buf[0] == 0) {
		return;
	}

	/* send the plugins information that we're sending a private message
	   with content termed_buf to uid */
#ifdef USE_PLUGIN
	if(EMIT_SIGNAL(XP_SND_MSG, &sessions[0], termed_buf, &uid, 0, 0, 0) == 1) {
		return;
	}
#endif
	len = strlen(termed_buf);
	msg_output(sessions[0].htlc.name, *uid, termed_buf);
	LF2CR(termed_buf, len);
	hx_send_msg(&sessions[0].htlc, *uid, termed_buf, len, 0);
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
	msg->outputbuf = gtk_xtext_new(0,0);

	gtk_xtext_set_palette (GTK_XTEXT (msg->outputbuf), colors);
	gtk_xtext_set_font(GTK_XTEXT(msg->outputbuf), gtkhx_font, 0);
#ifdef USE_GDK_PIXBUF
	GTK_XTEXT(msg->outputbuf)->tint_red = gtkhx_prefs.tint_red;
	GTK_XTEXT(msg->outputbuf)->tint_green = gtkhx_prefs.tint_green;
	GTK_XTEXT(msg->outputbuf)->tint_blue = gtkhx_prefs.tint_blue;
#endif
	GTK_XTEXT(msg->outputbuf)->wordwrap = gtkhx_prefs.word_wrap;
	GTK_XTEXT(msg->outputbuf)->urlcheck_function = word_check;
	GTK_XTEXT(msg->outputbuf)->max_lines = gtkhx_prefs.xbuf_max;

	gtk_xtext_set_background(GTK_XTEXT(msg->outputbuf), 0, gtkhx_prefs.trans_xtext, 1);
	msg->vscroll = gtk_vscrollbar_new(GTK_XTEXT(msg->outputbuf)->adj);
	msg->inputbuf = gtk_text_new(0,0);

	gtk_widget_set_style(msg->inputbuf, gtktext_style);
	gtk_text_set_editable(GTK_TEXT(msg->inputbuf), 1);
	gtk_text_set_word_wrap(GTK_TEXT(msg->inputbuf), 1);

	gtk_object_set_data(GTK_OBJECT(msg->inputbuf), "msg", msg);
	gtk_object_set_data(GTK_OBJECT(msg->inputbuf), "sess", &sessions[0]);
	gtk_signal_connect(GTK_OBJECT(msg->inputbuf), "key_press_event", 
					   GTK_SIGNAL_FUNC(msg_input_key_press), 0);
	gtk_signal_connect(GTK_OBJECT(msg->inputbuf), "activate", 
					   GTK_SIGNAL_FUNC(msg_input_activate), uid);
	gtk_signal_connect(GTK_OBJECT(msg->window), "configure_event", 
					   GTK_SIGNAL_FUNC(msg_update_trans), msg->outputbuf);

	msg_list = msg;
	return msg;
}

void destroy_msgwin (GtkWidget *widget, gpointer data)
{
	struct msgwin *msg = gtk_object_get_data(GTK_OBJECT(widget), "msg");
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

	gtk_widget_set_usize(msg->window, 412, 280);
	gtk_window_set_policy(GTK_WINDOW(msg->window), 1, 1, 0);
	gtk_container_set_border_width(GTK_CONTAINER(msg->window), 0);
	hbox = gtk_hbox_new(0,0);
	gtk_widget_set_usize(hbox, 500, 400);

	outputframe = gtk_frame_new(0);
	gtk_frame_set_shadow_type(GTK_FRAME(outputframe), GTK_SHADOW_IN);
	gtk_container_add(GTK_CONTAINER(outputframe), hbox);
	gtk_box_pack_start(GTK_BOX(hbox), msg->outputbuf, 1, 1, 0);
	gtk_box_pack_start(GTK_BOX(hbox), msg->vscroll, 0, 0, 0);

	inputframe = gtk_frame_new(0);
	gtk_frame_set_shadow_type(GTK_FRAME(inputframe), GTK_SHADOW_IN);
	gtk_container_add(GTK_CONTAINER(inputframe), msg->inputbuf);
	gtk_widget_set_usize(inputframe, 0, 40);
	gtk_widget_set_usize(msg->inputbuf, 0, 40);

	vpane = gtk_vpaned_new();
	gtk_paned_pack1(GTK_PANED(vpane), outputframe, 0, 1);
	gtk_paned_pack2(GTK_PANED(vpane), inputframe, 0, 1);
	gtk_paned_set_position(GTK_PANED(vpane), 230);
	gtk_container_set_border_width(GTK_CONTAINER(vpane), 5);


	gtk_container_add(GTK_CONTAINER(msg->window), vpane);


	gtk_widget_show_all(msg->window);

	gtk_object_set_data(GTK_OBJECT(msg->window), "msg", msg);
	gtk_signal_connect(GTK_OBJECT(msg->window), "delete_event", GTK_SIGNAL_FUNC(destroy_msgwin), 0);
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


	brack_col = !(strcmp(name, sessions[0].htlc.name)) ? 13: 12;


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
	GtkWidget *dialog = (GtkWidget *)gtk_object_get_data(GTK_OBJECT(widget), "dialog");
	gtk_widget_destroy(dialog);
}

void broadcastmsg(char *text)
{

	GtkWidget *hbox;
	GtkWidget *dialog;
	GtkWidget *okbtn;
	GtkWidget *textbox;
	GtkWidget *vscroll;


	dialog = gtk_dialog_new();
	okbtn = gtk_button_new_with_label(_("OK"));
    GTK_WIDGET_SET_FLAGS (okbtn, GTK_CAN_DEFAULT);

	textbox = gtk_text_new(0,0);
	vscroll = gtk_vscrollbar_new(GTK_TEXT(textbox)->vadj);
	hbox = gtk_hbox_new(0,0);
	gtk_widget_set_usize(dialog, 300, 250);
	gtk_widget_set_usize(textbox, 280, 220);
    gtk_window_set_title(GTK_WINDOW(dialog), _("Broadcast"));

    gtk_box_pack_start (GTK_BOX (GTK_DIALOG (dialog)->vbox), hbox, TRUE, TRUE , 0);
	gtk_box_pack_start(GTK_BOX(hbox), textbox, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(hbox), vscroll, 0, 0, 0);
	gtk_box_pack_start (GTK_BOX (GTK_DIALOG(dialog) ->action_area), okbtn, TRUE, TRUE, 0);
    gtk_widget_grab_default (okbtn);
	gtk_text_insert(GTK_TEXT(textbox), 0, 0, 0, text, strlen(text));
	gtk_object_set_data(GTK_OBJECT(okbtn), "dialog", dialog);
	gtk_signal_connect(GTK_OBJECT(okbtn), "clicked", GTK_SIGNAL_FUNC(broadcastok), 0);
	init_keyaccel(dialog);
	gtk_widget_show_all(dialog);
}
