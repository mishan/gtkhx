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
#include <ctype.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include "hx.h"
#include "gtk_hlist.h"
#include "macres.h"
#include "xfers.h"
#include "toolbar.h"
#include "gtkutil.h"
#include "gtkhx.h"
#include "cicn.h"
#include "tasks.h"
#include "rcv.h"
#include "files.h"


#define ICON_FILE	400
#define ICON_FOLDER	401
#define ICON_FOLDER_IN	421
#define ICON_FILE_HTft	402
#define ICON_FILE_SIT	403
#define ICON_FILE_TEXT	404
#define ICON_FILE_IMAGE	406
#define ICON_FILE_APPL	407
#define ICON_FILE_HTLC	408
#define ICON_FILE_SITP	409
#define ICON_FILE_alis	422
#define ICON_FILE_DISK	423
#define ICON_FILE_NOTE	424
#define ICON_FILE_MOOV	425
#define ICON_FILE_ZIP	426

guint8 dir_char  = '/';

struct gfile_list *gfile_list;

static struct gfile_list *gfl_new (GtkWidget *window, GtkWidget *hlist,
								   char *path)
{
	struct gfile_list *gfl;

	gfl = g_malloc(sizeof(struct gfile_list));
	gfl->next = 0;
	gfl->prev = 0;

	if (gfile_list) {
		gfile_list->next = gfl;
		gfl->prev = gfile_list;
	}

	gfl->cfl = 0;
	gfl->window = window;
	gfl->hlist = hlist;
	gfl->row = 0;
	gfl->column = 0;

	gfl->path_list = g_malloc(sizeof(struct path_hist)+strlen(path));
	strcpy(gfl->path_list->path, path);
	gfl->path_list->prev = NULL;

	gfl->in_use = 0;

	gfile_list = gfl;

	return gfl;
}

static void gfl_delete (struct gfile_list *gfl)
{
	struct path_hist *path, *prev;

	for(path = gfl->path_list; path; path = prev) {
		if(path->prev) {
			prev = path->prev;
		}
		else {
			prev = 0;
		}
		g_free(path);
	}

	g_free(gfl->cfl);

	if (gfl->next)
		gfl->next->prev = gfl->prev;
	if (gfl->prev)
		gfl->prev->next = gfl->next;
	if (gfl == gfile_list)
		gfile_list = gfl->prev;
	g_free(gfl);
}

void destroy_gfl_list(void)
{
	struct gfile_list *gfl, *prev;


	for(gfl = gfile_list; gfl; gfl = prev) {
		prev = gfl->prev;
		gtkhx_widget_destroy(gfl->window);
		gfl_delete(gfl);
	}
	gfile_list = 0;
}

static struct gfile_list *gfl_with_hlist (GtkWidget *hlist)
{
	struct gfile_list *gfl;

	for(gfl = gfile_list; gfl; gfl = gfl->prev) {
		if(gfl->hlist == hlist) {
			return gfl;
		}
	}

	return 0;
}

static struct gfile_list *gfl_with_path (char *path)
{
	struct gfile_list *gfl;

	for(gfl = gfile_list; gfl; gfl = gfl->prev) {
		if(!strcmp(path, gfl->cfl->path)) {
			return gfl;
		}
	}

	return 0;
}

static void
open_fldr (struct cached_filelist *cfl, struct hl_filelist_hdr *fh,
		   struct gfile_list *gfl)
{
	char path[4096];
	char *curr_path = g_strdup_printf("%.*s", (int)fh->fnlen, fh->fname);

	if(gfl->in_use) {
		g_free(curr_path);
		return;
	}

	if(cfl->path[0] == '/' && cfl->path[1] == 0) {
		snprintf(path, sizeof(path), "/%s", curr_path);
	}
	else {
		snprintf(path, sizeof(path), "%s/%s", cfl->path, curr_path);
	}
	g_free(curr_path);


	gfl->row = 0;
	hx_list_dir(&the_session.htlc, path, 1, 0, gfl);
}

static void
get_file (struct cached_filelist *cfl, struct hl_filelist_hdr *fh)
{
	char rpath[4096], lpath[4096];
	struct htxf_conn *htxf;
	struct stat sb;
	guint32 fsize;

	snprintf(rpath, sizeof(rpath), "%s/%.*s", cfl->path, (int)fh->fnlen,
			fh->fname);
	snprintf(lpath, sizeof(lpath), "%s/%.*s", gtkhx_prefs.download_path,
			(int)fh->fnlen, fh->fname);

	if(stat(gtkhx_prefs.download_path, &sb)) {
		if(mkdir(gtkhx_prefs.download_path, 0770)) {
			g_warning("%s: %s", _("cannot create download directory"),
					  gtkhx_prefs.download_path);
			g_warning(_("aborting download"));
			return;
		}
	}

	HN32(&fsize, &fh->fsize);
	htxf = xfer_new(lpath, rpath, XFER_GET, 0, fsize);
	htxf->filter_argv = 0;
	htxf->opt.retry = 0;
}

/* Phase 3.2: GtkFileSelection was removed in GTK 3 (deprecated since
 * 2.4).  Replaced wholesale with GtkFileChooserDialog.  The old
 * "ok_button"/"cancel_button" public field-clicked-handler dance becomes
 * a single "response" handler that switches on GTK_RESPONSE_ACCEPT vs.
 * GTK_RESPONSE_CANCEL.  files_list is passed in via user_data instead
 * of being stashed on the OK button. */
/* Phase 4.13: GtkFileChooserDialog and gtk_file_chooser_get_file are
 * deprecated in GTK 4.10 — replacement is GtkFileDialog with an async
 * open/save callback. Phase 4.7 follow-up tracks the migration. */
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
static void
upload_file_response(GtkDialog *dialog, gint response_id, gpointer user_data)
{
	GtkWidget *files_list = user_data;
	struct gfile_list *gfl;
	char *lpath;
	char rpath[4096];

	if (response_id == GTK_RESPONSE_ACCEPT) {
		/* Phase 4.7: gtk_file_chooser_get_filename returned a g_malloc'd
		 * char* in GTK 3. In GTK 4 the chooser returns a GFile, so we
		 * grab the path off it and free the GFile afterwards. */
		GFile *gf = gtk_file_chooser_get_file (GTK_FILE_CHOOSER (dialog));
		lpath = gf ? g_file_get_path (gf) : NULL;
		gfl = gfl_with_hlist(files_list);
		if (gfl && gfl->cfl && lpath) {
			snprintf(rpath, sizeof(rpath), "%s/%s",
					 gfl->cfl->path, basename(lpath));
			hx_put_file(&the_session.htlc, lpath, rpath);
		}
		g_free(lpath);
		if (gf) g_object_unref (gf);
	}
	gtkhx_widget_destroy(GTK_WIDGET(dialog));
}

static void get_put_data (GtkWidget *widget, gpointer data)
{
	GtkRoot *root = gtk_widget_get_root (widget);
	GtkWidget *file_dialog = gtk_file_chooser_dialog_new(
		_("Upload..."),
		GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL,
		GTK_FILE_CHOOSER_ACTION_OPEN,
		_("_Cancel"), GTK_RESPONSE_CANCEL,
		_("_Open"),   GTK_RESPONSE_ACCEPT,
		NULL);

	g_signal_connect(file_dialog, "response",
					 G_CALLBACK(upload_file_response), data);

	gtk_window_present(GTK_WINDOW(file_dialog));
}
G_GNUC_END_IGNORE_DEPRECATIONS

