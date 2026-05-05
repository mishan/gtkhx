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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <gtk/gtk.h>
#include <time.h>
#include <netinet/in.h>
#include "hx.h"
#include "hfs.h"
#include "network.h"
#include "rcv.h"
#include "chat.h"
#include "tasks.h"
#include "sound.h"
#include "files.h"
#include "preview.h"

static struct hx_preview *hx_preview_list = NULL;

static struct hx_text_preview *hx_text_preview_new(struct hx_preview *p)
{
	struct hx_text_preview *tp = malloc(sizeof(struct hx_text_preview));
	GtkWidget *window;
	GtkWidget *text;
	GtkWidget *scroll;

	window = gtk_window_new();
	gtk_window_set_title(GTK_WINDOW(window), p->name);
	text = gtk_text_view_new();
	gtk_text_view_set_editable(GTK_TEXT_VIEW(text), FALSE);
	gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(text), FALSE);
	scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
	                               GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtkhx_widget_set_child(scroll, text);

	gtk_widget_set_size_request(window, 400, 300);
	gtkhx_widget_set_child(window, scroll);

	gtk_widget_show(window);

	tp->window = window;
	tp->text = text;
	tp->p = p;

	return tp;
}

static void hx_preview_text_output (struct hx_preview *p, char *buf, int len)
{
	struct hx_text_preview *tp = p->data;

	{
		GtkTextBuffer *tbuf;
		GtkTextIter end;

		CR2LF(buf, len);
		tbuf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tp->text));
		gtk_text_buffer_get_end_iter(tbuf, &end);
		gtk_text_buffer_insert(tbuf, &end, buf, len);
	}
}

struct hx_preview *hx_preview_new(char *creator, char *type, char *name)
{
	struct hx_preview *p = malloc(sizeof(struct hx_preview));

	strncpy(p->creator, creator, 4);
	strncpy(p->type, type, 4);

	/* XXX: select appropriate output plugin */

	/* text */
	p->name = g_strdup(name);
	p->data = hx_text_preview_new(p);
	p->output = hx_preview_text_output;


	if(hx_preview_list) {
		hx_preview_list->next = p;
	}

	p->next = NULL;
	p->prev = hx_preview_list;
	hx_preview_list = p;

	return p;
}
