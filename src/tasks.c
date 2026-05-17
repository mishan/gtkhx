/*
 * Copyright (C) 2000-2026 Misha Nasledov <misha@nasledov.com>
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
#include <netinet/in.h>
#include <sys/time.h>
#include <time.h>
#include "hx.h"
#include "gtkhx_session.h"
#include "gtk_hlist.h"
#include "network.h"
#include "gtkutil.h"
#include "gtkhx.h"
#include "xfers.h"
#include "sound.h"
#include "toolbar.h"
#include "tasks.h"
#include "tasks_table.h"

struct gtask {
    struct gtask *next, *prev;
    guint32 trans;
    struct htxf_conn *htxf;
    GtkWidget *label;
    GtkWidget *pbar;
    GtkWidget *listitem;
    GtkWidget *queue;
};

void
create_tasks (session *sess)
{
    GtkWidget *gtklist, *gtask_scroll;

    /* Phase 3.2: ported from GtkList (removed in GTK 3) to GtkListBox.
	 * Each transfer/task is a GtkListBoxRow holding the label+pbar
	 * vbox; gtsk->listitem points at the row, with the gtsk pointer
	 * stashed via g_object_set_data on the row itself. */
    gtklist = gtk_list_box_new ();
    gtk_list_box_set_selection_mode (GTK_LIST_BOX (gtklist),
                                     GTK_SELECTION_MULTIPLE);
    g_object_ref_sink (gtklist);

    gtask_scroll = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (gtask_scroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
    gtkhx_widget_set_child (gtask_scroll, gtklist);
    g_object_ref_sink (gtask_scroll);

    sess->gtklist = gtklist;
    sess->gtask_scroll = gtask_scroll;
}

static struct gtask *
gtask_with_trans (session *sess, guint32 trans)
{
    struct gtask *gtsk;

    for (gtsk = sess->gtask_list; gtsk; gtsk = gtsk->prev) {
        if (gtsk->trans == trans) {
            return gtsk;
        }
    }

    return 0;
}

static struct gtask *
gtask_with_htxf (session *sess, struct htxf_conn *htxf)
{
    struct gtask *gtsk;

    for (gtsk = sess->gtask_list; gtsk; gtsk = gtsk->prev) {
        if (gtsk->htxf == htxf) {
            return gtsk;
        }
    }

    return 0;
}

void
output_xfer_queue (session *sess, struct htxf_conn *htxf)
{
    struct gtask *gtsk = gtask_with_htxf (sess, htxf);
    char qid[16];

    if (!gtsk) {
        return;
    }

    g_snprintf (qid, sizeof (qid), "%d", htxf->queue);
    gtk_label_set_text (GTK_LABEL (gtsk->queue), qid);
}