/* Phase 4.5: button-press-event is gone in GTK 4. Files-list single
 * and double click handling lives on a GtkGestureClick controller
 * now; n_press == 2 gates open-folder / get-file, single-click just
 * remembers the row for the toolbar buttons. */
static void file_pressed (GtkGestureClick *gesture, int n_press,
                          double x, double y, gpointer data)
{
	GtkWidget *widget = gtk_event_controller_get_widget (
		GTK_EVENT_CONTROLLER (gesture));
	struct gfile_list *gfl;
	int row, column;
	(void) data;

	gfl = gfl_with_hlist(widget);

	if (!gfl)
		return;

	gtk_hlist_get_selection_info(GTK_HLIST(widget),
				     (int) x, (int) y, &row, &column);

	if (n_press == 2) {
		struct hl_filelist_hdr *fh;

		fh = gtk_hlist_get_row_data(GTK_HLIST(widget), gfl->row);
		if (fh) {
			if(gfl->cfl) {
				if (!memcmp(&fh->ftype, "fldr", 4)) {
					open_fldr(gfl->cfl, fh, gfl);
				} else {
					get_file(gfl->cfl, fh);
				}
			}
		}
	}
	else {
		gfl->row = row;
		gfl->column = column;
	}
}

static void delete_file (GtkWidget *widget, gpointer data)
{
	struct gfile_list *gfl;
	struct hl_filelist_hdr *fh;
	char path[4096];

	gfl = gfl_with_hlist((GtkWidget *)data);
	fh = gtk_hlist_get_row_data(GTK_HLIST(data), gfl->row);
	

	snprintf(path, sizeof(path), "%s/%.*s", gfl->cfl->path,
			 (int)fh->fnlen, fh->fname);
	hx_file_delete(&the_session.htlc, path);
	hx_list_dir(&the_session.htlc, gfl->cfl->path, 1, 0, gfl);
 }

static void get_file_info(GtkWidget *widget, gpointer data)
{
	struct gfile_list *gfl;
	struct hl_filelist_hdr *fh;
	char path[4096];

	gfl = gfl_with_hlist((GtkWidget *)data);
	fh = gtk_hlist_get_row_data(GTK_HLIST(data), gfl->row);

	g_snprintf(path, sizeof(path), "%s/%.*s", gfl->cfl->path,
			 (int)fh->fnlen, fh->fname);

	hx_file_info(&the_session.htlc, path);
}

static void file_up_btn (GtkWidget *widget, gpointer data)
{
	struct gfile_list *gfl;
	struct path_hist *path;

	if(!gtkhx_prefs.file_samewin) {
		return;
	}

	gfl = gfl_with_hlist((GtkWidget *)data);

	if(!gfl) {
		return;
	}

	if(gfl->in_use) {
		return;
	}

	if(gfl->path_list->prev) {
		path = gfl->path_list;
		gfl->path_list = gfl->path_list->prev;
		g_free(path);
	}
	else {
		return;
	}


	gfl->in_use = 1;
	gfl->row = 0;
	hx_list_dir(&the_session.htlc, gfl->path_list->path, 1, 0, gfl);
}

static void file_dl_btn (GtkWidget *widget, gpointer data)
{
	struct gfile_list *gfl;
	struct hl_filelist_hdr *fh;
	char rpath[4096], lpath[4096];
	struct htxf_conn *htxf;

	gfl = gfl_with_hlist((GtkWidget *)data);
	fh = gtk_hlist_get_row_data(GTK_HLIST(data), gfl->row);

	snprintf(rpath, sizeof(rpath), "%s/%.*s", gfl->cfl->path,
			 (int)fh->fnlen, fh->fname);

	snprintf(lpath, sizeof(lpath), "%s/%.*s", gtkhx_prefs.download_path,
			 (int)fh->fnlen, fh->fname);

	if(!memcmp(&fh->ftype, "fldr", 4)) {
		return;
	}

	{
		guint32 fsize;
		HN32(&fsize, &fh->fsize);
		htxf = xfer_new(lpath, rpath, XFER_GET, 0, fsize);
	}
	htxf->filter_argv = 0;
	htxf->opt.retry = 0;
}

static void file_pre_btn (GtkWidget *widget, gpointer data)
{
	struct gfile_list *gfl;
	struct hl_filelist_hdr *fh;
	char rpath[4096], lpath[4096];
	struct htxf_conn *htxf;

	gfl = gfl_with_hlist((GtkWidget *)data);
	fh = gtk_hlist_get_row_data(GTK_HLIST(data), gfl->row);

	snprintf(rpath, sizeof(rpath), "%s/%.*s", gfl->cfl->path,
			 (int)fh->fnlen, fh->fname);

	snprintf(lpath, sizeof(lpath), "%s/%.*s", gtkhx_prefs.download_path,
			 (int)fh->fnlen, fh->fname);

	if(!memcmp(&fh->ftype, "fldr", 4)) {
		return;
	}

	/* opt.preview is set inside xfer_new (before the inner xfer_go
	 * call) so the resume / rename decision is correctly skipped
	 * for previews. Setting it on the returned htxf here would be
	 * too late. srv_data_size is irrelevant for previews — they
	 * never resume — so 0 is fine. */
	htxf = xfer_new(lpath, rpath, XFER_GET, 1, 0);
	htxf->filter_argv = 0;
	htxf->opt.retry = 0;
}


static void file_reload_btn (GtkWidget *widget, gpointer data)
{
	GtkWidget *files_list = (GtkWidget *)data;
	struct gfile_list *gfl;

	gfl = gfl_with_hlist(files_list);

	if(!gfl->cfl) {
		return;
	}

	gtk_hlist_clear(GTK_HLIST(files_list));
	hx_list_dir(&the_session.htlc, gfl->cfl->path, 1, 0, gfl);
}

/* Phase 4.5: GTK 4 fires "close-request" instead of "delete-event"
 * (GtkWindow *, gpointer) returning TRUE to inhibit close, FALSE to
 * allow the default destroy. The body just clears the per-window
 * model state — the framework destroys the widget itself. */
static gboolean close_files_window (GtkWindow *window, gpointer data)
{
	struct gfile_list *gfl = (struct gfile_list *)g_object_get_data(
		G_OBJECT(window), "gfl");
	(void) data;

	gfl_delete(gfl);
	return FALSE;
}

/* Phase 5: AdwAlertDialog with a GtkEntry as extra-child replaces
 * the hand-rolled GtkDialog + label + entry + OK/Cancel buttons.
 * The response callback handles both buttons (id-string dispatch);
 * the entry's "activate" signal forwards to the same response so
 * Enter-to-submit works. The dialog auto-dismisses when the
 * response handler returns. */

static void
makeDir_response (AdwAlertDialog *dialog, const char *response, gpointer data)
{
	GtkEditable *entry;
	GtkWidget *files_list = data;
	struct gfile_list *gfl;
	char pathname[MAXPATHLEN];

	if (g_strcmp0 (response, "create") != 0)
		return;

	entry = GTK_EDITABLE (adw_alert_dialog_get_extra_child (dialog));
	gfl = gfl_with_hlist (files_list);
	if (!gfl || !entry)
		return;

	snprintf (pathname, MAXPATHLEN, "%s/%s",
	          gfl->cfl->path, gtk_editable_get_text (entry));
	hx_make_dir (&the_session.htlc, pathname);
	hx_list_dir (&the_session.htlc, gfl->cfl->path, 1, 0, gfl);
}

