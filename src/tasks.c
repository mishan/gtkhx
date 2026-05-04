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
#include <netinet/in.h>
#include <sys/time.h>
#include <time.h>
#include "hx.h"
#include "gtk_hlist.h"
#include "network.h"
#include "gtkutil.h"
#include "gtkhx.h"
#include "xfers.h"
#include "sound.h"
#include "toolbar.h"
#include "tasks.h"

#include "pixmaps/kick.xpm"
#include "pixmaps/start.xpm"
#include "pixmaps/up.xpm"
#include "pixmaps/down.xpm"

static GtkWidget *tasks_vbox;

struct gtask {
	struct gtask *next, *prev;
	guint32 trans;
	struct htxf_conn *htxf;
	GtkWidget *label;
	GtkWidget *pbar;
	GtkWidget *listitem;
	GtkWidget *queue;
};

void create_tasks(session *sess)
{
	GtkWidget *gtklist, *gtask_scroll;

	/* Phase 3.2: ported from GtkList (removed in GTK 3) to GtkListBox.
	 * Each transfer/task is a GtkListBoxRow holding the label+pbar
	 * vbox; gtsk->listitem points at the row, with the gtsk pointer
	 * stashed via g_object_set_data on the row itself. */
	gtklist = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(gtklist),
									GTK_SELECTION_MULTIPLE);
	g_object_ref_sink(gtklist);

	gtask_scroll = gtk_scrolled_window_new(0, 0);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(gtask_scroll),
				       GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
	gtk_container_add(GTK_CONTAINER(gtask_scroll), gtklist);
	g_object_ref_sink(gtask_scroll);

	sess->gtklist = gtklist;
	sess->gtask_scroll = gtask_scroll;
}

static struct gtask *
gtask_with_trans (session *sess, guint32 trans)
{
	struct gtask *gtsk;

	for (gtsk = sess->gtask_list; gtsk; gtsk = gtsk->prev) {
		if (gtsk->trans == trans)
			return gtsk;
	}

	return 0;
}


static struct gtask *
gtask_with_htxf ( session *sess, struct htxf_conn *htxf)
{
	struct gtask *gtsk;

	for (gtsk = sess->gtask_list; gtsk; gtsk = gtsk->prev) {
		if (gtsk->htxf == htxf)
			return gtsk;
	}

	return 0;
}

void output_xfer_queue(session *sess, struct htxf_conn *htxf)
{
	struct gtask *gtsk = gtask_with_htxf(sess, htxf);
	char qid[16];

	if(!gtsk) {
		return;
	}

	g_snprintf(qid, sizeof(qid), "%d", htxf->queue);
	gtk_label_set_text(GTK_LABEL(gtsk->queue), qid);
}

static struct gtask *gtask_new (session *sess, guint32 trans, 
								struct htxf_conn *htxf)
{	GtkWidget *pbar;
	GtkWidget *vbox;
	GtkWidget *hbox;
	GtkWidget *label;
	GtkWidget *listitem;
	GtkWidget *queue = 0;
	char qid[16];
	struct gtask *gtsk;

	gtsk = g_malloc(sizeof(struct gtask));
	gtsk->next = 0;
	gtsk->prev = sess->gtask_list;
	if (sess->gtask_list)
		sess->gtask_list->next = gtsk;

	pbar = gtk_progress_bar_new();
	label = gtk_label_new("");

	if(htxf) {
		if(htxf->queue > 0) {
			g_snprintf(qid, sizeof(qid), "%d", htxf->queue);
			queue = gtk_label_new(qid);
		}
		else {
			queue = gtk_label_new("");
		}
	}

	gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_LEFT);
	vbox = gtk_vbox_new(0, 0);
	hbox = gtk_hbox_new(0, 0);
	gtk_widget_set_size_request(vbox, 240, 40);

	if(htxf) {
		gtk_box_pack_start(GTK_BOX(hbox), queue, 0, 0, 0);
	}
	gtk_box_pack_start(GTK_BOX(hbox), label, 0, 0, 4);
	gtk_box_pack_start(GTK_BOX(vbox), hbox, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(vbox), pbar, 1, 1, 0);

	listitem = gtk_list_box_row_new();
	g_object_set_data(G_OBJECT(listitem), "gtsk", gtsk);
	gtk_container_add(GTK_CONTAINER(listitem), vbox);

	if (sess->gtklist) {
		gtk_list_box_insert(GTK_LIST_BOX(sess->gtklist), listitem, -1);
		gtk_widget_show_all(listitem);
	}

	gtsk->label = label;
	gtsk->pbar = pbar;
	gtsk->listitem = listitem;
	gtsk->trans = trans;
	gtsk->htxf = htxf;
	gtsk->queue = queue;
	sess->gtask_list = gtsk;

	return gtsk;
}

static void gtask_delete (session *sess, struct gtask *gtsk)
{
	if(sess->gtklist) {
		gtk_container_remove(GTK_CONTAINER(sess->gtklist), gtsk->listitem);
	}
	if (gtsk->next)
		gtsk->next->prev = gtsk->prev;
	if (gtsk->prev)
		gtsk->prev->next = gtsk->next;
	if (gtsk == sess->gtask_list)
		sess->gtask_list = gtsk->prev;
	g_free(gtsk);
}

void gtask_delete_htxf (session *sess, struct htxf_conn *htxf)
{
	struct gtask *gtsk = gtask_with_htxf(sess, htxf);
	if(!gtsk) {
		return;
	}
	gtask_delete(sess, gtsk);
}

void gtask_delete_tsk(session *sess, guint32 trans)
{
	struct gtask *gtsk = gtask_with_trans(sess, trans);
	if(!gtsk) {
		return;
	}
	gtask_delete(sess, gtsk);
}

void track_prog_update (session *sess, char *str, int num, int total)
{
	GtkWidget *pbar;
	GtkWidget *label;
	char taskstr[256];
	struct gtask *gtsk;
	guint32 pos = num;


	gtsk = gtask_with_trans(sess, -127);
	if(!gtsk) {
		gtsk = gtask_new(sess, -127, 0);
	}

	label = gtsk->label;
	pbar = gtsk->pbar;
	g_snprintf(taskstr, sizeof(taskstr), _("Task (Listing Tracker: %s) %u/%u"),
			 str, pos, total);
	gtk_label_set_text(GTK_LABEL(label), taskstr);

	if (pos)
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(pbar), 
								(gfloat)pos / (gfloat)(pos + total));

	if (num >= total)
		gtask_delete(sess, gtsk);
}

void trackconn_prog_update (session *sess, char *str, int num, int total)
{
	GtkWidget *pbar;
	GtkWidget *label;
	char taskstr[256];
	struct gtask *gtsk;
	guint32 pos = num;


	gtsk = gtask_with_trans(sess, -129);
	if(!gtsk) {
		gtsk = gtask_new(sess, -129, 0);
	}

	label = gtsk->label;
	pbar = gtsk->pbar;
	g_snprintf(taskstr, sizeof(taskstr), _("Task (Connecting to Tracker: %s) %u/%u"),
			 str, pos, total);
	gtk_label_set_text(GTK_LABEL(label), taskstr);

	if (pos)
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(pbar), 
								(gfloat)pos / (gfloat)(pos + total));

	if (num >= total)
		gtask_delete(sess, gtsk);
}



void conn_task_update(session *sess, int stat)
{
	GtkWidget *pbar;
	GtkWidget *label;
	char taskstr[256];
	struct gtask *gtsk;
	guint32 pos = stat/2, len = 2;


	gtsk = gtask_with_trans(sess, -128);
	if(!gtsk) {
		gtsk = gtask_new(sess, -128, 0);
	}


	label = gtsk->label;
	pbar = gtsk->pbar;
	snprintf(taskstr, sizeof(taskstr), _("Task (Connecting) %u/%u"),
			 pos, pos+len);
	gtk_label_set_text(GTK_LABEL(label), taskstr);

	if (pos)
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(pbar), 
								(gfloat)pos / (gfloat)(pos + len));

	if (stat == len)
		gtask_delete(sess, gtsk);
}