static struct gtask *
gtask_new (session *sess, guint32 trans, struct htxf_conn *htxf)
{
    GtkWidget *pbar;
    GtkWidget *vbox;
    GtkWidget *hbox;
    GtkWidget *label;
    GtkWidget *listitem;
    GtkWidget *queue = 0;
    char qid[16];
    struct gtask *gtsk;

    gtsk = g_malloc (sizeof (struct gtask));
    gtsk->next = 0;
    gtsk->prev = sess->gtask_list;
    if (sess->gtask_list) {
        sess->gtask_list->next = gtsk;
    }

    pbar = gtk_progress_bar_new ();
    /* Phase 5 dark-theme follow-up: surface percent on the bar itself
	 * and give it real vertical room. The default GTK 4 progress-bar
	 * trough is ~6 px tall, which is hard to read against any theme;
	 * a 16 px floor keeps it visible without dwarfing the row, and
	 * show_text overlays percentage right where the eye looks. The
	 * descriptive label above still carries filename + transferred /
	 * total / speed / ETA. */
    gtk_progress_bar_set_show_text (GTK_PROGRESS_BAR (pbar), TRUE);
    gtk_widget_set_size_request (pbar, -1, 16);
    gtk_widget_set_valign (pbar, GTK_ALIGN_CENTER);
    label = gtk_label_new ("");

    if (htxf) {
        if (htxf->queue > 0) {
            g_snprintf (qid, sizeof (qid), "%d", htxf->queue);
            queue = gtk_label_new (qid);
        } else {
            queue = gtk_label_new ("");
        }
    }

    gtk_label_set_justify (GTK_LABEL (label), GTK_JUSTIFY_LEFT);
    gtk_label_set_xalign (GTK_LABEL (label), 0.0f);
    gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
    vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_size_request (vbox, 240, 50);

    if (htxf) {
        gtkhx_box_pack (hbox, queue, 0, 0, 0);
    }
    gtkhx_box_pack (hbox, label, 1, 1, 4);
    gtkhx_box_pack (vbox, hbox, 0, 0, 0);
    gtkhx_box_pack (vbox, pbar, 1, 1, 0);

    listitem = gtk_list_box_row_new ();
    g_object_set_data (G_OBJECT (listitem), "gtsk", gtsk);
    gtkhx_widget_set_child (listitem, vbox);

    if (sess->gtklist) {
        gtk_list_box_insert (GTK_LIST_BOX (sess->gtklist), listitem, -1);
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

static void
gtask_delete (session *sess, struct gtask *gtsk)
{
    if (sess->gtklist) {
        gtkhx_widget_remove_child (sess->gtklist, gtsk->listitem);
    }
    if (gtsk->next) {
        gtsk->next->prev = gtsk->prev;
    }
    if (gtsk->prev) {
        gtsk->prev->next = gtsk->next;
    }
    if (gtsk == sess->gtask_list) {
        sess->gtask_list = gtsk->prev;
    }
    g_free (gtsk);
}

void
gtask_delete_htxf (session *sess, struct htxf_conn *htxf)
{
    struct gtask *gtsk = gtask_with_htxf (sess, htxf);
    if (!gtsk) {
        return;
    }
    gtask_delete (sess, gtsk);
}

void
gtask_delete_tsk (session *sess, guint32 trans)
{
    struct gtask *gtsk = gtask_with_trans (sess, trans);
    if (!gtsk) {
        return;
    }
    gtask_delete (sess, gtsk);
}

void
track_prog_update (session *sess, char *str, int num, int total)
{
    GtkWidget *pbar;
    GtkWidget *label;
    char taskstr[256];
    struct gtask *gtsk;
    guint32 pos = num;

    gtsk = gtask_with_trans (sess, -127);
    if (!gtsk) {
        gtsk = gtask_new (sess, -127, 0);
    }

    label = gtsk->label;
    pbar = gtsk->pbar;
    g_snprintf (taskstr, sizeof (taskstr),
                _ ("Task (Listing Tracker: %s) %u/%u"), str, pos, total);
    gtk_label_set_text (GTK_LABEL (label), taskstr);

    if (pos) {
        gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (pbar),
                                       (gfloat)pos / (gfloat)(pos + total));
    }

    if (num >= total) {
        gtask_delete (sess, gtsk);
    }
}

void
trackconn_prog_update (session *sess, char *str, int num, int total)
{
    GtkWidget *pbar;
    GtkWidget *label;
    char taskstr[256];
    struct gtask *gtsk;
    guint32 pos = num;

    gtsk = gtask_with_trans (sess, -129);
    if (!gtsk) {
        gtsk = gtask_new (sess, -129, 0);
    }

    label = gtsk->label;
    pbar = gtsk->pbar;
    g_snprintf (taskstr, sizeof (taskstr),
                _ ("Task (Connecting to Tracker: %s) %u/%u"), str, pos, total);
    gtk_label_set_text (GTK_LABEL (label), taskstr);

    if (pos) {
        gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (pbar),
                                       (gfloat)pos / (gfloat)(pos + total));
    }

    if (num >= total) {
        gtask_delete (sess, gtsk);
    }
}

void
conn_task_update (session *sess, int stat)
{
    GtkWidget *pbar;
    GtkWidget *label;
    char taskstr[256];
    struct gtask *gtsk;
    guint32 pos = stat / 2, len = 2;

    gtsk = gtask_with_trans (sess, -128);
    if (!gtsk) {
        gtsk = gtask_new (sess, -128, 0);
    }

    label = gtsk->label;
    pbar = gtsk->pbar;
    snprintf (taskstr, sizeof (taskstr), _ ("Task (Connecting) %u/%u"), pos,
              pos + len);
    gtk_label_set_text (GTK_LABEL (label), taskstr);

    if (pos) {
        gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (pbar),
                                       (gfloat)pos / (gfloat)(pos + len));
    }

    if ((guint32)stat == len) {
        gtask_delete (sess, gtsk);
    }
}