static void
makeDir_entry_activate (GtkEntry *entry, gpointer data)
{
	(void) entry;
	/* adw_alert_dialog_response (the obvious "activate this response
	 * id" call) is libadwaita 1.7+; our floor is 1.6. Emitting the
	 * "response" signal by name does the same thing — handlers run,
	 * dialog auto-closes. */
	g_signal_emit_by_name (data, "response", "create");
}

static void makeDirDialog (GtkWidget *widget, gpointer data)
{
	AdwDialog *dialog;
	GtkWidget *entry;

	dialog = adw_alert_dialog_new (_("New Folder"),
	                               _("Enter a name for the new folder."));
	adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog),
	                               "cancel", _("_Cancel"));
	adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog),
	                               "create", _("C_reate"));
	adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog),
	                                          "create",
	                                          ADW_RESPONSE_SUGGESTED);
	adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "create");
	adw_alert_dialog_set_close_response   (ADW_ALERT_DIALOG (dialog), "cancel");

	entry = gtk_entry_new ();
	gtk_entry_set_activates_default (GTK_ENTRY (entry), TRUE);
	g_signal_connect (entry, "activate",
	                  G_CALLBACK (makeDir_entry_activate), dialog);
	adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dialog), entry);

	g_signal_connect (dialog, "response",
	                  G_CALLBACK (makeDir_response), data);

	adw_dialog_present (dialog, widget);
}

/* Phase 4.8: drag-and-drop between file lists.
 *
 * GTK 4 replaces GtkTargetEntry / gtk_drag_source_set / gtk_drag_dest_set
 * (with the "drag_data_get" + "drag_data_received" signal pair) with two
 * event controllers: GtkDragSource on the source widget, advertising a
 * GdkContentProvider; GtkDropTarget on the destination, accepting one or
 * more GTypes. There is no longer a gtk_drag_get_source_widget(context)
 * accessor on the receive side — the source has to actually push data
 * across.
 *
 * The original GTK 3 code cheated: drag_send was a no-op (selection_data
 * empty) and drag_receive used gtk_drag_get_source_widget to fetch the
 * source widget directly. To keep the wire-payload trivial we mirror
 * that intra-app-only design: the content provider holds a GtkWidget*
 * (GTK_TYPE_WIDGET) pointing at the source files_list, and the drop
 * callback derives source/target gfl via gfl_with_hlist on each end.
 *
 * The selected source row was already recorded by file_pressed on the
 * preceding click (gfl->row), so we don't have to capture it at
 * drag-start time. */
static gboolean files_drop_cb (GtkDropTarget *target, const GValue *value,
                               double x, double y, gpointer user_data)
{
	GtkWidget *target_widget;
	GtkWidget *source_widget;
	struct gfile_list *gfl_source, *gfl_target;
	struct hl_filelist_hdr *fh;
	char pathf[4096], patht[4096];

	(void) x;
	(void) y;
	(void) user_data;

	target_widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (target));

	if (!G_VALUE_HOLDS (value, GTK_TYPE_WIDGET))
		return FALSE;
	source_widget = g_value_get_object (value);
	if (!source_widget || source_widget == target_widget)
		return FALSE;	/* same window — let it be a no-op */

	gfl_source = gfl_with_hlist (source_widget);
	gfl_target = gfl_with_hlist (target_widget);
	if (!gfl_source || !gfl_target)
		return FALSE;

	fh = gtk_hlist_get_row_data (GTK_HLIST (source_widget), gfl_source->row);
	if (!fh)
		return FALSE;

	if (strcmp (gfl_source->cfl->path, gfl_target->cfl->path) == 0)
		return FALSE;	/* same directory; nothing to do */

	g_snprintf (pathf, sizeof pathf, "%s/%.*s", gfl_source->cfl->path,
	            (int) fh->fnlen, fh->fname);
	g_snprintf (patht, sizeof patht, "%s/", gfl_target->cfl->path);

	hx_file_move (&the_session.htlc, pathf, patht);
	/*	hx_file_link(&the_session.htlc, pathf, patht);
	 *
	 *	XXX: Pop up a dialog and prompt the user whether he wants to
	 *	move or link the file or cancel — preserved from the GTK 3
	 *	code so we don't lose the design intent. */

	hx_list_dir (&the_session.htlc, gfl_target->cfl->path, 1, 0, gfl_target);
	hx_list_dir (&the_session.htlc, gfl_source->cfl->path, 1, 0, gfl_source);

	return TRUE;
}

static void files_attach_dnd (GtkWidget *files_list)
{
	GtkDragSource *source;
	GtkDropTarget *target;
	GdkContentProvider *provider;
	GValue widget_value = G_VALUE_INIT;

	g_value_init (&widget_value, GTK_TYPE_WIDGET);
	g_value_set_object (&widget_value, files_list);
	provider = gdk_content_provider_new_for_value (&widget_value);
	g_value_unset (&widget_value);

	source = gtk_drag_source_new ();
	gtk_drag_source_set_content (source, provider);
	gtk_drag_source_set_actions (source, GDK_ACTION_MOVE);
	g_object_unref (provider);
	gtk_widget_add_controller (files_list, GTK_EVENT_CONTROLLER (source));

	target = gtk_drop_target_new (GTK_TYPE_WIDGET, GDK_ACTION_MOVE);
	g_signal_connect (target, "drop", G_CALLBACK (files_drop_cb), NULL);
	gtk_widget_add_controller (files_list, GTK_EVENT_CONTROLLER (target));
}

static struct gfile_list *create_files_window (char *path)
{
	GtkWidget *files_window;
	GtkWidget *files_list;
	GtkWidget *files_window_scroll;
	GtkWidget *reloadbtn;
	GtkWidget *downloadbtn;
	GtkWidget *crtfldbtn;
	GtkWidget *filinfobtn;
	GtkWidget *uploadbtn;
	GtkWidget *delbtn;
	GtkWidget *upbtn;
	GtkWidget *prebtn;
	GtkWidget *vbox;
	GtkWidget *hbuttonbox;
	GtkWidget *topframe;
	GdkBitmap *mask;
	GdkPixmap *icon;
	GtkWidget *pix;
	struct gfile_list *gfl;
	gchar *titles[2];

	titles[0] = _("Size");
	titles[1] = _("Name");