void task_update (session *sess, struct task *tsk)
{
	GtkWidget *pbar;
	GtkWidget *label;
	char taskstr[256];
	struct gtask *gtsk;
	guint32 pos = tsk->pos, len = tsk->len;

	gtsk = gtask_with_trans(sess, tsk->trans);
	if (!gtsk)
		gtsk = gtask_new(sess, tsk->trans, 0);


	label = gtsk->label;
	pbar = gtsk->pbar;
	snprintf(taskstr, sizeof(taskstr), _("Task 0x%x (%s) %u/%u"),
			 tsk->trans, tsk->str ? tsk->str : "", pos, pos+len);
	gtk_label_set_text(GTK_LABEL(label), taskstr);
	if (pos)
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(pbar), 
								(gfloat)pos / (gfloat)(pos + len));

	if (len == 0)
		gtask_delete(sess, gtsk);
}

static void
tasks_destroy (GtkWidget *widget, gpointer data)
{
	session *sess = data;

	gtk_container_remove(GTK_CONTAINER(tasks_vbox), sess->gtask_scroll);
	gtkhx_prefs.geo.tasks.open = 0;
	gtkhx_prefs.geo.tasks.init = 0;
}

extern void tracker_kill_threads(void);
static void
task_stop (GtkWidget *widget, gpointer data)
{
	struct gtask *gtsk;
	GList *sel, *lp, *next;
	GtkWidget *listitem;
	session *sess = data;

	if (!gtkhx_prefs.geo.tasks.open)
		return;

	/* Phase 3.2: gtk_list_box_get_selected_rows returns a GList* of
	 * GtkListBoxRow* (the rows themselves, not their children).
	 * Caller owns the GList and must g_list_free() it; the rows
	 * themselves are owned by the list box. */
	sel = gtk_list_box_get_selected_rows(GTK_LIST_BOX(sess->gtklist));
	for(lp = sel; lp; lp = next) {
		next = lp->next;
		listitem = (GtkWidget *)lp->data;
		gtsk = (struct gtask *)g_object_get_data(G_OBJECT(listitem), "gtsk");


		if (gtsk->htxf) {
			xfer_delete(gtsk->htxf);
			gtask_delete(sess, gtsk);
		}
		else if(gtsk->trans == -127) {
			tracker_kill_threads();
			gtask_delete(sess, gtsk);
		}
		else if(gtsk->trans == -128) {
			disconnect_clicked();
			/* disconnect_clicked updates connection task, so it should already
			   handle deleting the task */
/*			gtask_delete(sess, gtsk); */
		}
		else if(gtsk->trans == -129) {
			tracker_kill_threads();
			gtask_delete(sess, gtsk);
		}
		else {
			/* task_delete should handle deleting the gtask */
			task_delete(sess, task_with_trans(sess, gtsk->trans));
/*			gtask_delete(sess, gtsk); */
		}
	}
	g_list_free(sel);
}

/* Phase 3.2: Move a GtkListBoxRow to a new index by ref'ing it,
 * removing it from the container, and re-inserting at the new index.
 * The list box re-acquires its ref on insert. */
static void
gtklist_row_move(GtkListBox *box, GtkWidget *row, int new_index)
{
	g_object_ref(row);
	gtk_container_remove(GTK_CONTAINER(box), row);
	gtk_list_box_insert(box, row, new_index);
	g_object_unref(row);
	gtk_list_box_select_row(box, GTK_LIST_BOX_ROW(row));
}

static void
task_up(GtkWidget *widget, gpointer data)
{
	struct gtask *gtsk;
	GList *sel;
	GtkWidget *listitem;
	int num, gtkpos;
	session *sess = data;

	if(!gtkhx_prefs.queuedl) {
		return;
	}

	sel = gtk_list_box_get_selected_rows(GTK_LIST_BOX(sess->gtklist));
	if(!sel) {
		return;
	}
	listitem = sel->data;
	g_list_free(sel);
	gtsk = g_object_get_data(G_OBJECT(listitem), "gtsk");


	if(!gtsk->htxf) {
		return;
	}

	num = xfer_num(gtsk->htxf);

	if(num <= 1) {
		return;
	}


	xfer_up(num);

	gtkpos = gtk_list_box_row_get_index(GTK_LIST_BOX_ROW(listitem));
	if(gtkpos <= 0) {
		return;
	}
	gtklist_row_move(GTK_LIST_BOX(sess->gtklist), listitem, gtkpos - 1);
}