void
task_update (session *sess, struct task *tsk)
{
    GtkWidget *pbar;
    GtkWidget *label;
    char taskstr[256];
    struct gtask *gtsk;
    guint32 pos = tsk->pos, len = tsk->len;

    gtsk = gtask_with_trans (sess, tsk->trans);
    if (!gtsk) {
        gtsk = gtask_new (sess, tsk->trans, 0);
    }

    label = gtsk->label;
    pbar = gtsk->pbar;
    snprintf (taskstr, sizeof (taskstr), _ ("Task 0x%x (%s) %u/%u"), tsk->trans,
              tsk->str ? tsk->str : "", pos, pos + len);
    gtk_label_set_text (GTK_LABEL (label), taskstr);
    if (pos) {
        gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (pbar),
                                       (gfloat)pos / (gfloat)(pos + len));
    }

    if (len == 0) {
        gtask_delete (sess, gtsk);
    }
}

static void
tasks_destroy (GtkWidget *widget, gpointer data)
{
    session *sess = data;
    (void)widget;

    /* Phase 5: gtask_scroll used to live inside an outer vbox that
	 * also held the topframe + button row; here we unparented it
	 * from that vbox so the next create_tasks_window could re-attach
	 * it as a fresh child. With the buttons moved into the
	 * AdwHeaderBar, gtask_scroll is the window's direct child — so
	 * we just unparent it from whatever its current parent is. */
    if (sess->gtask_scroll && gtk_widget_get_parent (sess->gtask_scroll)) {
        gtk_widget_unparent (sess->gtask_scroll);
    }
    gtkhx_prefs.geo.tasks.open = 0;
    gtkhx_prefs.geo.tasks.init = 0;
}

extern void tracker_kill_threads (void);
static void
task_stop (GtkWidget *widget, gpointer data)
{
    struct gtask *gtsk;
    GList *sel, *lp, *next;
    GtkWidget *listitem;
    session *sess = data;

    if (!gtkhx_prefs.geo.tasks.open) {
        return;
    }

    /* Phase 3.2: gtk_list_box_get_selected_rows returns a GList* of
	 * GtkListBoxRow* (the rows themselves, not their children).
	 * Caller owns the GList and must g_list_free() it; the rows
	 * themselves are owned by the list box. */
    sel = gtk_list_box_get_selected_rows (GTK_LIST_BOX (sess->gtklist));
    for (lp = sel; lp; lp = next) {
        next = lp->next;
        listitem = (GtkWidget *)lp->data;
        gtsk = (struct gtask *)g_object_get_data (G_OBJECT (listitem), "gtsk");

        if (gtsk->htxf) {
            xfer_delete (gtsk->htxf);
            gtask_delete (sess, gtsk);
        } else if (gtsk->trans == (guint32)-127) {
            tracker_kill_threads ();
            gtask_delete (sess, gtsk);
        } else if (gtsk->trans == (guint32)-128) {
            disconnect_clicked ();
            /* disconnect_clicked updates connection task, so it should already
			   handle deleting the task */
            /*			gtask_delete(sess, gtsk); */
        } else if (gtsk->trans == (guint32)-129) {
            tracker_kill_threads ();
            gtask_delete (sess, gtsk);
        } else {
            /* task_delete should handle deleting the gtask */
            task_delete (sess, task_with_trans (sess, gtsk->trans));
            /*			gtask_delete(sess, gtsk); */
        }
    }
    g_list_free (sel);
}

/* Phase 3.2: Move a GtkListBoxRow to a new index by ref'ing it,
 * removing it from the container, and re-inserting at the new index.
 * The list box re-acquires its ref on insert. */
static void
gtklist_row_move (GtkListBox *box, GtkWidget *row, int new_index)
{
    g_object_ref (row);
    gtkhx_widget_remove_child (GTK_WIDGET (box), row);
    gtk_list_box_insert (box, row, new_index);
    g_object_unref (row);
    gtk_list_box_select_row (box, GTK_LIST_BOX_ROW (row));
}