	files_list = gtk_hlist_new_with_titles(2, titles);
	gtk_hlist_set_column_width(GTK_HLIST(files_list), 0, 64);
	gtk_hlist_set_column_width(GTK_HLIST(files_list), 1, 240);
	gtk_hlist_set_row_height(GTK_HLIST(files_list), 18);
	gtk_hlist_set_shadow_type(GTK_HLIST(files_list), GTK_SHADOW_NONE);
	gtk_hlist_set_column_justification(GTK_HLIST(files_list), 0,
									   GTK_JUSTIFY_LEFT);
	{
		/* Phase 4.5: button-press-event is gone — gesture controller
		 * dispatches single-click row tracking and double-click
		 * open-folder / get-file via file_pressed. */
		GtkGesture *click = gtk_gesture_click_new ();
		gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click),
		                               GDK_BUTTON_PRIMARY);
		g_signal_connect (click, "pressed",
		                  G_CALLBACK (file_pressed), NULL);
		gtk_widget_add_controller (files_list,
		                           GTK_EVENT_CONTROLLER (click));
	}
	/* Phase 4.8: drag-and-drop between file lists. See files_drop_cb /
	 * files_attach_dnd above. */
	files_attach_dnd (files_list);

	files_window = gtk_window_new();
	/* Phase 5: AdwHeaderBar across all GtkHx windows for visual
	 * consistency. */
	gtk_window_set_titlebar(GTK_WINDOW(files_window), adw_header_bar_new());
	gtk_window_set_resizable(GTK_WINDOW(files_window), TRUE);

	/* Phase 3.x: dropped GTK 1.2-era realize+get_style pair (style unused). */
	gtk_window_set_title(GTK_WINDOW(files_window), path);
	gtk_widget_set_size_request(files_window, 264, 400);

	gfl = gfl_new(files_window, files_list, path);
	g_object_set_data(G_OBJECT(files_window), "gfl", gfl);
	g_signal_connect(files_window, "close-request",
			   G_CALLBACK(close_files_window), files_list);

	files_window_scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(files_window_scroll),
								   GTK_POLICY_AUTOMATIC, GTK_POLICY_ALWAYS);

	topframe = gtk_frame_new(0);
	gtk_widget_set_size_request(topframe, -1, 30);

	hbuttonbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

	upbtn = gtk_button_new();
	gfl->up_btn = upbtn;
	g_signal_connect(upbtn, "clicked",
					   G_CALLBACK(file_up_btn), files_list);
	gtk_widget_set_tooltip_text(upbtn, _("Parent Directory"));
	icon = (GdkPixmap *)gdk_pixbuf_new_from_resource("/com/nasledov/gtkhx/pixmaps/up.xpm", NULL);
	pix = gtkhx_image_new_from_pixbuf((GdkPixbuf *)icon);
	gtkhx_widget_set_child(upbtn, pix);
	pix = 0, icon = 0, mask = 0;

	reloadbtn =  gtk_button_new();
	g_signal_connect(reloadbtn, "clicked",
					   G_CALLBACK(file_reload_btn), files_list);
	gtk_widget_set_tooltip_text(reloadbtn, _("Reload"));
	icon = (GdkPixmap *)gdk_pixbuf_new_from_resource("/com/nasledov/gtkhx/pixmaps/refresh.xpm", NULL);
	pix = gtkhx_image_new_from_pixbuf((GdkPixbuf *)icon);
	gtkhx_widget_set_child(reloadbtn, pix);
	pix = 0, icon = 0, mask = 0;

	downloadbtn = gtk_button_new();
	gtk_widget_set_tooltip_text(downloadbtn, _("Download"));
	icon = (GdkPixmap *)gdk_pixbuf_new_from_resource("/com/nasledov/gtkhx/pixmaps/dl.xpm", NULL);
	pix = gtkhx_image_new_from_pixbuf((GdkPixbuf *)icon);
	g_signal_connect(downloadbtn, "clicked",
					   G_CALLBACK(file_dl_btn), files_list);
	gtkhx_widget_set_child(downloadbtn, pix);
	pix = 0, icon = 0, mask = 0;

	prebtn = gtk_button_new();
	icon = (GdkPixmap *)gdk_pixbuf_new_from_resource("/com/nasledov/gtkhx/pixmaps/preview.xpm", NULL);
	pix = gtkhx_image_new_from_pixbuf((GdkPixbuf *)icon);
	gtk_widget_set_tooltip_text(prebtn, _("Preview"));
	g_signal_connect(prebtn, "clicked",
					   G_CALLBACK(file_pre_btn), files_list);
	gtkhx_widget_set_child(prebtn, pix);
	pix = 0, icon = 0, mask = 0;

	uploadbtn = gtk_button_new();
	gtk_widget_set_tooltip_text(uploadbtn, _("Upload"));
	icon = (GdkPixmap *)gdk_pixbuf_new_from_resource("/com/nasledov/gtkhx/pixmaps/ul.xpm", NULL);
	g_signal_connect(uploadbtn, "clicked",
					   G_CALLBACK(get_put_data), files_list);
	pix = gtkhx_image_new_from_pixbuf((GdkPixbuf *)icon);
	gtkhx_widget_set_child(uploadbtn, pix);
	pix = 0, icon = 0, mask = 0;

	crtfldbtn = gtk_button_new();
	gtk_widget_set_tooltip_text(crtfldbtn, _("New Folder"));
	icon = (GdkPixmap *)gdk_pixbuf_new_from_resource("/com/nasledov/gtkhx/pixmaps/mkdir.xpm", NULL);
	pix = gtkhx_image_new_from_pixbuf((GdkPixbuf *)icon);
	gtkhx_widget_set_child(crtfldbtn, pix);
	g_signal_connect(crtfldbtn, "clicked",
					   G_CALLBACK(makeDirDialog), files_list);
	pix = 0, icon = 0, mask = 0;

	filinfobtn = gtk_button_new();
	gtk_widget_set_tooltip_text(filinfobtn, _("Info"));
	icon = (GdkPixmap *)gdk_pixbuf_new_from_resource("/com/nasledov/gtkhx/pixmaps/info.xpm", NULL);
	pix = gtkhx_image_new_from_pixbuf((GdkPixbuf *)icon);
	gtkhx_widget_set_child(filinfobtn, pix);
	g_signal_connect(filinfobtn, "clicked",
					   G_CALLBACK(get_file_info), files_list);
	pix = 0, icon = 0, mask = 0;

	delbtn = gtk_button_new();
	gtk_widget_set_tooltip_text(delbtn, _("Delete"));
	icon = (GdkPixmap *)gdk_pixbuf_new_from_resource("/com/nasledov/gtkhx/pixmaps/trash.xpm", NULL);
	pix = gtkhx_image_new_from_pixbuf((GdkPixbuf *)icon);
	gtkhx_widget_set_child(delbtn, pix);
	g_signal_connect(delbtn, "clicked",
					   G_CALLBACK(delete_file), files_list);
	pix = 0, icon = 0, mask = 0;


	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_size_request(vbox, 240, 400);
	gtkhx_box_pack(hbuttonbox, upbtn, 0, 0, 2);
	gtkhx_box_pack(hbuttonbox, reloadbtn, 0, 0, 2);
	gtkhx_box_pack(hbuttonbox, downloadbtn, 0, 0, 2);
	gtkhx_box_pack(hbuttonbox, uploadbtn, 0, 0, 2);
	gtkhx_box_pack(hbuttonbox, crtfldbtn, 0, 0, 2);
	gtkhx_box_pack(hbuttonbox, filinfobtn, 0, 0, 2);
	gtkhx_box_pack(hbuttonbox, delbtn, 0, 0, 2);
	gtkhx_box_pack(hbuttonbox, prebtn, 0, 0, 2);

	gtkhx_widget_set_child(topframe, hbuttonbox);
	gtkhx_box_pack(vbox, topframe, 0, 0, 0);
	gtkhx_widget_set_child(files_window_scroll, files_list);
	gtkhx_box_pack(vbox, files_window_scroll, 1, 1, 0);
	gtkhx_widget_set_child(files_window, vbox);

	gtk_window_present(GTK_WINDOW(files_window));
	init_keyaccel(files_window);

	gfl->cfl = NULL;

	return gfl;
}

void
open_files (void)
{

	struct gfile_list *gfl = create_files_window("/");

	hx_list_dir(&the_session.htlc, "/", 1, 0, gfl);
}

/* fileutils-4.0/lib/human.c */
#define LONGEST_HUMAN_READABLE	32