static void task_dn(GtkWidget *widget, gpointer data)
{
	struct gtask *gtsk;
	GList *sel;
	GtkWidget *listitem;
	int num, gtkpos;
	session *sess = data;

	if(!gtkhx_prefs.queuedl) {
		return;
	}
	sel = gtk_list_box_get_selected_rows(GTK_LIST_BOX(sess->gtklist));
	if(!sel) {
		return;
	}
	listitem = sel->data;
	g_list_free(sel);
	gtsk = g_object_get_data(G_OBJECT(listitem), "gtsk");


	if(!gtsk->htxf) {
		return;
	}

	num = xfer_num(gtsk->htxf);

	if(num <= 0) {
		return;
	}


	if(xfer_down(num)) {
		return;
	}

	gtkpos = gtk_list_box_row_get_index(GTK_LIST_BOX_ROW(listitem));
	gtklist_row_move(GTK_LIST_BOX(sess->gtklist), listitem, gtkpos + 1);
}

static void
task_go (GtkWidget *widget, gpointer data)
{
	struct gtask *gtsk;
	GList *sel;
	GtkWidget *listitem;
	session *sess = data;

	if (!gtkhx_prefs.geo.tasks.open) {
		return;
	}

	sel = gtk_list_box_get_selected_rows(GTK_LIST_BOX(sess->gtklist));
	if(!sel) {
		return;
	}
	listitem = sel->data;
	g_list_free(sel);
	gtsk = (struct gtask *)g_object_get_data(G_OBJECT(listitem), "gtsk");
	if (gtsk->htxf) {
		xfer_go(gtsk->htxf);
	}
}

static void tasks_move(GtkWidget *w, GdkEventConfigure *e, gpointer data)
{
	int x, y, width, height;
	session *sess = data;

	gdk_window_get_root_origin(gtk_widget_get_window(sess->tasks_window), &x, &y);
	width = gdk_window_get_width(gtk_widget_get_window(sess->tasks_window));
	height = gdk_window_get_height(gtk_widget_get_window(sess->tasks_window));
	if(e->send_event) { /* Is a position event */
		gtkhx_prefs.geo.tasks.xpos = x;
		gtkhx_prefs.geo.tasks.ypos = y;
	} else { /* Is a size event */
		gtkhx_prefs.geo.tasks.xsize = width;
		gtkhx_prefs.geo.tasks.ysize = height;
	}
}

void task_tasks_update (session *sess)
{
	struct task *tsk;

	for (tsk = sess->task_list->next; tsk; tsk = tsk->next) {
		hx_output.task_update(sess, tsk);
	}
}


