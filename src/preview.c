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
	GtkWidget *vscroll;
	GtkWidget *hbox;

	window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(window), p->name);
	text = gtk_text_new(0,0);
	vscroll = gtk_vscrollbar_new(GTK_TEXT(text)->vadj);
	hbox = gtk_hbox_new(0,0);

	gtk_widget_set_usize(window, 400, 300);
	gtk_box_pack_start(GTK_BOX(hbox), text, 1, 1, 0);
	gtk_box_pack_start(GTK_BOX(hbox), vscroll, 0, 0, 0);
	gtk_container_add(GTK_CONTAINER(window), hbox);

	gtk_widget_show_all(window);

	tp->window = window;
	tp->text = text;
	tp->p = p;

	return tp;
}

static void hx_preview_text_output (struct hx_preview *p, char *buf, int len)
{
	struct hx_text_preview *tp = p->data;

	CR2LF(buf, len);
	gtk_text_insert(GTK_TEXT(tp->text), NULL, NULL, NULL, buf, len);
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