const char human_suffixes[] = {
	0,	    /* not used */
	'k',	/* kilo */
	'M',	/* Mega */
	'G',	/* Giga */
	'T',	/* Tera */
	'P',	/* Peta */
	'E',	/* Exa */
	'Z',	/* Zetta */
	'Y'	    /* Yotta */
};

/* Convert N to a human readable format in BUF.

   N is expressed in units of FROM_BLOCK_SIZE.  FROM_BLOCK_SIZE must
   be positive.

   If OUTPUT_BLOCK_SIZE is positive, use units of OUTPUT_BLOCK_SIZE in
   the output number.  OUTPUT_BLOCK_SIZE must be a multiple of
   FROM_BLOCK_SIZE or vice versa.

   If OUTPUT_BLOCK_SIZE is negative, use a format like "127k" if
   possible, using powers of -OUTPUT_BLOCK_SIZE; otherwise, use
   ordinary decimal format.  Normally -OUTPUT_BLOCK_SIZE is either
   1000 or 1024; it must be at least 2.  Most people visually process
   strings of 3-4 digits effectively, but longer strings of digits are
   more prone to misinterpretation.  Hence, converting to an
   abbreviated form usually improves readability.  Use a suffix
   indicating which power is being used.  For example, assuming
   -OUTPUT_BLOCK_SIZE is 1024, 8500 would be converted to 8.3k,
   133456345 to 127M, 56990456345 to 53G, and so on.  Numbers smaller
   than -OUTPUT_BLOCK_SIZE aren't modified.  */

char *
human_readable (guint32 n, char *buf,
		int from_block_size, int output_block_size)
{
  guint32 amt;
  uint base;
  int to_block_size;
  uint tenths;
  uint power = 0;
  char *p;

  /* 0 means adjusted N == AMT.TENTHS;
     1 means AMT.TENTHS < adjusted N < AMT.TENTHS + 0.05;
     2 means adjusted N == AMT.TENTHS + 0.05;
     3 means AMT.TENTHS + 0.05 < adjusted N < AMT.TENTHS + 0.1.  */
  uint rounding;

  if (output_block_size < 0)
    {
      base = -output_block_size;
      to_block_size = 1;
    }
  else
    {
      base = 0;
      to_block_size = output_block_size;
    }

  p = buf + LONGEST_HUMAN_READABLE;
  *p = '\0';

  /* Adjust AMT out of FROM_BLOCK_SIZE units and into TO_BLOCK_SIZE units.  */

  if (to_block_size <= from_block_size)
    {
      int multiplier = from_block_size / to_block_size;
      amt = n * multiplier;
      tenths = rounding = 0;

      if (amt / multiplier != n)
	{
	  /* Overflow occurred during multiplication.  We should use
	     multiple precision arithmetic here, but we'll be lazy and
	     resort to floating point.  This can yield answers that
	     are slightly off.  In practice it is quite rare to
	     overflow uintmax_t, so this is good enough for now.  */

	  double damt = n * (double) multiplier;

	  if (! base)
	    g_snprintf (buf, LONGEST_HUMAN_READABLE, "%.0f", damt);
	  else
	    {
	      double e = 1;
	      power = 0;

	      do
		{
		  e *= base;
		  power++;
		}
	      while (e * base <= damt && power < sizeof(human_suffixes) - 1);

	      damt /= e;

	      g_snprintf (buf, LONGEST_HUMAN_READABLE, "%.1f%c", damt, human_suffixes[power]);
	      if (4 < strlen (buf))
		g_snprintf (buf, LONGEST_HUMAN_READABLE, "%.0f%c", damt, human_suffixes[power]);
	    }

	  return buf;
	}
    }
  else
    {
      uint divisor = to_block_size / from_block_size;
      uint r10 = (n % divisor) * 10;
      uint r2 = (r10 % divisor) * 2;
      amt = n / divisor;
      tenths = r10 / divisor;
      rounding = r2 < divisor ? 0 < r2 : 2 + (divisor < r2);
    }


  /* Use power of BASE notation if adjusted AMT is large enough.  */

  if (base && base <= amt)
    {
      power = 0;

      do
	{
	  uint r10 = (amt % base) * 10 + tenths;
	  uint r2 = (r10 % base) * 2 + (rounding >> 1);
	  amt /= base;
	  tenths = r10 / base;
	  rounding = (r2 < base
		      ? 0 < r2 + rounding
		      : 2 + (base < r2 + rounding));
	  power++;
	}
      while (base <= amt && power < sizeof(human_suffixes) - 1);

      *--p = human_suffixes[power];

	  tenths += 2 < rounding + (tenths & 1);

	  if (tenths == 10)
	    {
	      amt++;
	      tenths = 0;
	    }

	      *--p = '0' + tenths;
	      *--p = '.';
	      tenths = 0;
    }

  if (5 < tenths + (2 < rounding + (amt & 1)))
    {
      amt++;

      if (amt == base && power < sizeof(human_suffixes) - 1)
	{
	  *p = human_suffixes[power + 1];
	  *--p = '0';
	  *--p = '.';
	  amt = 1;
	}
    }

  do
    *--p = '0' + (int) (amt % 10);
  while ((amt /= 10) != 0);

  return p;
}

char *human_size(char *sizstr, guint32 size)
{
	return human_readable(size, sizstr, 1, -1024);
}

/* needle must be uppercase :) */
int strcasestr_len (char *haystack, char *needle, size_t len)
{
	char *p, *startn = 0, *np = 0, *end = haystack + len;

	for (p = haystack; p < end; p++) {
		if (np) {
			if (toupper(*p) == *np) {
				if (!*++np)
					return 1;
			} else
			np = 0;
		} else if (toupper(*p) == *needle) {
			np = needle + 1;
			startn = p;
		}
	}
	return 0;
}


static guint16
icon_of_fh (struct hl_filelist_hdr *fh)
{
	guint16 icon;

	if (!memcmp(&fh->ftype, "fldr", 4)) {
		if(	strcasestr_len(fh->fname, "DROP BOX", fh->fnlen) ||
			strcasestr_len(fh->fname, "UPLOAD", fh->fnlen)) {
			icon = ICON_FOLDER_IN;
		}
		else {
			icon = ICON_FOLDER;
		}
	}
	else if (!memcmp(&fh->ftype, "JPEG", 4)
		 || !memcmp(&fh->ftype, "PNGf", 4)
		 || !memcmp(&fh->ftype, "GIFf", 4)
		 || !memcmp(&fh->ftype, "PICT", 4))
		icon = ICON_FILE_IMAGE;
	else if (!memcmp(&fh->ftype, "MPEG", 4)
		 || !memcmp(&fh->ftype, "MPG ", 4)
		 || !memcmp(&fh->ftype, "AVI ", 4)
		 || !memcmp(&fh->ftype, "MooV", 4))
		icon = ICON_FILE_MOOV;
	else if (!memcmp(&fh->ftype, "MP3 ", 4))
		icon = ICON_FILE_NOTE;
	else if (!memcmp(&fh->ftype, "ZIP ", 4))
		icon = ICON_FILE_ZIP;
	else if (!memcmp(&fh->ftype, "SIT", 3))
		icon = ICON_FILE_SIT;
	else if (!memcmp(&fh->ftype, "APPL", 4))
		icon = ICON_FILE_APPL;
	else if (!memcmp(&fh->ftype, "rohd", 4))
		icon = ICON_FILE_DISK;
	else if (!memcmp(&fh->ftype, "HTft", 4))
		icon = ICON_FILE_HTft;
	else if (!memcmp(&fh->ftype, "alis", 4))
		icon = ICON_FILE_alis;
	else
		icon = ICON_FILE;

	return icon;
}