void create_tasks_window (GtkWidget *widget, gpointer data)
{
	GtkWidget *vbox;
	GtkWidget *hbuttonbox;
	GtkWidget *topframe;
	GtkWidget *stopbtn;
	GtkWidget *gobtn;
	GtkWidget *upbtn;
	GtkWidget *dnbtn;
	GdkBitmap *mask;
	GdkPixmap *icon;
	GtkWidget *pix;
	GtkStyle *style;
	GtkWidget *tasks_window;
	session *sess = data;

	if (gtkhx_prefs.geo.tasks.open) {
		gdk_window_raise(gtk_widget_get_window(sess->tasks_window));
		return;
	}

	tasks_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_wmclass(GTK_WINDOW(tasks_window), "tasks", "GtkHx");
	gtk_window_set_resizable(GTK_WINDOW(tasks_window), TRUE);
	gtk_widget_realize(tasks_window);
	style = gtk_widget_get_style(tasks_window);
	gtk_window_set_title(GTK_WINDOW(tasks_window), _("Tasks"));


	topframe = gtk_frame_new(0);
	gtk_widget_set_size_request(topframe, -1, 30);
	gtk_frame_set_shadow_type(GTK_FRAME(topframe), GTK_SHADOW_OUT);

	hbuttonbox = gtk_hbox_new(0,0);

	stopbtn = gtk_button_new();
	icon = (GdkPixmap *)gdk_pixbuf_new_from_xpm_data((const char **)kick_xpm);
	pix = gtk_image_new_from_pixbuf((GdkPixbuf *)icon);
	gtk_container_add(GTK_CONTAINER(stopbtn), pix);
	gtk_widget_set_tooltip_text(stopbtn, _("Stop Task"));
	g_signal_connect(stopbtn, "clicked",
			   G_CALLBACK(task_stop), sess);
	icon = 0, mask = 0, pix = 0;

	gobtn = gtk_button_new();
	icon = (GdkPixmap *)gdk_pixbuf_new_from_xpm_data((const char **)start_xpm);
	pix = gtk_image_new_from_pixbuf((GdkPixbuf *)icon);
	gtk_container_add(GTK_CONTAINER(gobtn), pix);
	gtk_widget_set_tooltip_text(gobtn, _("Start Task"));
	g_signal_connect(gobtn, "clicked",
					   G_CALLBACK(task_go), sess);
	icon = 0, mask = 0, pix = 0;

	upbtn = gtk_button_new();
	icon = (GdkPixmap *)gdk_pixbuf_new_from_xpm_data((const char **)up_xpm);
	pix = gtk_image_new_from_pixbuf((GdkPixbuf *)icon);
	gtk_container_add(GTK_CONTAINER(upbtn), pix);
	gtk_widget_set_tooltip_text(upbtn, _("Move Xfer Up in Queue"));
	g_signal_connect(upbtn, "clicked", G_CALLBACK(task_up), 
					   sess);
	icon = 0, mask = 0, pix = 0;


	dnbtn = gtk_button_new();
	icon = (GdkPixmap *)gdk_pixbuf_new_from_xpm_data((const char **)down_xpm);
	pix = gtk_image_new_from_pixbuf((GdkPixbuf *)icon);
	gtk_container_add(GTK_CONTAINER(dnbtn), pix);
	gtk_widget_set_tooltip_text(dnbtn, _("Move Xfer Down in Queue"));
	g_signal_connect(dnbtn, "clicked", G_CALLBACK(task_dn), 
					   sess);

	vbox = gtk_vbox_new(0, 0);

	gtk_box_pack_start(GTK_BOX(hbuttonbox), stopbtn, 0, 0, 2);
	gtk_box_pack_start(GTK_BOX(hbuttonbox), gobtn, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(hbuttonbox), upbtn, 0, 0, 2);
	gtk_box_pack_start(GTK_BOX(hbuttonbox), dnbtn, 0, 0, 0);

	gtk_container_add(GTK_CONTAINER(topframe), hbuttonbox);
	gtk_box_pack_start(GTK_BOX(vbox), topframe, 0, 0, 0);
	gtk_container_add(GTK_CONTAINER(tasks_window), vbox);

	tasks_vbox = vbox;
	gtk_box_pack_start(GTK_BOX(vbox), sess->gtask_scroll, 1, 1, 0);

	g_signal_connect(tasks_window, "destroy",
					   G_CALLBACK(tasks_destroy), sess);


	init_keyaccel(tasks_window);
	g_signal_connect(tasks_window, "configure_event", 
					   G_CALLBACK(tasks_move), sess);
	gtk_widget_set_size_request(tasks_window, gtkhx_prefs.geo.tasks.xsize, 
						 gtkhx_prefs.geo.tasks.ysize);
	gtk_window_move(GTK_WINDOW(tasks_window), gtkhx_prefs.geo.tasks.xpos,
					gtkhx_prefs.geo.tasks.ypos);
	gtk_widget_show_all(tasks_window);


	if(connected == 1) {
		changetitlespecific(tasks_window, _("Tasks"));
	}
	sess->tasks_window = tasks_window;

	gtkhx_prefs.geo.tasks.open = 1;
	gtkhx_prefs.geo.tasks.init = 1;

	task_tasks_update(sess);
	xfer_tasks_update(&sess->htlc);
}

