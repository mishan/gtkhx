/*
 * Copyright (C) 2001 Misha Nasledov <misha@nasledov.com>
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
#include <gdk/gdkkeysyms.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <time.h>
#include <pthread.h>
#include "hx.h"
#include "gtk_hlist.h"
#include "news.h"
#include "network.h"
#include "toolbar.h"
#include "tasks.h"
#include "users.h"
#include "chat.h"
#include "connect.h"
#include "gtkhx.h"
#include "files.h"
#include "xtext.h"

GtkAccelGroup *accel_group = NULL;


void init_keyaccel(GtkWidget *widget)
{
	if(!accel_group) {
		accel_group = gtk_accel_group_new();
		gtk_widget_add_accelerator(connect_btn, "clicked" , accel_group, 'k', GDK_CONTROL_MASK, 0);
		gtk_widget_add_accelerator(quit_btn, "clicked" ,accel_group, 'q', GDK_CONTROL_MASK, 0);
	}
	
	gtk_accel_group_attach (accel_group, GTK_OBJECT(widget));
}

void set_disconnect_btn(session *sess, int stat)
{
	gtk_widget_set_sensitive(disconnect_btn, stat);
}

void setbtns(session *sess, int stat)
{
	if(gtkhx_prefs.geo.users.open) {
		gtk_widget_set_sensitive(msgbtn, stat);
		gtk_widget_set_sensitive(kickbtn, stat);
		gtk_widget_set_sensitive(infobtn, stat);
		gtk_widget_set_sensitive(banbtn, stat);
		gtk_widget_set_sensitive(chatbtn, stat);
		gtk_widget_set_sensitive(ignobtn, stat);
	}
	if(gtkhx_prefs.geo.news.open) {
		gtk_widget_set_sensitive(sess->postButton, stat);

		gtk_widget_set_sensitive(sess->reloadButton, stat);

	}

	gtk_widget_set_sensitive(files_btn, stat);
	gtk_widget_set_sensitive(usermod_btn, stat);
	gtk_widget_set_sensitive(usernew_btn, stat);
	
	if(!stat) {
		gtk_widget_set_sensitive(news15_btn, stat);
		gtk_widget_set_sensitive(post_btn, stat);
	}
	else if(sess->htlc.version >= 150) {
		gtk_widget_set_sensitive(news15_btn, TRUE);
		gtk_widget_set_sensitive(post_btn, FALSE);
	}
	else if(sess->htlc.version < 150) {
		gtk_widget_set_sensitive(post_btn, TRUE);
		gtk_widget_set_sensitive(news15_btn, FALSE);
	}
}

void set_status_bar(int status)
{
	if(!status_bar) {
		return;
	}

	/* XXX: switch statement here */
	if(status == -1) {
		char *str;

		gtk_statusbar_remove(GTK_STATUSBAR(status_bar), context_status, status_msg);
		str = g_strdup_printf("%s %s", _("Connecting to"), server_addr);
		status_msg = gtk_statusbar_push(GTK_STATUSBAR(status_bar), context_status, str);
		g_free(str);
	}
	else if(!status) {
		gtk_statusbar_remove(GTK_STATUSBAR(status_bar), context_status, status_msg);
		status_msg = gtk_statusbar_push(GTK_STATUSBAR(status_bar), context_status, _("Not Connected"));
	}
	else if(status == 1) {
		char *str = g_strdup_printf("%s %s", _("Connected to"), server_addr);
		gtk_statusbar_remove(GTK_STATUSBAR(status_bar), context_status, status_msg);
		status_msg = gtk_statusbar_push(GTK_STATUSBAR(status_bar), context_status, str);
		g_free(str);
	}
	else if(status == 2) {
		char *str = g_strdup_printf("%s %s", _("Logged in to"), server_addr);
		gtk_statusbar_remove(GTK_STATUSBAR(status_bar), context_status, status_msg);
		status_msg = gtk_statusbar_push(GTK_STATUSBAR(status_bar), context_status, str);
		g_free(str);
	}
}