void output_file_list (struct cached_filelist *cfl, struct hl_filelist_hdr *fh,
					   void *data)
{
	GtkWidget *files_list;
	GdkPixmap *pixmap;
	GdkBitmap *mask;
	guint16 icon;
	gint row;
	gchar *nulls[2] = {0, 0};
	char humanbuf[LONGEST_HUMAN_READABLE+1], *sizstr;
	char namstr[255];
	struct gfile_list *gfl = (struct gfile_list *)data;
	struct path_hist *path = 0;

	files_list = gfl->hlist;
	gtk_window_set_title(GTK_WINDOW(gfl->window), cfl->path);

	if(strcmp(cfl->path, gfl->path_list->path)) {
		path = g_malloc(sizeof(struct path_hist)+strlen(cfl->path));
		strcpy(path->path, cfl->path);
		path->prev = 0;
		if(gfl->path_list) {
			path->prev = gfl->path_list;
		}
		gfl->path_list = path;
	}

	gtk_widget_set_sensitive(gfl->up_btn, gfl->path_list->prev!=NULL &&
		gtkhx_prefs.file_samewin);

	gtk_hlist_freeze(GTK_HLIST(files_list));
	gtk_hlist_clear(GTK_HLIST(files_list));

	for (fh = cfl->fh; (guint32)((char *)fh - (char *)cfl->fh) < cfl->fhlen;
		 fh += fh->len + SIZEOF_HL_DATA_HDR) {
		fh->fnlen = ntohl(fh->fnlen);
		fh->len = ntohs(fh->len);
		fh->fsize = ntohl(fh->fsize);
		
		row = gtk_hlist_append(GTK_HLIST(files_list), nulls);
		gtk_hlist_set_row_data(GTK_HLIST(files_list), row, fh);
		icon = icon_of_fh(fh);
		load_icon(files_list, icon, &icon_files, 1, &pixmap, &mask);
		
		if (fh->fnlen > 255)
			fh->fnlen = 255;
		memcpy(namstr, fh->fname, fh->fnlen);
		namstr[fh->fnlen] = 0;
		if (!memcmp(&fh->ftype, "fldr", 4)) {
			sizstr = humanbuf;
			g_snprintf(sizstr, LONGEST_HUMAN_READABLE+1, "(%u)", fh->fsize);
		}
		else {
			sizstr = human_size(humanbuf, fh->fsize);
		}
		/* Phase 5 dark-theme: no per-row foreground override — theme
		 * default applies, so file size + name read on both light
		 * and dark themes. */
		gtk_hlist_set_text(GTK_HLIST(files_list), row, 0, sizstr);
		
		if (!pixmap) {
			gtk_hlist_set_text(GTK_HLIST(files_list), row, 1, namstr);
		}
		else {
			gtk_hlist_set_pixtext(GTK_HLIST(files_list), row, 1, namstr, 34,
								  pixmap, mask);
		}
	}
	gtk_hlist_thaw(GTK_HLIST(files_list));
	gtk_hlist_select_row(GTK_HLIST(files_list), (gfl->row-1)?(gfl->row-1):gfl->row, 0);
	gtk_hlist_moveto(GTK_HLIST(files_list), (gfl->row-1)?(gfl->row-1):gfl->row, 0, .5, 0);
	

	gfl->in_use = 0;
}

void set_name_comment(GtkWidget *btn, gpointer data)
{
	GtkWidget *name_entry    = g_object_get_data (G_OBJECT (btn), "name");
	GtkWidget *comments_text = g_object_get_data (G_OBJECT (btn), "comments");
	char *path = g_object_get_data (G_OBJECT (btn), "path");
	const char *name;
	char *comments;
	char *file;
	GtkTextBuffer *cbuf;
	GtkTextIter cstart, cend;

	(void) data;

	/* gtk_editable_get_text returns a const string owned by the
	 * entry — don't free it. */
	name = gtk_editable_get_text (GTK_EDITABLE (name_entry));
	if (!name)
		name = "";

	/* The comments widget is a GtkTextView, not a GtkEditable —
	 * gtk_editable_get_chars on it would null-deref, which is what
	 * the previous code did and what crashed Save. Pull the text
	 * via the buffer API instead. gtk_text_buffer_get_text returns
	 * a fresh g_malloc'd copy that we own. */
	cbuf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (comments_text));
	gtk_text_buffer_get_start_iter (cbuf, &cstart);
	gtk_text_buffer_get_end_iter   (cbuf, &cend);
	comments = gtk_text_buffer_get_text (cbuf, &cstart, &cend, FALSE);
	if (!comments)
		comments = g_strdup ("");

	file = dirchar_basename (path);
	task_new (&the_session.htlc, 0, 0, 0, "set file info");
	if (file != path) {
		guint16 hldirlen = 0;
		guint8 *hldir = path_to_hldir (path, &hldirlen, 1);
		hlwrite (&the_session.htlc, HTLC_HDR_FILE_SETINFO, 0, 4,
		         HTLC_DATA_FILE_NAME,    strlen (file),     file,
		         HTLC_DATA_FILE_RENAME,  strlen (name),     name,
		         HTLC_DATA_FILE_COMMENT, strlen (comments), comments,
		         HTLC_DATA_DIR,          hldirlen,          hldir);
		g_free (hldir);
	} else {
		hlwrite (&the_session.htlc, HTLC_HDR_FILE_SETINFO, 0, 3,
		         HTLC_DATA_FILE_NAME,    strlen (file),     file,
		         HTLC_DATA_FILE_RENAME,  strlen (name),     name,
		         HTLC_DATA_FILE_COMMENT, strlen (comments), comments);
	}

	g_free (comments);
}

void close_file_info(GtkWidget *win, char *path)
{
	g_free(path);
}

/* Single read-only metadata row in the File Info dialog. Title is
 * the field name (e.g. "Size"); subtitle is the value, rendered as
 * an em-dash when the value is missing or empty so the row stays
 * informative either way. The subtitle is selectable for copy. */
static GtkWidget *
hx_file_info_row (const char *title, const char *value)
{
	GtkWidget *row = adw_action_row_new ();
	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
	adw_action_row_set_subtitle (ADW_ACTION_ROW (row),
	                             (value && value[0]) ? value : "—");
	adw_action_row_set_subtitle_selectable (ADW_ACTION_ROW (row), TRUE);
	return row;
}

