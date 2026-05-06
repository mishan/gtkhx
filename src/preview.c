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
#include <adwaita.h>
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
#include "gtkutil.h"

static struct hx_preview *hx_preview_list = NULL;

/* Phase 5: GTK 4 GtkTextBuffer requires valid UTF-8; injecting raw
 * bytes from a binary file (or any file with invalid sequences in
 * the middle) hits an assertion that takes the app down. Sanitize
 * with g_utf8_make_valid so binary content displays as garbled text
 * rather than crashing the app. */
static gboolean
preview_window_close_request (GtkWindow *window, gpointer user_data)
{
	struct hx_text_preview *tp = user_data;
	(void) window;
	if (tp)
		tp->closed = TRUE;
	return FALSE;  /* let the default destroy proceed */
}

static struct hx_text_preview *hx_text_preview_new(struct hx_preview *p)
{
	struct hx_text_preview *tp = g_malloc0(sizeof(struct hx_text_preview));
	GtkWidget *window;
	GtkWidget *text;
	GtkWidget *scroll;

	window = gtk_window_new();
	gtk_window_set_title(GTK_WINDOW(window), p->name);
	gtk_window_set_titlebar(GTK_WINDOW(window), adw_header_bar_new());
	text = gtk_text_view_new();
	gtk_text_view_set_editable(GTK_TEXT_VIEW(text), FALSE);
	gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(text), FALSE);
	gtk_text_view_set_monospace(GTK_TEXT_VIEW(text), TRUE);
	scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
	                               GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtkhx_widget_set_child(scroll, text);

	gtk_window_set_default_size(GTK_WINDOW(window), 480, 360);
	gtkhx_widget_set_child(window, scroll);

	tp->window = window;
	tp->text = text;
	tp->p = p;
	tp->closed = FALSE;

	g_signal_connect(window, "close-request",
	                 G_CALLBACK(preview_window_close_request), tp);

	gtk_window_present(GTK_WINDOW(window));

	return tp;
}

static void hx_preview_text_output (struct hx_preview *p, char *buf, int len)
{
	struct hx_text_preview *tp = p->data;
	GtkTextBuffer *tbuf;
	GtkTextIter end;
	char *valid;

	if (!tp || tp->closed)
		return;

	CR2LF(buf, len);
	/* g_utf8_make_valid takes a length-bounded buffer and returns a
	 * fresh g_strdup'd copy with invalid sequences replaced by U+FFFD.
	 * Binary content stays renderable; valid UTF-8 passes through
	 * unchanged. */
	valid = g_utf8_make_valid(buf, len);
	tbuf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tp->text));
	gtk_text_buffer_get_end_iter(tbuf, &end);
	gtk_text_buffer_insert(tbuf, &end, valid, -1);
	g_free(valid);
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