static void
task_up (GtkWidget *widget, gpointer data)
{
    struct gtask *gtsk;
    GList *sel;
    GtkWidget *listitem;
    int num, gtkpos;
    session *sess = data;

    if (!gtkhx_prefs.queuedl) {
        return;
    }

    sel = gtk_list_box_get_selected_rows (GTK_LIST_BOX (sess->gtklist));
    if (!sel) {
        return;
    }
    listitem = sel->data;
    g_list_free (sel);
    gtsk = g_object_get_data (G_OBJECT (listitem), "gtsk");

    if (!gtsk->htxf) {
        return;
    }

    num = xfer_num (gtsk->htxf);

    if (num <= 1) {
        return;
    }

    xfer_up (num);

    gtkpos = gtk_list_box_row_get_index (GTK_LIST_BOX_ROW (listitem));
    if (gtkpos <= 0) {
        return;
    }
    gtklist_row_move (GTK_LIST_BOX (sess->gtklist), listitem, gtkpos - 1);
}

static void
task_dn (GtkWidget *widget, gpointer data)
{
    struct gtask *gtsk;
    GList *sel;
    GtkWidget *listitem;
    int num, gtkpos;
    session *sess = data;

    if (!gtkhx_prefs.queuedl) {
        return;
    }
    sel = gtk_list_box_get_selected_rows (GTK_LIST_BOX (sess->gtklist));
    if (!sel) {
        return;
    }
    listitem = sel->data;
    g_list_free (sel);
    gtsk = g_object_get_data (G_OBJECT (listitem), "gtsk");

    if (!gtsk->htxf) {
        return;
    }

    num = xfer_num (gtsk->htxf);

    if (num <= 0) {
        return;
    }

    if (xfer_down (num)) {
        return;
    }

    gtkpos = gtk_list_box_row_get_index (GTK_LIST_BOX_ROW (listitem));
    gtklist_row_move (GTK_LIST_BOX (sess->gtklist), listitem, gtkpos + 1);
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

    sel = gtk_list_box_get_selected_rows (GTK_LIST_BOX (sess->gtklist));
    if (!sel) {
        return;
    }
    listitem = sel->data;
    g_list_free (sel);
    gtsk = (struct gtask *)g_object_get_data (G_OBJECT (listitem), "gtsk");
    if (gtsk->htxf) {
        xfer_go (gtsk->htxf);
    }
}

/* Phase 3.x: see users.c users_move() for rationale — size on
 * configure, position deferred to quit.
 * Phase 4.5: gone — GTK 4 widgets don't fire configure-event. Tasks
 * window size is captured at hx_quit() in gtkhx.c. */

static void
task_tasks_update (session *sess)
{
    GHashTableIter iter;
    gpointer val;

    if (!sess->tasks) {
        return;
    }
    g_hash_table_iter_init (&iter, sess->tasks);
    while (g_hash_table_iter_next (&iter, NULL, &val)) {
        gtkhx_session_emit_task_update (gtkhx_session_get_default (), sess,
                                        (struct task *)val);
    }
}

/* Phase 5: 2x scale on tasks-headerbar pixmap buttons (matches the
 * toolbar treatment). gtkhx_pixmap_button in gtkutil.c handles the
 * upscale + button construction. */
#define TASKS_ICON_SCALE 2

static GtkWidget *
tasks_pixmap_button (const char *resource_name, const char *tooltip,
                     GCallback cb, gpointer user_data)
{
    return gtkhx_pixmap_button (resource_name, tooltip, TASKS_ICON_SCALE, cb,
                                user_data);
}