void output_file_info (char *path, char *name, char *creator, char *type,
                       char *comments, char *modified, char *created,
                       guint32 size)
{
	GtkWidget *window, *header, *savebtn;
	GtkWidget *vbox;
	GtkWidget *name_group, *name_entry;
	GtkWidget *info_group;
	GtkWidget *comments_group, *comments_scroll, *comments_text;
	GtkTextBuffer *cbuf;
	char humanbuf[LONGEST_HUMAN_READABLE + 1];
	char sizestr[64];

	window = gtk_window_new ();
	gtk_window_set_title (GTK_WINDOW (window), _("File Info"));
	gtk_window_set_default_size (GTK_WINDOW (window), 460, 540);

	/* AdwHeaderBar with Save action on the trailing edge. */
	header = adw_header_bar_new ();
	savebtn = gtk_button_new_with_label (_("Save"));
	gtk_widget_add_css_class (savebtn, "suggested-action");
	adw_header_bar_pack_end (ADW_HEADER_BAR (header), savebtn);
	gtk_window_set_titlebar (GTK_WINDOW (window), header);

	vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 18);
	gtk_widget_set_margin_start  (vbox, 12);
	gtk_widget_set_margin_end    (vbox, 12);
	gtk_widget_set_margin_top    (vbox, 12);
	gtk_widget_set_margin_bottom (vbox, 12);

	/* Name (editable). AdwEntryRow keeps the label inline with the
	 * field and gives us a wide entry that doesn't truncate the
	 * file name visually. */
	name_group = adw_preferences_group_new ();
	name_entry = adw_entry_row_new ();
	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (name_entry),
	                               _("Name"));
	gtk_editable_set_text (GTK_EDITABLE (name_entry), name ? name : "");
	adw_preferences_group_add (ADW_PREFERENCES_GROUP (name_group), name_entry);
	gtk_box_append (GTK_BOX (vbox), name_group);

	/* Read-only metadata. */
	if (size > 0) {
		char *human = human_size (humanbuf, size);
		if (size >= 1024)
			g_snprintf (sizestr, sizeof sizestr, "%s (%u %s)",
			            human, size, _("bytes"));
		else
			g_snprintf (sizestr, sizeof sizestr, "%u %s",
			            size, _("bytes"));
	} else {
		sizestr[0] = '\0';
	}

	info_group = adw_preferences_group_new ();
	adw_preferences_group_add (ADW_PREFERENCES_GROUP (info_group),
	                           hx_file_info_row (_("Creator"),  creator));
	adw_preferences_group_add (ADW_PREFERENCES_GROUP (info_group),
	                           hx_file_info_row (_("Type"),     type));
	adw_preferences_group_add (ADW_PREFERENCES_GROUP (info_group),
	                           hx_file_info_row (_("Size"),     sizestr));
	adw_preferences_group_add (ADW_PREFERENCES_GROUP (info_group),
	                           hx_file_info_row (_("Created"),  created));
	adw_preferences_group_add (ADW_PREFERENCES_GROUP (info_group),
	                           hx_file_info_row (_("Modified"), modified));
	gtk_box_append (GTK_BOX (vbox), info_group);

	/* Comments. AdwPreferencesGroup gives us a titled section; the
	 * scrolled text view is added directly to the group. */
	comments_group = adw_preferences_group_new ();
	adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (comments_group),
	                                 _("Comments"));
	comments_text = gtk_text_view_new ();
	gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (comments_text),
	                             GTK_WRAP_WORD_CHAR);
	gtk_text_view_set_editable (GTK_TEXT_VIEW (comments_text), TRUE);
	cbuf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (comments_text));
	gtk_text_buffer_set_text (cbuf, comments ? comments : "",
	                          comments ? strlen (comments) : 0);

	comments_scroll = gtk_scrolled_window_new ();
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (comments_scroll),
	                                GTK_POLICY_AUTOMATIC,
	                                GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_has_frame (GTK_SCROLLED_WINDOW (comments_scroll),
	                                   TRUE);
	gtk_widget_set_size_request (comments_scroll, -1, 140);
	gtkhx_widget_set_child (comments_scroll, comments_text);
	adw_preferences_group_add (ADW_PREFERENCES_GROUP (comments_group),
	                           comments_scroll);
	gtk_widget_set_vexpand (comments_group, TRUE);
	gtk_box_append (GTK_BOX (vbox), comments_group);

	gtkhx_widget_set_child (window, vbox);

	g_object_set_data (G_OBJECT (savebtn), "name",     name_entry);
	g_object_set_data (G_OBJECT (savebtn), "comments", comments_text);
	g_object_set_data (G_OBJECT (savebtn), "path",     path);
	g_signal_connect (savebtn, "clicked",
	                  G_CALLBACK (set_name_comment), NULL);

	g_signal_connect (window, "destroy",
	                  G_CALLBACK (close_file_info), path);

	gtk_window_present (GTK_WINDOW (window));
	init_keyaccel (window);
}

struct cached_filelist *cfl_lookup (const char *path)
{
	struct gfile_list *gfl;

	gfl = gfl_with_path((char *)path);
	if(!gfl) {
		struct cached_filelist *cfl = g_malloc0(sizeof(struct cached_filelist));
		return cfl;
	}
	else {
		return gfl->cfl;
	}
}

void cfl_print (struct cached_filelist *cfl, void *data)
{
	struct hl_filelist_hdr *fh = cfl->fh;

	if(data) {
		hx_output.file_list(cfl, fh, data);
	}
}

struct x_fhdr {
	guint16 enc PACKED;
	guint8 len, name[1];
};

guint8 *path_to_hldir (const char *path, guint16 *hldirlen, int is_file)
{
	guint8 *hldir;
	struct x_fhdr *fh;
	char const *p, *p2;
	guint16 pos = 2, dc = 0;
	guint8 nlen;

	hldir = g_malloc(2);
	p = path;
	while ((p2 = strchr(p, dir_char))) {
		if (!(p2 - p)) {
			p++;
			continue;
		}
		nlen = (guint8)(p2 - p);
		pos += 3 + nlen;
		hldir = g_realloc(hldir, pos);
		fh = (struct x_fhdr *)(&(hldir[pos - (3 + nlen)]));
		memset(&fh->enc, 0, 2);
		fh->len = nlen;
		memcpy(fh->name, p, nlen);
		dc++;
		p = p2 + 1;
	}
	if (!is_file && *p) {
		nlen = (guint8)strlen(p);
		pos += 3 + nlen;
		hldir = g_realloc(hldir, pos);
		fh = (struct x_fhdr *)(&(hldir[pos - (3 + nlen)]));
		memset(&fh->enc, 0, 2);
		fh->len = nlen;
		memcpy(fh->name, p, nlen);
		dc++;
	}
	*((guint16 *)hldir) = htons(dc);

	*hldirlen = pos;
	return hldir;
}

/* Phase 5: dirchar_basename is now a thin wrapper around the
 * dir_char-free path_basename(path, sep) so the unit tests can
 * exercise the underlying logic without linking files.c. The shape
 * stays identical for callers; dir_char is still the global the
 * Hotline-server-driven dirchar_change() rewrites. */
char *dirchar_basename (char *path)
{
	return path_basename (path, (char) dir_char);
}

void dirchar_fix (char *lpath)
{
	char *p;

	for (p = lpath; *p; p++)
		if (*p == '/')
			*p = (dir_char == '/' ? ':' : dir_char);
}

void dirmask (char *dst, char *src, char *mask)
{
	while (*mask && *src && *mask++ == *src++) ;

	strcpy(dst, src);
}