void changetitlesconnected(session *sess)
{
	char *newstitle;
	char *taskstitle;
	char *chattitle;
	char *userstitle;
	char *tooltitle;

	tooltitle = g_strdup_printf("%s (%s)", _("GtkHx"), server_addr);
	gtk_window_set_title(GTK_WINDOW(sess->toolbar_window), tooltitle);
	g_free(tooltitle);

	if(gtkhx_prefs.geo.news.open) {
			newstitle = g_strdup_printf("%s (%s)", _("News"), server_addr);
			gtk_window_set_title(GTK_WINDOW(sess->news_window), newstitle);
			g_free(newstitle);
		}
	if(gtkhx_prefs.geo.chat.open) {
			chattitle = g_strdup_printf("%s (%s)", _("Chat"), server_addr);
			gtk_window_set_title(GTK_WINDOW(sess->chat_window), chattitle);
			g_free(chattitle);
		}
	if(gtkhx_prefs.geo.users.open) {
			userstitle = g_strdup_printf("%s (%s)", _("Users"), server_addr);
			gtk_window_set_title(GTK_WINDOW(sess->users_window), userstitle);
			g_free(userstitle);
		}
	if(gtkhx_prefs.geo.tasks.open) {
			taskstitle = g_strdup_printf("%s (%s)", _("Tasks"), server_addr);
			gtk_window_set_title(GTK_WINDOW(sess->tasks_window), taskstitle);
			g_free(taskstitle);
		}
}

void changetitlespecific(GtkWidget *widget, char *name)
{
	char *futuretitle;
	futuretitle = g_strdup_printf("%s (%s)", name, server_addr);
	gtk_window_set_title(GTK_WINDOW(widget), futuretitle);
	g_free(futuretitle);
}

void changetitlesdisconnected(session *sess)
{
	if(gtkhx_prefs.geo.news.open) {
		gtk_window_set_title(GTK_WINDOW(sess->news_window), _("News"));
	}
	if(gtkhx_prefs.geo.chat.open) {
		gtk_window_set_title(GTK_WINDOW(sess->chat_window), _("Chat"));
	}
	if(gtkhx_prefs.geo.users.open) {
		gtk_window_set_title(GTK_WINDOW(sess->users_window), _("Users"));
	}
	if(gtkhx_prefs.geo.tasks.open) {
		gtk_window_set_title(GTK_WINDOW(sess->tasks_window), _("Tasks"));
	}

	gtk_window_set_title(GTK_WINDOW(sess->toolbar_window), _("GtkHx"));
}

void close_connected_windows(session *sess)
{
	struct gtkhx_chat *gchat, *prev = NULL;

	if(sess->agreementwin) {
		gtk_widget_destroy(sess->agreementwin);
		sess->agreementwin = NULL;
	}
	destroy_gfl_list();


	for(gchat = sess->gchat_list; gchat; gchat = prev) {
		prev = gchat->prev;
		if(gchat->cid) {
			gtk_widget_destroy(gchat->window);
			gchat_delete(sess, gchat);
		}
	}
}

char *add_break(char *msg, int pos)
{
	size_t len = strlen(msg);	

	msg = g_realloc(msg, len+1);
	memmove(&msg[pos+1], &msg[pos], len-pos);
	msg[pos] = '\n';

	return msg;
}

void error_dialog (char *title, char *msg)
{
    GtkWidget *label;
    GtkWidget *dialog;
    GtkWidget *okbutton;
	char *message = g_strdup(msg);
	size_t len = strlen(message);
	int i;

	/* insert a line break at every 50 chars, otherwise the message will just
	   run off */
	if(len > 50) {
		for(i = 0; i < len; i++) {
			if((!(i%50)) && i) {
				message = add_break(message, i);
				len++;
			}
		}
	}

    dialog = gtk_dialog_new();

    gtk_window_set_title(GTK_WINDOW(dialog), title);
    gtk_container_border_width (GTK_CONTAINER(dialog), 5);
    label = gtk_label_new (message);
    gtk_widget_set_usize(dialog, 250, 200);

    gtk_box_pack_start (GTK_BOX (GTK_DIALOG (dialog)->vbox), label, TRUE, 
						TRUE , 0);

    okbutton = gtk_button_new_with_label ("Ok");

    gtk_signal_connect_object (GTK_OBJECT (okbutton), "clicked", 
							   (GtkSignalFunc)gtk_widget_destroy, 
							   GTK_OBJECT(dialog));

    GTK_WIDGET_SET_FLAGS (okbutton, GTK_CAN_DEFAULT);

    gtk_box_pack_start (GTK_BOX (GTK_DIALOG(dialog)->action_area), okbutton, 
						TRUE, TRUE, 0);


    gtk_widget_grab_default (okbutton);

    gtk_widget_show_all (dialog);
	g_free(message);
}