void
create_tasks_window (GtkWidget *widget, gpointer data)
{
    GtkWidget *header;
    GtkWidget *stopbtn, *gobtn, *upbtn, *dnbtn;
    GtkWidget *tasks_window;
    session *sess = data;

    if (gtkhx_prefs.geo.tasks.open) {
        gtk_window_present (GTK_WINDOW (sess->tasks_window));
        return;
    }

    tasks_window = gtk_window_new ();
    gtk_window_set_resizable (GTK_WINDOW (tasks_window), TRUE);
    gtk_window_set_title (GTK_WINDOW (tasks_window), _ ("Tasks"));

    /* Phase 5: AdwHeaderBar replaces both the default GtkWindow
	 * title bar and the in-content "topframe + hbuttonbox" row. The
	 * four task-control buttons (Stop / Start on the start, Move Up /
	 * Down on the end) live directly in the headerbar — exactly the
	 * shape AdwHeaderBar was designed for. The content area drops
	 * down to just the task scrolledwindow. */
    header = adw_header_bar_new ();

    stopbtn
        = tasks_pixmap_button ("/com/nasledov/gtkhx/pixmaps/kick.png",
                               _ ("Stop Task"), G_CALLBACK (task_stop), sess);
    gobtn = tasks_pixmap_button ("/com/nasledov/gtkhx/pixmaps/start.png",
                                 _ ("Start Task"), G_CALLBACK (task_go), sess);
    upbtn = tasks_pixmap_button ("/com/nasledov/gtkhx/pixmaps/up.png",
                                 _ ("Move Xfer Up in Queue"),
                                 G_CALLBACK (task_up), sess);
    dnbtn = tasks_pixmap_button ("/com/nasledov/gtkhx/pixmaps/down.png",
                                 _ ("Move Xfer Down in Queue"),
                                 G_CALLBACK (task_dn), sess);

    adw_header_bar_pack_start (ADW_HEADER_BAR (header), stopbtn);
    adw_header_bar_pack_start (ADW_HEADER_BAR (header), gobtn);
    /* pack_end appends from the right edge inward, so up appears
	 * to the left of down to match the natural reading order. */
    adw_header_bar_pack_end (ADW_HEADER_BAR (header), dnbtn);
    adw_header_bar_pack_end (ADW_HEADER_BAR (header), upbtn);

    gtk_window_set_titlebar (GTK_WINDOW (tasks_window), header);

    gtk_window_set_child (GTK_WINDOW (tasks_window), sess->gtask_scroll);

    g_signal_connect (tasks_window, "destroy", G_CALLBACK (tasks_destroy),
                      sess);

    init_keyaccel (tasks_window);

    /* Phase 3.x: only apply saved geometry when the prefs file actually
	 * has one (see users.c for rationale — zero-size collapses the
	 * window under GTK 3). */
    if (gtkhx_prefs.geo.tasks.xsize > 0 && gtkhx_prefs.geo.tasks.ysize > 0) {
        gtk_window_set_default_size (GTK_WINDOW (tasks_window),
                                     gtkhx_prefs.geo.tasks.xsize,
                                     gtkhx_prefs.geo.tasks.ysize);
    }
    if (gtkhx_prefs.geo.tasks.xpos > 0 || gtkhx_prefs.geo.tasks.ypos > 0) {
        /* Phase 4.2: gtk_window_move removed (Wayland) */
        gtk_window_present (GTK_WINDOW (tasks_window));
    }

    if (connected == 1) {
        changetitlespecific (tasks_window, _ ("Tasks"));
    }
    sess->tasks_window = tasks_window;

    gtkhx_prefs.geo.tasks.open = 1;
    gtkhx_prefs.geo.tasks.init = 1;

    task_tasks_update (sess);
    xfer_tasks_update (&sess->htlc);
}

#define LONGEST_HUMAN_READABLE 32
extern char *human_size (char *sizstr, guint32 size);

void
file_update (session *sess, struct htxf_conn *htxf)
{
    GtkWidget *pbar;
    GtkWidget *label;
    struct gtask *gtsk;
    char humanbuf[LONGEST_HUMAN_READABLE + 1], *posstr, *sizestr, *bpsstr;
    guint32 pos, size;
    struct timeval now;
    time_t sdiff, Bps, eta;
    int hrs, mins, secs;

    gtsk = gtask_with_htxf (sess, htxf);
    if (!gtsk) {
        gtsk = gtask_new (sess, 0, htxf);
    }
    label = gtsk->label;
    pbar = gtsk->pbar;

    pos = htxf->total_pos;
    size = htxf->total_size;

    gettimeofday (&now, 0);
    sdiff = now.tv_sec - htxf->start.tv_sec;
    if (!sdiff) {
        sdiff = 1;
    }
    Bps = pos / sdiff;
    if (!Bps) {
        Bps = 1;
    }
    eta = (size - pos) / Bps + ((size - pos) % Bps) / Bps;

    hrs = eta / 3600;
    eta %= 3600;
    mins = eta / 60;
    eta %= 60;
    secs = eta;

    posstr = g_strdup (human_size (humanbuf, pos));
    memset (&humanbuf, 0, sizeof (humanbuf));
    sizestr = g_strdup (human_size (humanbuf, size));
    memset (&humanbuf, 0, sizeof (humanbuf));
    bpsstr = g_strdup (human_size (humanbuf, Bps));

    {
        /* htxf->path is up to MAXPATHLEN (4096) bytes — if we held the
		 * format result in a fixed-size local snprintf walks GCC's
		 * worst-case length analysis right into a -Wformat-truncation
		 * warning. Use g_strdup_printf and let GLib size the buffer. */
        char *line
            = g_strdup_printf (_ ("%s  %s/%s  %s/s  ETA: %d:%02d:%02d  %s"),
                               htxf->type == XFER_GET ? "get" : "put", posstr,
                               sizestr, bpsstr, hrs, mins, secs, htxf->path);
        gtk_label_set_text (GTK_LABEL (label), line);
        g_free (line);
    }

    if (((gfloat)pos / size) <= 1) {
        gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (pbar),
                                       (gfloat)pos / size);
    }

    g_free (posstr);
    g_free (sizestr);
    g_free (bpsstr);

    if (pos >= size) {
        gtask_delete (sess, gtsk);
    }
}