int exists_remote (char *path)
{
	struct gfile_list *gfl;
	struct cached_filelist *cfl = NULL;
	struct hl_filelist_hdr *fh;
	char *p, *ent, buf[MAXPATHLEN];
	int blen = 0, len;

	len = strlen(path);
	if (*path != dir_char) {
		snprintf(buf, MAXPATHLEN, "%s%c%.*s", "/",
				 dir_char, len, path);
		len = strlen(buf); /* Unfortunately we can't trust snprintf
							  return value .. */
		path = buf;
	}
	ent = path;
	for (p = path + len - 1; p >= path; p--)
		if (*p == dir_char) {
			ent = p+1;
			while (p > path && *p == dir_char)
				p--;
			blen = (p+1) - path;
			len -= ent - path;
			break;
		}
	if (!*ent)
		return -1;


	for(gfl = gfile_list; gfl; gfl = gfl->prev) {
		if(!strncmp(gfl->cfl->path, path, blen)) {
			cfl = gfl->cfl;
			break;
		}
	}

	if (!cfl) {
		guint16 hldirlen;
		guint8 *hldir;

		snprintf(buf, MAXPATHLEN, "%.*s", blen, path);
		path = buf; 
		len = strlen(path);
		while (len > 1 && path[--len] == dir_char)
			path[len] = 0;
		cfl = g_malloc0(sizeof(struct cached_filelist));
		cfl->completing = COMPLETE_EXPAND;
		cfl->path = g_strdup(path);
		hldir = path_to_hldir(path, &hldirlen, 0);
		task_new(&the_session.htlc, RCV_TASK_FN(rcv_task_file_list), cfl, 0, "ls_exists");
		hlwrite(&the_session.htlc, HTLC_HDR_FILE_LIST, 0, 1,
			HTLC_DATA_DIR, hldirlen, hldir);
		g_free(hldir);
		return 0;
	}
	if (!cfl->fh)
		return 0;

	for (fh = cfl->fh; (guint32)((char *)fh - (char *)cfl->fh) < cfl->fhlen;
		 fh += fh->len + SIZEOF_HL_DATA_HDR) {
		if ((int)fh->fnlen == len && !strncmp(fh->fname, ent, len))
			return 1;
	}

	return 0;
}

void hx_list_dir (struct htlc_conn *htlc, const char *path, int reload,
				  int recurs, void *data)
{
	guint16 hldirlen;
	guint8 *hldir;
	struct cached_filelist *cfl;
	struct gfile_list *gfl = data;

	if(gfl->cfl && (strcmp(gfl->cfl->path, path) && !gtkhx_prefs.file_samewin)) {
		gfl = create_files_window((char *)path);
	}

	if(gfl->cfl) {
		g_free(gfl->cfl);
	}

	gfl->cfl = g_malloc0(sizeof(struct cached_filelist));
	cfl = gfl->cfl;
	cfl->path = g_strdup(path);
	gfl->in_use = 1;
	if(recurs)
		cfl->completing = COMPLETE_LS_R;

	hldir = path_to_hldir(path, &hldirlen, 0);
	task_new(htlc, RCV_TASK_FN(rcv_task_file_list), cfl, (void *)gfl, "ls");
	hlwrite(htlc, HTLC_HDR_FILE_LIST, 0, 1,
		HTLC_DATA_DIR, hldirlen, hldir);
	g_free(hldir);
}

void hx_make_dir(struct htlc_conn *htlc, char *path)
{
	guint16 hldirlen;
	guint8 *hldir;


	hldir = path_to_hldir(path, &hldirlen, 0);
	task_new(htlc, 0, 0, 0, "mkdir");
	hlwrite(htlc, HTLC_HDR_FILE_MKDIR, 0, 1, HTLC_DATA_DIR, hldirlen, hldir);


	g_free(hldir);
}

void hx_file_delete (struct htlc_conn *htlc, char *path)
{
	guint16 hldirlen;
	guint8 *hldir;
	char *file;

	task_new(htlc, 0, 0, 0, "rm");
	file = dirchar_basename(path); 
	if (file != path) {
		hldir = path_to_hldir(path, &hldirlen, 1);
		hlwrite(htlc, HTLC_HDR_FILE_DELETE, 0, 2,
			HTLC_DATA_FILE_NAME, strlen(file), file,
			HTLC_DATA_DIR, hldirlen, hldir);
		g_free(hldir);
	} else {
		hlwrite(htlc, HTLC_HDR_FILE_DELETE, 0, 1,
			HTLC_DATA_FILE_NAME, strlen(file), file);
	}
}
void hx_file_info(struct htlc_conn *htlc, char *_path)
{
	char *file;
	guint8 *hldir;
	guint16 hldirlen;
	char *path = g_strdup(_path);

	task_new(htlc, RCV_TASK_FN(rcv_task_file_getinfo), path, 0, "finfo");
	file = dirchar_basename(path); 

	if(file != path) {
		hldir = path_to_hldir(path, &hldirlen, 1);
		hlwrite(htlc, HTLC_HDR_FILE_GETINFO, 0, 2,
				HTLC_DATA_FILE_NAME, strlen(file), file,
				HTLC_DATA_DIR, hldirlen, hldir);
		g_free(hldir);
	}
	else {
		hlwrite(htlc, HTLC_HDR_FILE_GETINFO, 0, 1, HTLC_DATA_FILE_NAME,
				strlen(file), file);
	}
}

void hx_put_file(struct htlc_conn *htlc, char *lpath, char *rpath)
{
	struct htxf_conn *htxf;

	/* Uploads don't use srv_data_size — that's a download-side
	 * heuristic for resume vs rename. */
	htxf = xfer_new(lpath, rpath, XFER_PUT, 0, 0);
	htxf->filter_argv = 0;
	htxf->opt.retry = 0;
}

void hx_file_link (struct htlc_conn *htlc, char *src_path, char *dst_path)
{
	char *src_file, *dst_file;
	guint16 hldirlen, rnhldirlen;
	guint8 *hldir, *rnhldir;

	src_file = dirchar_basename(src_path);
	dst_file = dirchar_basename(dst_path);
	hldir = path_to_hldir(src_path, &hldirlen, 1);
	rnhldir = path_to_hldir(dst_path, &rnhldirlen, 1);
	task_new(htlc, 0, 0, 0, "ln");
	hlwrite(htlc, HTLC_HDR_FILE_SYMLINK, 0, 4,
		HTLC_DATA_FILE_NAME, strlen(src_file), src_file,
		HTLC_DATA_DIR, hldirlen, hldir,
		HTLC_DATA_DIR_RENAME, rnhldirlen, rnhldir,
		HTLC_DATA_FILE_RENAME, strlen(dst_file), dst_file);
	g_free(rnhldir);
	g_free(hldir);
}

void hx_file_move (struct htlc_conn *htlc, char *src_path, char *dst_path)
{
	char *dst_file, *src_file;
	guint16 hldirlen, rnhldirlen;
	guint8 *hldir, *rnhldir;
	size_t len;

	dst_file = dirchar_basename(dst_path);
	src_file = dirchar_basename(src_path);

	hldir = path_to_hldir(src_path, &hldirlen, 1);
	len = strlen(dst_path) - (strlen(dst_path) - (dst_file - dst_path));
	if (len && (len != strlen(src_path) - (strlen(src_path) - (src_file -
															   src_path)) ||
				memcmp(dst_path, src_path, len))) {
		rnhldir = path_to_hldir(dst_path, &rnhldirlen, 1);
		task_new(htlc, 0, 0, 0, "mv");
		hlwrite(htlc, HTLC_HDR_FILE_MOVE, 0, 3,
			HTLC_DATA_FILE_NAME, strlen(src_file), src_file,
			HTLC_DATA_DIR, hldirlen, hldir,
			HTLC_DATA_DIR_RENAME, rnhldirlen, rnhldir);
		g_free(rnhldir);
	}
	if (*dst_file && strcmp(src_file, dst_file)) {
		task_new(htlc, 0, 0, 0, "mv");
		hlwrite(htlc, HTLC_HDR_FILE_SETINFO, 0, 3,
			HTLC_DATA_FILE_NAME, strlen(src_file), src_file,
			HTLC_DATA_FILE_RENAME, strlen(dst_file), dst_file,
		HTLC_DATA_DIR, hldirlen, hldir);
	}
	g_free(hldir);
}

