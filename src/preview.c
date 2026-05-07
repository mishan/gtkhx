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

/* Phase 5 v2: previously we did the gtk_text_buffer_insert directly from
 * the download worker thread under gtk_threads_enter. That serializes the
 * insert with the rest of the GUI but holds the GTK lock for the duration
 * of every insert — and inserts get linearly slower as the buffer grows,
 * because the GtkTextView's layout cache rebuilds. For a multi-MB preview
 * the worker reacquires the lock at high frequency, starving the main
 * thread until poll() can no longer service input. Symptom: app appears
 * frozen mid-preview.
 *
 * The fix is to keep the worker out of GTK entirely for previews. The
 * worker copies each chunk into a heap buffer and pushes it onto the
 * default main context via g_idle_add. The main thread services the idle
 * queue between input events at G_PRIORITY_DEFAULT_IDLE (lower priority
 * than user input), so the GUI stays responsive while the buffer fills.
 *
 * Lifetime: the parent `struct hx_preview` and `struct hx_text_preview`
 * stay alive for the duration of the program (the hx_preview_list leak
 * predates this fix and is tracked separately). The close-request
 * handler sets tp->closed = TRUE; both the worker-side hx_preview_text_output
 * and any already-queued idle callbacks check that flag and bail before
 * touching the (now-destroyed) text view.
 */

struct preview_chunk {
	struct hx_text_preview *tp;
	char  *data;
	gsize  len;
};

static gboolean
preview_window_close_request (GtkWindow *window, gpointer user_data)
{
	struct hx_text_preview *tp = user_data;
	(void) window;
	if (tp)
		tp->closed = TRUE;
	return FALSE;  /* let the default destroy proceed */
}

static gboolean
preview_chunk_idle (gpointer user_data)
{
	struct preview_chunk *chunk = user_data;
	GtkTextBuffer *tbuf;
	GtkTextIter end;
	char *valid;

	if (chunk->tp && !chunk->tp->closed) {
		/* g_utf8_make_valid returns a fresh g_strdup'd copy with
		 * invalid sequences replaced by U+FFFD. Binary content stays
		 * renderable; valid UTF-8 passes through unchanged. The
		 * caller's chunk is length-bounded so embedded NULs are
		 * tolerated up to here — gtk_text_buffer_insert with -1 will
		 * still strlen-truncate at the first NUL, but for any
		 * vaguely text-like preview that's fine and for binary
		 * files the user only loses the chunk past a NUL anyway. */
		valid = g_utf8_make_valid (chunk->data, chunk->len);
		tbuf  = gtk_text_view_get_buffer (GTK_TEXT_VIEW (chunk->tp->text));
		gtk_text_buffer_get_end_iter (tbuf, &end);
		gtk_text_buffer_insert (tbuf, &end, valid, -1);
		g_free (valid);
	}

	g_free (chunk->data);
	g_free (chunk);
	return G_SOURCE_REMOVE;
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
	/* Phase 5: word-char wrap so long lines flow within the preview
	 * window instead of overflowing the viewport. WORD_CHAR breaks
	 * preferentially on word boundaries but falls back to mid-word
	 * for content with no whitespace (long paths, hex dumps, etc.). */
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text), GTK_WRAP_WORD_CHAR);
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
	struct preview_chunk *chunk;

	if (!tp || tp->closed || len <= 0)
		return;

	/* CR2LF rewrites the caller's buffer in place; do it before we copy
	 * out so the idle callback gets the cleaned-up bytes. */
	CR2LF (buf, len);

	chunk = g_new (struct preview_chunk, 1);
	chunk->tp   = tp;
	chunk->data = g_memdup2 (buf, len);
	chunk->len  = len;

	/* Hand off to the main thread. Worker returns immediately rather
	 * than holding the GTK lock while the text view re-lays out. */
	g_idle_add (preview_chunk_idle, chunk);
}

struct hx_preview *hx_preview_new(char *creator, char *type, char *name)
{
	/* Phase 5: g_malloc0 (was malloc) so creator[5]/type[5] trailing
	 * bytes are deterministically zero — strncpy of 4 bytes leaves
	 * the 5th uninitialized which is fine but trips up valgrind /
	 * asan and bites us if anything ever reads them as a NUL-terminated
	 * string. */
	struct hx_preview *p = g_malloc0 (sizeof (struct hx_preview));

	if (creator)
		strncpy (p->creator, creator, 4);
	if (type)
		strncpy (p->type, type, 4);

	/* XXX: select appropriate output plugin */

	/* text */
	p->name = g_strdup (name ? name : "");
	p->data = hx_text_preview_new (p);
	p->output = hx_preview_text_output;


	if(hx_preview_list) {
		hx_preview_list->next = p;
	}

	p->next = NULL;
	p->prev = hx_preview_list;
	hx_preview_list = p;

	return p;
}