/* Phase 5+: task lifecycle on GHashTable.
 *
 * The GHashTable factory + value-destroy notify (task_free) live in
 * tasks_table.c so the unit tests can build the same table the
 * runtime uses without pulling in GTK. task_delete() below is the
 * public removal entry point and additionally notifies the UI side
 * (gtask_delete_tsk peels off the Tasks window's progress row);
 * g_hash_table_remove then invokes task_free() for the heap
 * cleanup. */

void
tasks_init (session *sess)
{
    if (!sess->tasks) {
        sess->tasks = tasks_table_new ();
    }
}

struct task *
task_new (struct htlc_conn *htlc, rcv_task_fn rcv, void *ptr, void *data,
          const char *str)
{
    struct task *tsk;
    session *sess = &the_session;

    tsk = g_malloc0 (sizeof (struct task));
    tsk->trans = htlc->trans;
    tsk->data = data;
    tsk->str = str ? g_strdup (str) : NULL;
    tsk->ptr = ptr;
    tsk->rcv = rcv;
    tsk->pos = 0;
    tsk->len = 1;

    g_hash_table_insert (sess->tasks, GUINT_TO_POINTER (tsk->trans), tsk);
    gtkhx_session_emit_task_update (gtkhx_session_get_default (), sess, tsk);
    return tsk;
}

void
task_delete (session *sess, struct task *tsk)
{
    if (!tsk) {
        return;
    }
    gtask_delete_tsk (sess, tsk->trans);
    g_hash_table_remove (sess->tasks, GUINT_TO_POINTER (tsk->trans));
}

struct task *
task_with_trans (session *sess, guint32 trans)
{
    return g_hash_table_lookup (sess->tasks, GUINT_TO_POINTER (trans));
}

/* task_error_extract lives in proto_helpers.c so the Tier 2 unit
 * tests can drive it without a GTK build. The prototype is in
 * tasks.h via #include "proto_helpers.h". */

void
task_error (struct htlc_conn *htlc)
{
    char errormsg[8192 + 1];
    gsize len = 0;

    if (!task_error_extract (htlc, errormsg, sizeof (errormsg), &len)) {
        return;
    }

    (void)len;
    /* Phase 5: server task errors used to pop a modal
	 * error_dialog. That's too heavy for the common case —
	 * the server rejecting one of our auto-fired bootstrap
	 * requests (news fetch on a no-news-permission account,
	 * user list on a server that rate-limits, etc.) made
	 * every login on certain servers (hlserver.com is the
	 * known-bad example) yield a dialog the user has to
	 * click through before they can do anything else.
	 *
	 * Toast instead: AdwToast slides in over the toolbar
	 * window's overlay, auto-dismisses after a few seconds,
	 * doesn't steal focus. The user still sees the message;
	 * they're just not blocked by it. The ERROR sound still
	 * fires so the alert isn't fully silent.
	 *
	 * Phase 5+: errormsg may contain MacRoman bytes (Mac servers
	 * commonly hand us curly quotes \xd2/\xd3 around filenames).
	 * toolbar_show_toast sanitises to UTF-8 internally — the
	 * accessibility announcement layer behind AdwToast aborts on
	 * non-UTF-8, so the defence has to be at that choke point. */
    toolbar_show_toast (errormsg);
    play_sound (ERROR);
}