#define LONGEST_HUMAN_READABLE	32
extern char *
human_size (char *sizstr, guint32 size);

void file_update (session *sess, struct htxf_conn *htxf)
{
	GtkWidget *pbar;
	GtkWidget *label;
	struct gtask *gtsk;
	char str[4096];
	char humanbuf[LONGEST_HUMAN_READABLE+1], *posstr, *sizestr, *bpsstr;
	guint32 pos, size;
	struct timeval now;
	time_t sdiff, usdiff, Bps, eta;
	int hrs, mins, secs;

	gtsk = gtask_with_htxf(sess, htxf);
	if (!gtsk)
		gtsk = gtask_new(sess, 0, htxf);
	label = gtsk->label;
	pbar = gtsk->pbar;

	pos = htxf->total_pos;
	size = htxf->total_size;

	gettimeofday(&now, 0);
	sdiff = now.tv_sec - htxf->start.tv_sec;
	usdiff = now.tv_usec - htxf->start.tv_usec;
	if (!sdiff)
		sdiff = 1;
	Bps = pos / sdiff;
	if (!Bps)

		Bps = 1;
	eta = (size - pos) / Bps
	    + ((size - pos) % Bps) / Bps;

	hrs = eta/3600;
	eta %= 3600;
	mins = eta/60;
	eta %= 60;
	secs = eta;

	posstr = g_strdup(human_size(humanbuf, pos));
	memset(&humanbuf, 0, sizeof(humanbuf));
	sizestr = g_strdup(human_size(humanbuf, size));
	memset(&humanbuf, 0, sizeof(humanbuf));
	bpsstr = g_strdup(human_size(humanbuf, Bps));

	snprintf(str, sizeof(str), _("%s  %s/%s  %s/s  ETA: %d:%02d:%02d  %s"),
		 htxf->type == XFER_GET ? "get" : "put",
		 posstr, sizestr, bpsstr, hrs, mins, secs, htxf->path);
	gtk_label_set_text(GTK_LABEL(label), str);

	if(((gfloat)pos/size) <= 1) {
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(pbar), (gfloat)pos/size);
	}

	g_free(posstr);
	g_free(sizestr);
	g_free(bpsstr);

	if (pos >= size) {
		gtask_delete(sess, gtsk);
	}
}


struct task * task_new (struct htlc_conn *htlc, void (*rcv)(), void *ptr, 
						void *data, const char *str)
{
	struct task *tsk;
	session *sess = &the_session;

	tsk = g_malloc(sizeof(struct task));
	tsk->trans = htlc->trans;
	tsk->data = data;
	if (str)
		tsk->str = g_strdup(str);
	else
		tsk->str = 0;
	tsk->ptr = ptr;
	tsk->rcv = rcv;

	tsk->next = 0;
	tsk->prev = sess->task_tail;
	sess->task_tail->next = tsk;
	sess->task_tail = tsk;

	tsk->pos = 0;
	tsk->len = 1;
	hx_output.task_update(sess, tsk);

	return tsk;
}

void task_delete (session *sess, struct task *tsk)
{
	gtask_delete_tsk(sess, tsk->trans);

	if (tsk->next)
		tsk->next->prev = tsk->prev;
	if (tsk->prev)
		tsk->prev->next = tsk->next;
	if (sess->task_tail == tsk)
		sess->task_tail = tsk->prev;
	if (tsk->str)
		g_free(tsk->str);
	g_free(tsk);
}

struct task *task_with_trans (session *sess, guint32 trans)
{
	struct task *tsk;

	for (tsk = sess->task_list->next; tsk; tsk = tsk->next)
		if (tsk->trans == trans)
			return tsk;

	return 0;
}

void task_error (struct htlc_conn *htlc)
{
	char errormsg[8192+1];
	
	dh_start(htlc) {
		if (_type == HTLS_DATA_TASKERROR) {
			strncpy(errormsg, dh->data, _len);
			CR2LF(errormsg, _len);
			strip_ansi(errormsg, _len);
			errormsg[_len] = 0;
			error_dialog("Error", errormsg);
			play_sound(ERROR);
		}
	} dh_end();
}
