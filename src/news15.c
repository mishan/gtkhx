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
#include <gdk/gdk.h>
#include <sys/time.h>
#include <time.h>
#include <netinet/in.h>

#include "hx.h"
#include "network.h"
#include "gtkutil.h"
#include "gtkhx.h"
#include "gtk_hlist.h"
#include "tasks.h"
#include "rcv.h"
#include "files.h"
#include "news15.h"

/* Phase 5: gtkhx_text_to_utf8 lives in gtkutil.h now. */

/* Phase 4.13: this file uses GtkTreeView + GtkTreeStore directly for
 * the news thread tree (Phase 2.8 work) and GtkDialog for the
 * mkdir / mkcat prompts. Both API families are deprecated in
 * GTK 4.10 in favor of GtkColumnView/GListModel and GtkAlertDialog/
 * GtkWindow respectively. The replacement migrations are tracked
 * as Phase 5 (tree-view) and Phase 4.7 (dialogs) follow-ups; until
 * then suppress deprecations across the file so the rest of the
 * tree can keep -Werror=deprecated-declarations on. */
G_GNUC_BEGIN_IGNORE_DEPRECATIONS

struct gnews_folder *gfnews_list = NULL;
struct gnews_folder *gfnews_with_hlist (GtkWidget *hlist);
struct gnews_catalog *gcnews_list = NULL;
struct gnews_catalog *create_gcnews_window (char *path);

/* Phase 2.8: GtkTreeStore column layout for the news thread tree.
 * Column 0 is the subject text shown in the visible column; column 1
 * carries the underlying news_item pointer so the selection callbacks
 * can recover it without keeping a separate row-data table. */
enum {
	NEWS_COL_SUBJECT = 0,
	NEWS_COL_ITEM,
	NEWS_N_COLS
};

/* Return the news_item * for the currently selected row, or NULL. */
static struct news_item *gcnews_selected_item(struct gnews_catalog *gcnews)
{
	GtkTreeSelection *sel;
	GtkTreeModel *model;
	GtkTreeIter iter;
	struct news_item *item = NULL;

	if (!gcnews || !gcnews->news_tree)
		return NULL;

	sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(gcnews->news_tree));
	if (!gtk_tree_selection_get_selected(sel, &model, &iter))
		return NULL;

	gtk_tree_model_get(model, &iter, NEWS_COL_ITEM, &item, -1);
	return item;
}
struct gnews_folder *create_gfnews_window(char *path);

void hx_news15_get_post(struct htlc_conn *htlc, struct news_item *item)
{
	guint8 *hldir;
	guint16 hldirlen;
	guint32 postid;

	hldir = path_to_hldir(item->group->path, &hldirlen, 0);
	task_new(htlc, RCV_TASK_FN(rcv_task_news_post), item, 0, "news_post");

	postid = htonl(item->postid);
	hlwrite(htlc, HTLC_HDR_GETTHREAD, 0, 3,
			HTLC_DATA_NEWSPATH, hldirlen, hldir,
			HTLC_DATA_THREADID, 4, &postid,
			HTLC_DATA_NEWSTYPE, strlen(item->parts[0].mime_type),
			item->parts[0].mime_type);
	g_free(hldir);
}

void hx_news15_cat_list(struct htlc_conn *htlc, struct gnews_catalog *gcnews)
{
	guint8 *hldir;
	guint16 hldirlen;

	gcnews->listing = 1;
	hldir = path_to_hldir(gcnews->path, &hldirlen, 0);
	task_new(htlc, RCV_TASK_FN(rcv_task_newscat_list), gcnews, 0, "news_category");
	hlwrite(htlc, HTLC_HDR_NEWSCATLIST, 0, 1, 
			HTLC_DATA_NEWSPATH, hldirlen, hldir);
	g_free(hldir);
}

void hx_news15_fldr_list(struct htlc_conn *htlc, struct gnews_folder *gfnews)
{
	guint8 *hldir;
	guint16 hldirlen;

	gfnews->listing = 1;
	hldir = path_to_hldir(gfnews->path, &hldirlen, 0);
	task_new(htlc, RCV_TASK_FN(rcv_task_newsfolder_list), gfnews, 0, "news_folder");

	hlwrite(htlc, HTLC_HDR_NEWSDIRLIST, 0, 1, 
			HTLC_DATA_NEWSPATH, hldirlen, hldir);
	g_free(hldir);
}

static void hx_news15_post_thread(struct htlc_conn *htlc, char *path, const char *subject,
						   guint32 threadid, char *text)
{
	guint8 *hldir;
	guint16 hldirlen;
	guint32 parent = 0;

	hldir = path_to_hldir(path, &hldirlen, 0);
	task_new(htlc, 0, 0, 0, "news15_post");
	threadid = htonl(threadid);
	hlwrite(htlc, HTLC_HDR_POSTTHREAD, 0, 6,
			HTLC_DATA_NEWSPATH, hldirlen, hldir, 
			HTLC_DATA_PARENTTHREAD, 4, &parent,
			HTLC_DATA_NEWSTYPE, 11, "text/plain", 
			HTLC_DATA_NEWSSUBJECT, strlen(subject), subject,
			HTLC_DATA_NEWSDATA, strlen(text), text,
			HTLC_DATA_THREADID, 4, &threadid);
	g_free(hldir);
}

static void hx_news15_delete_thread(struct htlc_conn *htlc, char *path,
							 guint32 threadid)
{
	guint8 *hldir;
	guint16 hldirlen;
	
	hldir = path_to_hldir(path, &hldirlen, 0);
	task_new(htlc, 0, 0, 0, "news15_rm_thread");
	threadid = htonl(threadid);
	hlwrite(htlc, HTLC_HDR_DELETETHREAD, 0, 2,
			HTLC_DATA_NEWSPATH, hldirlen, hldir,
			HTLC_DATA_THREADID, 4, &threadid);
	g_free(hldir);
}



static void hx_news15_delete(struct htlc_conn *htlc, char *path)
{
	guint8 *hldir;
	guint16 hldirlen;
	
	hldir = path_to_hldir(path, &hldirlen, 0);
	task_new(htlc, 0, 0, 0, "news15_rm");
	hlwrite(htlc, HTLC_HDR_DELNEWSDIRCAT, 0, 1,
			HTLC_DATA_NEWSPATH, hldirlen, hldir);
	g_free(hldir);
}

static void hx_news15_mkcat(struct htlc_conn *htlc, char *path, const char *name)
{
	guint8 *hldir;
	guint16 hldirlen;

	hldir = path_to_hldir(path, &hldirlen, 0);
	task_new(htlc, 0, 0, 0, "news15_mkcat");
	hlwrite(htlc, HTLC_HDR_MAKECATEGORY, 0, 2,
			HTLC_DATA_NEWSPATH, hldirlen, hldir,
			HTLC_DATA_CATEGORY, strlen(name), name);
	g_free(hldir);
}

static void hx_news15_mkdir(struct htlc_conn *htlc, char *path)
{
	guint8 *hldir;
	guint16 hldirlen;
	
	hldir = path_to_hldir(path, &hldirlen, 0);
	task_new(htlc, 0, 0, 0, "news15_mkdir");
	hlwrite(htlc, HTLC_HDR_MAKENEWSDIR, 0, 1,
			HTLC_DATA_NEWSPATH, hldirlen, hldir);
	g_free(hldir);
}

/* Phase 4.5: button-press-event is gone in GTK 4. Folder-list
 * single/double-click handling lives on a GtkGestureClick controller
 * now. n_press == 2 gates the descend-into-folder action. */
static void newsf_pressed (GtkGestureClick *gesture, int n_press,
                           double x, double y, gpointer data)
{
	GtkWidget *widget = gtk_event_controller_get_widget (
		GTK_EVENT_CONTROLLER (gesture));
	struct gnews_folder *gfnews;
	int row, col;
	(void) data;

	gfnews = gfnews_with_hlist(widget);
	if(!gfnews)
		return;
	gtk_hlist_get_selection_info(GTK_HLIST(widget),
								 (int) x, (int) y, &row, &col);

	if(gfnews->listing)
		return;
	if(n_press == 2) {
		struct folder_item *item = gtk_hlist_get_row_data(GTK_HLIST(widget), 
														  gfnews->row);
		if(item) {
			char path[4096];

			
			if(strcmp(gfnews->news->path, "/")) {
				g_snprintf(path, sizeof(path), "%s/%s", gfnews->news->path, item->name);
			}
			else {
				g_snprintf(path, sizeof(path), "/%s", item->name);
			}
			if(item->type == 1) {
				if(gtkhx_prefs.news_samewin) {
					g_free(gfnews->path);
					gfnews->path = g_strdup(path);
					gtk_window_set_title(GTK_WINDOW(gfnews->window), 
										 gfnews->path);
					hx_news15_fldr_list(&the_session.htlc, gfnews);
				}
				else {
					struct gnews_folder *new_gfnews = NULL;

					new_gfnews = create_gfnews_window(path);
					hx_news15_fldr_list(&the_session.htlc, new_gfnews);
				}
			}
			else {
				struct gnews_catalog *gcnews;

				gcnews = create_gcnews_window(path);				
				hx_news15_cat_list(&the_session.htlc, gcnews);
			}
		}
	}
	else {
		gfnews->row = row;
		gfnews->col = col;
	}
}

struct gnews_folder *gfnews_with_hlist (GtkWidget *hlist)
{
	struct gnews_folder *gfnews;

	for(gfnews = gfnews_list; gfnews; gfnews = gfnews->prev) {
		if(gfnews->news_list == hlist) {
			return gfnews;
		}
	}
	return 0;
}


static void delete_gfnews(struct gnews_folder *gfnews)
{
	int i;
	
	if (gfnews->next)
		gfnews->next->prev = gfnews->prev;
	if (gfnews->prev)
		gfnews->prev->next = gfnews->next;
	if (gfnews == gfnews_list)
		gfnews_list = gfnews->prev;

	for(i = 0; i < gfnews->news->num_entries; i++) {
		g_free(gfnews->news->entry[i]);
	}
	
	g_free(gfnews->news);
	g_free(gfnews->path);
	g_free(gfnews);
}

/* Phase 4.5: GTK 4 close-request returns FALSE so default destroy
 * proceeds; the framework destroys the widget. */
static gboolean destroy_gfnews_browser(GtkWindow *window, gpointer data)
{
	struct gnews_folder *gfnews = g_object_get_data(G_OBJECT(window),
														  "gfnews");
	(void) data;

	delete_gfnews(gfnews);
	return FALSE;
}

static void gfnews_reload_btn(GtkWidget *btn, struct gnews_folder *gfnews)
{
	if(gfnews->listing)
		return;
	gtk_hlist_clear(GTK_HLIST(gfnews->news_list));
	hx_news15_fldr_list(&the_session.htlc, gfnews);
}

/* Phase 5: AdwAlertDialog with a GtkEntry as extra-child replaces the
 * hand-rolled GtkDialog used by the New News Folder / New News
 * Category prompts. The two flows differ only in their post-create
 * action, so they share the same dialog skeleton: a small struct
 * carries the gfnews context plus a tag that on_response dispatches
 * on. The struct is freed via the dialog's "closed" signal. */

enum gfnews_create_kind { GFNEWS_CREATE_DIR, GFNEWS_CREATE_CAT };

struct gfnews_create_ctx {
	struct gnews_folder *gfnews;
	enum gfnews_create_kind kind;
};

static void
gfnews_create_response (AdwAlertDialog *dialog, const char *response, gpointer data)
{
	struct gfnews_create_ctx *ctx = data;
	GtkEditable *entry;
	const char *name;

	if (g_strcmp0 (response, "create") != 0)
		return;

	entry = GTK_EDITABLE (adw_alert_dialog_get_extra_child (dialog));
	if (!entry)
		return;
	name = gtk_editable_get_text (entry);

	if (ctx->kind == GFNEWS_CREATE_DIR) {
		char pathname[MAXPATHLEN];
		snprintf (pathname, MAXPATHLEN, "%s/%s",
		          ctx->gfnews->path, name);
		hx_news15_mkdir (&the_session.htlc, pathname);
	} else {
		hx_news15_mkcat (&the_session.htlc, ctx->gfnews->path, name);
	}
	hx_news15_fldr_list (&the_session.htlc, ctx->gfnews);
}

static void
gfnews_create_closed (AdwDialog *dialog, gpointer data)
{
	(void) dialog;
	g_free (data);
}

static void
gfnews_create_entry_activate (GtkEntry *entry, gpointer data)
{
	(void) entry;
	g_signal_emit_by_name (data, "response", "create");
}

static void
gfnews_create_dialog (GtkWidget *btn, struct gnews_folder *gfnews,
                      enum gfnews_create_kind kind)
{
	AdwDialog *dialog;
	GtkWidget *entry;
	struct gfnews_create_ctx *ctx;
	const char *title, *body, *create_label;

	if (kind == GFNEWS_CREATE_DIR) {
		title = _("New News Folder");
		body  = _("Enter a name for the new news folder.");
		create_label = _("C_reate Folder");
	} else {
		title = _("New News Category");
		body  = _("Enter a name for the new news category.");
		create_label = _("C_reate Category");
	}

	dialog = adw_alert_dialog_new (title, body);
	adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog),
	                               "cancel", _("_Cancel"));
	adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog),
	                               "create", create_label);
	adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog),
	                                          "create",
	                                          ADW_RESPONSE_SUGGESTED);
	adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog),
	                                       "create");
	adw_alert_dialog_set_close_response   (ADW_ALERT_DIALOG (dialog),
	                                       "cancel");

	entry = gtk_entry_new ();
	gtk_entry_set_activates_default (GTK_ENTRY (entry), TRUE);
	g_signal_connect (entry, "activate",
	                  G_CALLBACK (gfnews_create_entry_activate), dialog);
	adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dialog), entry);

	ctx = g_new (struct gfnews_create_ctx, 1);
	ctx->gfnews = gfnews;
	ctx->kind   = kind;
	g_signal_connect (dialog, "response",
	                  G_CALLBACK (gfnews_create_response), ctx);
	g_signal_connect (dialog, "closed",
	                  G_CALLBACK (gfnews_create_closed), ctx);

	adw_dialog_present (dialog, btn);
}

static void gfnews_mkdir_btn(GtkWidget *btn, struct gnews_folder *gfnews)
{
	gfnews_create_dialog (btn, gfnews, GFNEWS_CREATE_DIR);
}

static void gfnews_mkcat_btn(GtkWidget *btn, struct gnews_folder *gfnews)
{
	gfnews_create_dialog (btn, gfnews, GFNEWS_CREATE_CAT);
}

static void gfnews_delete_btn(GtkWidget *btn, struct gnews_folder *gfnews)
{
	struct folder_item *item = gtk_hlist_get_row_data(
		GTK_HLIST(gfnews->news_list), 
		gfnews->row);
	
	if(item) {
		char path[4096];
		
		if(strcmp(gfnews->news->path, "/")) {
			g_snprintf(path, sizeof(path), "%s/%s", gfnews->news->path, item->name);
		}
		else {
			g_snprintf(path, sizeof(path), "/%s", item->name);
		}
		hx_news15_delete(&the_session.htlc, path);
		hx_news15_fldr_list(&the_session.htlc, gfnews);
	}
}


static void gfnews_up_btn(GtkWidget *btn, struct gnews_folder *gfnews)
{
	struct path_hist *path = NULL;

	if(!gtkhx_prefs.news_samewin)
		return;

	if(gfnews->listing)
		return;

	if(gfnews->path_list->prev) {
		path = gfnews->path_list;
		gfnews->path_list = gfnews->path_list->prev;
		g_free(path);
	}
	else {
		return;
	}

	g_free(gfnews->path);
	gfnews->path = g_strdup(gfnews->path_list->path);
	gfnews->listing = 1;
	hx_news15_fldr_list(&the_session.htlc, gfnews);
}

/* Phase 4.8: news15 had a drag-and-drop scaffold (drag_data_get +
 * drag_data_received with a GTK_TARGET_SAME_APP "hlist" target) that
 * never actually moved anything — drag_send was a no-op send and
 * drag_receive returned at its first statement. The XXX comment block
 * inside it was the never-implemented hx_news15_move dispatch.
 *
 * GtkTargetEntry, GdkDragContext, GtkSelectionData, GTK_TARGET_SAME_APP,
 * gtk_drag_source_set / gtk_drag_dest_set are all gone in GTK 4 (the
 * replacement is GtkDragSource / GtkDropTarget controllers). Rather
 * than port the dead code, drop it. If multi-folder rearrangement
 * comes back as a feature, it's a clean GtkDragSource + GtkDropTarget
 * implementation under GTK 4. */


struct gnews_folder *create_gfnews_window(char *path)
{
	struct gnews_folder *gfnews = g_malloc(sizeof(struct gnews_folder));
	GtkWidget *news_window;
	GtkWidget *news_list;
	GtkWidget *news_scroll;
	GtkWidget *parentbtn;
	GtkWidget *reloadbtn;
	GtkWidget *deletebtn;
	GtkWidget *mkdirbtn;
	GtkWidget *mkcatbtn;

	gfnews->listing = 0;
	gfnews->prev = 0;
	gfnews->next = 0;

	if(path) {
		gfnews->path = g_strdup(path);
	}
	else {
		gfnews->path = g_strdup("/");
	}

	if(gfnews_list) {
		gfnews_list->next = gfnews;
		gfnews->prev = gfnews_list;
	}

	gfnews->path_list = g_malloc(sizeof(struct path_hist)+
								 strlen(gfnews->path));
	strcpy(gfnews->path_list->path, gfnews->path);
	gfnews->path_list->prev = NULL;

	news_window = gtk_window_new();
	gtk_window_set_resizable(GTK_WINDOW(news_window), TRUE);

	/* Phase 3.x: dropped GTK 1.2-era realize+get_style pair (style unused). */
	gtk_widget_set_size_request(news_window, 264, 400);
	gtk_window_set_title(GTK_WINDOW(news_window), gfnews->path);
	g_object_set_data(G_OBJECT(news_window), "gfnews", gfnews);
	g_signal_connect(news_window, "close-request",
					   G_CALLBACK(destroy_gfnews_browser), 0);

	news_scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(news_scroll), 
								   GTK_POLICY_AUTOMATIC, GTK_POLICY_ALWAYS);

	news_list = gtk_hlist_new(1);
	gtk_hlist_set_column_width(GTK_HLIST(news_list), 0, 64);
/*	gtk_hlist_set_column_width(GTK_HLIST(news_list), 1, 240); */
	gtk_hlist_set_row_height(GTK_HLIST(news_list), 18);
	gtk_hlist_set_shadow_type(GTK_HLIST(news_list), GTK_SHADOW_NONE);
	gtk_hlist_set_column_justification(GTK_HLIST(news_list), 0, 
									   GTK_JUSTIFY_LEFT);
	{
		/* Phase 4.5: button-press-event is gone — the gesture controller
		 * dispatches single-click row tracking and double-click
		 * descent through newsf_pressed. */
		GtkGesture *click = gtk_gesture_click_new ();
		gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click),
		                               GDK_BUTTON_PRIMARY);
		g_signal_connect (click, "pressed",
		                  G_CALLBACK (newsf_pressed), NULL);
		gtk_widget_add_controller (news_list,
		                           GTK_EVENT_CONTROLLER (click));
	}
	/* Phase 4.8: dead drag-and-drop scaffold removed. See note above
	 * the news15_drag* removal. */

	/* Phase 5: 2x-scaled headerbar buttons via gtkhx_pixmap_button. */
	parentbtn = gtkhx_pixmap_button (
		"/com/nasledov/gtkhx/pixmaps/up.xpm",
		_("Parent Directory"), 2,
		G_CALLBACK (gfnews_up_btn), gfnews);
	gtk_widget_set_sensitive (parentbtn, gtkhx_prefs.news_samewin);
	gfnews->up_btn = parentbtn;

	reloadbtn = gtkhx_pixmap_button (
		"/com/nasledov/gtkhx/pixmaps/refresh.xpm",
		_("Reload"), 2,
		G_CALLBACK (gfnews_reload_btn), gfnews);

	deletebtn = gtkhx_pixmap_button (
		"/com/nasledov/gtkhx/pixmaps/trash.xpm",
		_("Delete"), 2,
		G_CALLBACK (gfnews_delete_btn), gfnews);

	mkdirbtn = gtkhx_pixmap_button (
		"/com/nasledov/gtkhx/pixmaps/newsfld.xpm",
		_("New Folder"), 2,
		G_CALLBACK (gfnews_mkdir_btn), gfnews);

	mkcatbtn = gtkhx_pixmap_button (
		"/com/nasledov/gtkhx/pixmaps/newscat.xpm",
		_("New Category"), 2,
		G_CALLBACK (gfnews_mkcat_btn), gfnews);

	/* Phase 5: action buttons live in the AdwHeaderBar. Navigation
	 * (Parent / Reload) on the start; creation + destruction
	 * (New Folder / New Category / Delete) on the end. pack_end
	 * appends from the right edge inward, so deletebtn first /
	 * mkcatbtn / mkdirbtn last yields mkdir / mkcat / delete
	 * left-to-right with the close button at the very right. */
	{
		GtkWidget *header = adw_header_bar_new ();

		adw_header_bar_pack_start (ADW_HEADER_BAR (header), parentbtn);
		adw_header_bar_pack_start (ADW_HEADER_BAR (header), reloadbtn);

		adw_header_bar_pack_end (ADW_HEADER_BAR (header), deletebtn);
		adw_header_bar_pack_end (ADW_HEADER_BAR (header), mkcatbtn);
		adw_header_bar_pack_end (ADW_HEADER_BAR (header), mkdirbtn);

		gtk_window_set_titlebar (GTK_WINDOW (news_window), header);
	}

	gtkhx_widget_set_child(news_scroll, news_list);
	gtk_window_set_child(GTK_WINDOW(news_window), news_scroll);

	gfnews->window = news_window;
	gfnews->news_list = news_list;

	gtk_window_present(GTK_WINDOW(news_window));
	init_keyaccel(news_window);

	gfnews_list = gfnews;

	return gfnews;
}

void output_news_folder (struct gnews_folder *gfnews)
{
	struct news_folder *news = gfnews->news;
	GtkWidget *news_list;
	gint row, i;
	GdkPixmap *icon;
	GdkBitmap *mask = NULL;
	struct path_hist *path = NULL;

	news_list = gfnews->news_list;

	if(strcmp(gfnews->path, gfnews->path_list->path)) {
		path = g_malloc(sizeof(struct path_hist)+strlen(gfnews->path));
		strcpy(path->path, gfnews->path);
		path->prev = 0;
		if(gfnews->path_list) {
			path->prev = gfnews->path_list;
		}
		gfnews->path_list = path;
	}

	gtk_widget_set_sensitive(gfnews->up_btn, gfnews->path_list->prev!=NULL &&
		gtkhx_prefs.news_samewin);

	gtk_hlist_clear(GTK_HLIST(news_list));

	gtk_hlist_freeze(GTK_HLIST(news_list));
	for(i = 0; i < news->num_entries; i++) {
		struct folder_item *item = news->entry[i];
		gchar *nulls[2] = {0, 0};
		
		/* type 1 is folder */
		/* other is category */

		row = gtk_hlist_append(GTK_HLIST(news_list), nulls);
		gtk_hlist_set_row_data(GTK_HLIST(news_list), row, item);
		icon = (GdkPixmap *)gdk_pixbuf_new_from_resource(
			item->type == 1
				? "/com/nasledov/gtkhx/pixmaps/newsfld.xpm"
				: "/com/nasledov/gtkhx/pixmaps/newscat.xpm",
			NULL);
		gtk_hlist_set_pixtext(GTK_HLIST(news_list), row, 0, item->name, 34,
							  icon, mask);
		/* Phase 5 dark-theme: theme default foreground applies. */
	}
	gtk_hlist_thaw(GTK_HLIST(news_list));

	gfnews->listing = 0;
}

static struct gnews_catalog *gcnews_with_group (struct news_group *group)
{
	struct gnews_catalog *gcnews;

	for(gcnews = gcnews_list; gcnews; gcnews = gcnews->prev) {
		if(group == gcnews->group) {
			return gcnews;
		}
	}

	return 0;
}

static void delete_gcnews(struct gnews_catalog *gcnews)
{
	int i;

	if (gcnews->next)
		gcnews->next->prev = gcnews->prev;
	if (gcnews->prev)
		gcnews->prev->next = gcnews->next;
	if (gcnews == gcnews_list)
		gcnews_list = gcnews->prev;

	for(i = 0; i < gcnews->group->post_count; i++) {
		g_free(gcnews->group->posts[i].sender);
		g_free(gcnews->group->posts[i].subject);
		g_free(gcnews->group->posts[i].parts);
	}
	if(gcnews->group->post_count)
		g_free(gcnews->group->posts);
	g_free(gcnews->group);
	g_free(gcnews->path);
	g_free(gcnews);
}


/* Phase 4.5: GTK 4 close-request — FALSE allows default destroy. */
static gboolean destroy_gcnews_browser(GtkWindow *window, gpointer data)
{
	struct gnews_catalog *gcnews = g_object_get_data(G_OBJECT(window),
													   "gcnews");
	(void) data;

	delete_gcnews(gcnews);
	return FALSE;
}

/* Phase 2.8: now the GtkTreeView "cursor-changed" handler. */
static void newsc_clicked (GtkTreeView *tree, struct gnews_catalog *gcnews)
{
	struct news_item *item = gcnews_selected_item(gcnews);

	if (item)
		hx_news15_get_post(&the_session.htlc, item);
	(void)tree;
}

static void news15_do_reply(GtkWidget *btn, struct gnews_catalog *gcnews)
{
	GtkWidget *text    = g_object_get_data (G_OBJECT (btn), "text");
	GtkWidget *reply   = g_object_get_data (G_OBJECT (btn), "reply");
	GtkWidget *subject = g_object_get_data (G_OBJECT (btn), "subject");
	GtkWidget *window  = g_object_get_data (G_OBJECT (btn), "window");
	GtkTextBuffer *tbuf;
	GtkTextIter tstart, tend;
	char *textbuf;
	guint32 postid;
	const char *subjectbuf;

	/* `text` is a GtkTextView (see news15_post_window); pull body
	 * via the buffer API. The previous gtk_editable_get_chars
	 * call returned NULL and the next strlen segfaulted on Post.
	 * Mirror what news15_do_post already does. */
	tbuf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (text));
	gtk_text_buffer_get_start_iter (tbuf, &tstart);
	gtk_text_buffer_get_end_iter   (tbuf, &tend);
	textbuf = gtk_text_buffer_get_text (tbuf, &tstart, &tend, FALSE);

	postid     = atoi (gtk_editable_get_text (GTK_EDITABLE (reply)));
	subjectbuf = gtk_editable_get_text (GTK_EDITABLE (subject));

	hx_news15_post_thread (&the_session.htlc, gcnews->path, subjectbuf,
	                       postid, textbuf);
	g_free (textbuf);

	hx_news15_cat_list (&the_session.htlc, gcnews);
	gtkhx_widget_destroy (window);
}

static void news15_cancel_post(GtkWidget *btn, GtkWidget *window)
{
	gtkhx_widget_destroy(window);
}

static void news15_delete(GtkWidget *btn, struct gnews_catalog *gcnews)
{
	struct news_item *item = gcnews_selected_item(gcnews);

	if (!item)
		return;

	hx_news15_delete_thread(&the_session.htlc, gcnews->path, item->postid);
	hx_news15_cat_list(&the_session.htlc, gcnews);
	(void)btn;
}

static void news15_reply (GtkWidget *btn, struct gnews_catalog *gcnews)
{
	struct news_item *item = NULL;
	GtkWidget *window;
	GtkWidget *inreplyto;
	GtkWidget *replylbl;
	GtkWidget *subject;
	GtkWidget *subjectlbl;
	GtkWidget *text;
	GtkWidget *textlbl;
	GtkWidget *post, *cancel;
	GtkWidget *hbox, *vbox;
	GtkWidget *table;

	item = gcnews_selected_item(gcnews);

	window = gtk_window_new();
	/* Phase 5: AdwHeaderBar + default-size for the Reply variant of
	 * Post News (1.5+). The legacy gtk_widget_set_size_request was
	 * baking 320x250 in as a hard floor — switched to
	 * gtk_window_set_default_size so the user can shrink the window
	 * if their screen is narrow. */
	gtk_window_set_titlebar (GTK_WINDOW (window), adw_header_bar_new ());
	gtk_window_set_default_size (GTK_WINDOW (window), 420, 320);
	gtk_window_set_title(GTK_WINDOW(window), _("Reply to News Post"));
    (gtk_widget_set_margin_start(window, 5), gtk_widget_set_margin_end(window, 5), gtk_widget_set_margin_top(window, 5), gtk_widget_set_margin_bottom(window, 5));

	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	table = gtkhx_grid_new_table(3, 2, 0);

	replylbl = gtk_label_new("In Reply To Post #: ");
	inreplyto = gtk_entry_new();
	if(item) {
		char *buf = g_strdup_printf("%d", item->postid);
		
		gtk_editable_set_text(GTK_EDITABLE(inreplyto), buf);
		g_free(buf);
	}

	gtkhx_grid_attach_table(GTK_GRID(table), replylbl, 0, 1, 0, 1, GTK_EXPAND|GTK_FILL, 0, 0, 0);
	gtkhx_grid_attach_table(GTK_GRID(table), inreplyto, 1, 2, 0, 1, GTK_EXPAND|GTK_FILL, 0, 0, 0);

	subjectlbl = gtk_label_new("Subject: ");
	subject = gtk_entry_new();

	if(item) {
		if(strncasecmp(item->subject, "re:", 3)) {
			char *buf = g_strdup_printf("Re: %s", item->subject);
			gtk_editable_set_text(GTK_EDITABLE(subject), buf);
			g_free(buf);
		}
		else {
			gtk_editable_set_text(GTK_EDITABLE(subject), item->subject);
		}
	}

	gtkhx_grid_attach_table(GTK_GRID(table), subjectlbl, 0, 1, 1, 2, GTK_EXPAND|GTK_FILL, 0, 0, 0);
	gtkhx_grid_attach_table(GTK_GRID(table), subject, 1, 2, 1, 2, GTK_EXPAND|GTK_FILL, 0, 0, 0);

	textlbl = gtk_label_new(_("Body: "));
	
	gtkhx_grid_attach_table(GTK_GRID(table), textlbl, 0, 1, 2, 3, GTK_EXPAND|GTK_FILL, 0, 0, 0);

	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	text = gtk_text_view_new();
	gtk_text_view_set_editable(GTK_TEXT_VIEW(text), TRUE);
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text), GTK_WRAP_WORD);
	{
		GtkWidget *text_scroll = gtk_scrolled_window_new();
		gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(text_scroll),
		                               GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
		gtkhx_widget_set_child(text_scroll, text);
		gtkhx_box_pack(hbox, text_scroll, 1, 1, 0);
	}

	gtkhx_box_pack(vbox, table, 0, 0, 0);
	gtkhx_box_pack(vbox, hbox, 0, 0, 10);

	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

	post = gtk_button_new_with_label(_("Post"));
	gtk_widget_add_css_class (post, "suggested-action");
	g_object_set_data(G_OBJECT(post), "text", text);
	g_object_set_data(G_OBJECT(post), "reply", inreplyto);
	g_object_set_data(G_OBJECT(post), "subject", subject);
	g_object_set_data(G_OBJECT(post), "window", window);
	g_signal_connect(post, "clicked", G_CALLBACK(news15_do_reply), gcnews);

	cancel = gtk_button_new_with_label(_("Cancel"));
	g_signal_connect(cancel, "clicked", G_CALLBACK(news15_cancel_post),
					   window);

	/* Phase 5: right-align the button row so Post sits where Enter
	 * lives (HIG-conventional), with Cancel to its left. */
	gtk_widget_set_halign (hbox, GTK_ALIGN_END);
	gtk_box_set_spacing  (GTK_BOX (hbox), 8);
	gtkhx_box_pack(hbox, cancel, 0, 0, 0);
	gtkhx_box_pack(hbox, post, 0, 0, 0);

	gtkhx_box_pack(vbox, hbox, 0, 0, 0);

	gtkhx_widget_set_child(window, vbox);
	init_keyaccel(window);
	gtk_window_present(GTK_WINDOW(window));
}

static void news15_do_post(GtkWidget *btn, struct gnews_catalog *gcnews)
{
	GtkWidget *text = g_object_get_data(G_OBJECT(btn), "text");
	GtkWidget *subject = g_object_get_data(G_OBJECT(btn), "subject");
	GtkWidget *window = g_object_get_data(G_OBJECT(btn), "window");
	GtkTextBuffer *tbuf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text));
	GtkTextIter tstart, tend;
	char *textbuf;
	const char *subjectbuf = gtk_editable_get_text(GTK_EDITABLE(subject));

	gtk_text_buffer_get_start_iter(tbuf, &tstart);
	gtk_text_buffer_get_end_iter(tbuf, &tend);
	textbuf = gtk_text_buffer_get_text(tbuf, &tstart, &tend, FALSE);

	hx_news15_post_thread(&the_session.htlc, gcnews->path, subjectbuf,
						  0, textbuf);
	g_free(textbuf);

	hx_news15_cat_list(&the_session.htlc, gcnews);
	gtkhx_widget_destroy(window);
}

static void news15_post (GtkWidget *btn, struct gnews_catalog *gcnews)
{
	GtkWidget *window;
	GtkWidget *subject;
	GtkWidget *subjectlbl;
	GtkWidget *text;
	GtkWidget *textlbl;
	GtkWidget *post, *cancel;
	GtkWidget *hbox, *vbox;
	GtkWidget *table;

	window = gtk_window_new();
	/* Phase 5: AdwHeaderBar + default-size; same treatment as the
	 * Reply variant above. */
	gtk_window_set_titlebar (GTK_WINDOW (window), adw_header_bar_new ());
	gtk_window_set_default_size (GTK_WINDOW (window), 420, 320);
	gtk_window_set_title(GTK_WINDOW(window), _("Post News (1.5+)"));
    (gtk_widget_set_margin_start(window, 5), gtk_widget_set_margin_end(window, 5), gtk_widget_set_margin_top(window, 5), gtk_widget_set_margin_bottom(window, 5));

	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	table = gtkhx_grid_new_table(2, 2, 0);

	subjectlbl = gtk_label_new(_("Subject: "));
	subject = gtk_entry_new();

	gtkhx_grid_attach_table(GTK_GRID(table), subjectlbl, 0, 1, 0, 1, GTK_EXPAND|GTK_FILL, 0, 0, 0);
	gtkhx_grid_attach_table(GTK_GRID(table), subject, 1, 2, 0, 1, GTK_EXPAND|GTK_FILL, 0, 0, 0);

	textlbl = gtk_label_new(_("Body: "));
	
	gtkhx_grid_attach_table(GTK_GRID(table), textlbl, 0, 1, 1, 2, GTK_EXPAND|GTK_FILL, 0, 0, 0);

	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	text = gtk_text_view_new();
	gtk_text_view_set_editable(GTK_TEXT_VIEW(text), TRUE);
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text), GTK_WRAP_WORD);
	{
		GtkWidget *post_scroll = gtk_scrolled_window_new();
		gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(post_scroll),
		                               GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
		gtkhx_widget_set_child(post_scroll, text);
		gtkhx_box_pack(hbox, post_scroll, 1, 1, 0);
	}

	gtkhx_box_pack(vbox, table, 0, 0, 0);
	gtkhx_box_pack(vbox, hbox, 0, 0, 10);

	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

	post = gtk_button_new_with_label(_("Post"));
	gtk_widget_add_css_class (post, "suggested-action");
	g_object_set_data(G_OBJECT(post), "text", text);
	g_object_set_data(G_OBJECT(post), "subject", subject);
	g_object_set_data(G_OBJECT(post), "window", window);
	g_signal_connect(post, "clicked", G_CALLBACK(news15_do_post), gcnews);

	cancel = gtk_button_new_with_label(_("Cancel"));
	g_signal_connect(cancel, "clicked", G_CALLBACK(news15_cancel_post),
					   window);

	gtk_widget_set_halign (hbox, GTK_ALIGN_END);
	gtk_box_set_spacing  (GTK_BOX (hbox), 8);
	gtkhx_box_pack(hbox, cancel, 0, 0, 0);
	gtkhx_box_pack(hbox, post, 0, 0, 0);

	gtkhx_box_pack(vbox, hbox, 0, 0, 0);

	gtkhx_widget_set_child(window, vbox);
	init_keyaccel(window);
	gtk_window_present(GTK_WINDOW(window));
}

static void gcnews_reload_btn(GtkWidget *btn, struct gnews_catalog *gcnews)
{
	if(gcnews->listing)
		return;
	if (gcnews->news_store)
		gtk_tree_store_clear(gcnews->news_store);
	hx_news15_cat_list(&the_session.htlc, gcnews);
	(void)btn;
}

struct gnews_catalog *create_gcnews_window (char *path)
{
	struct gnews_catalog *gcnews = g_malloc0(sizeof(struct gnews_catalog));
	GtkWidget *news_window;
	GtkWidget *hpaned1;
	GtkWidget *vbox1;
	GtkWidget *reloadbtn;
	GtkWidget *postbtn;
	GtkWidget *replybtn;
	GtkWidget *deletebtn;
	GtkWidget *news_tree;
	GtkWidget *vbox2;
	GtkWidget *authorlbl;
	GtkWidget *datelbl;
	GtkWidget *subjectlbl;
	GtkWidget *scrolledwindow1;
	GtkWidget *news_text;
	GtkWidget *scrolledwindow2;

	gcnews->listing = 0;
	gcnews->prev = gcnews_list;
	gcnews->next = 0;
	gcnews->path = strdup(path);



	if(gcnews_list) {
		gcnews_list->next = gcnews;
	}

	news_window = gtk_window_new();
	gtk_window_set_resizable(GTK_WINDOW(news_window), TRUE);
	/* Phase 3.x: dropped GTK 1.2-era realize+get_style pair (style unused). */
	gtk_widget_set_size_request(news_window, 570, 375);
	gtk_window_set_title(GTK_WINDOW(news_window), path);
	g_object_set_data(G_OBJECT(news_window), "gcnews", gcnews);
	g_signal_connect(news_window, "close-request",
					   G_CALLBACK(destroy_gcnews_browser), 0);

	hpaned1 = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
	gtkhx_widget_set_child(news_window, hpaned1);
	(gtk_widget_set_margin_start(hpaned1, 4), gtk_widget_set_margin_end(hpaned1, 4), gtk_widget_set_margin_top(hpaned1, 4), gtk_widget_set_margin_bottom(hpaned1, 4));
	gtk_paned_set_position (GTK_PANED (hpaned1), 285);

	vbox1 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_paned_set_start_child(GTK_PANED(hpaned1), vbox1);
	
	/* Phase 5: 2x-scaled headerbar buttons via gtkhx_pixmap_button. */
	reloadbtn = gtkhx_pixmap_button (
		"/com/nasledov/gtkhx/pixmaps/refresh.xpm",
		_("Reload"), 2,
		G_CALLBACK (gcnews_reload_btn), gcnews);

	postbtn = gtkhx_pixmap_button (
		"/com/nasledov/gtkhx/pixmaps/postnews.xpm",
		_("Post Thread"), 2,
		G_CALLBACK (news15_post), gcnews);

	/* Reply has no dedicated XPM yet — keep the "[ R ]" text label. */
	replybtn = gtk_button_new_with_label ("[ R ]");
	g_signal_connect (replybtn, "clicked",
	                  G_CALLBACK (news15_reply), gcnews);
	gtk_widget_set_tooltip_text (replybtn, _("Reply To Thread"));

	deletebtn = gtkhx_pixmap_button (
		"/com/nasledov/gtkhx/pixmaps/trash.xpm",
		_("Delete Thread"), 2,
		G_CALLBACK (news15_delete), gcnews);

	/* Phase 5: action buttons live in the AdwHeaderBar instead of a
	 * topframe + hbuttonbox row inside the left pane. Reload + Post
	 * + Reply are non-destructive on the start; Delete (destructive)
	 * sits at the end. */
	{
		GtkWidget *header = adw_header_bar_new ();

		adw_header_bar_pack_start (ADW_HEADER_BAR (header), reloadbtn);
		adw_header_bar_pack_start (ADW_HEADER_BAR (header), postbtn);
		adw_header_bar_pack_start (ADW_HEADER_BAR (header), replybtn);

		adw_header_bar_pack_end (ADW_HEADER_BAR (header), deletebtn);

		gtk_window_set_titlebar (GTK_WINDOW (news_window), header);
	}

	/* Phase 3.9: dropped a GtkAlignment(0.5, 0.5, 1, 1) wrapper around
	 * the scrolled window. The xscale/yscale=1 made it expand to fill,
	 * which is what gtk_box_pack_start(TRUE, TRUE) already provides;
	 * GtkAlignment is gone in GTK 4 anyway. */
	scrolledwindow2 = gtk_scrolled_window_new();
	gtkhx_box_pack(vbox1, scrolledwindow2, TRUE, TRUE, 0);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolledwindow2), GTK_POLICY_AUTOMATIC, GTK_POLICY_ALWAYS);

	/* Phase 5: GtkTreeView (in tree mode) replaces GtkCTree (Phase 2.8).
	 * news_store holds the model; news_tree is the view.
	 *
	 * No GtkViewport wrapper between scrolledwindow2 and news_tree:
	 * GtkScrolledWindow only auto-wraps children that don't implement
	 * GtkScrollable, and GtkTreeView does — adding a viewport made
	 * the view dark-on-dark on some themes (the viewport carries a
	 * .view CSS class whose background landed under text whose
	 * foreground stayed at the default cell-renderer black). */
	gcnews->news_store = gtk_tree_store_new(NEWS_N_COLS,
											G_TYPE_STRING,
											G_TYPE_POINTER);
	news_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(gcnews->news_store));
	g_object_unref(gcnews->news_store);  /* the view holds the only ref now */
	gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(news_tree), FALSE);
	{
		GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
		GtkTreeViewColumn *col;
		/* Ensure the renderer never has a stale foreground-set TRUE
		 * lingering from any future model column attribute mapping —
		 * the catalog model has no foreground column so the theme
		 * default should always apply. */
		g_object_set (renderer, "foreground-set", FALSE, NULL);
		col = gtk_tree_view_column_new_with_attributes("Subject", renderer,
													   "text", NEWS_COL_SUBJECT,
													   NULL);
		gtk_tree_view_append_column(GTK_TREE_VIEW(news_tree), col);
	}
	gtk_tree_selection_set_mode(
		gtk_tree_view_get_selection(GTK_TREE_VIEW(news_tree)),
		GTK_SELECTION_BROWSE);
	g_signal_connect(news_tree, "cursor-changed",
					   G_CALLBACK(newsc_clicked), gcnews);
	gtkhx_widget_set_child(scrolledwindow2, news_tree);

	vbox2 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_paned_set_end_child(GTK_PANED(hpaned1), vbox2);
	
	authorlbl = gtk_label_new (_("Author: "));
	gtk_label_set_justify(GTK_LABEL(authorlbl), GTK_JUSTIFY_LEFT);
	gtkhx_box_pack(vbox2, authorlbl, 0, 1, 0);
	
	datelbl = gtk_label_new (_("Date: "));
	gtk_label_set_justify(GTK_LABEL(datelbl), GTK_JUSTIFY_LEFT);
	gtkhx_box_pack(vbox2, datelbl, 0, 1, 0);
	
	subjectlbl = gtk_label_new (_("Subject: "));
	gtk_label_set_justify(GTK_LABEL(subjectlbl), GTK_JUSTIFY_LEFT);
	gtkhx_box_pack(vbox2, subjectlbl, 0, 1, 0);
	
	scrolledwindow1 = gtk_scrolled_window_new();
	gtkhx_box_pack(vbox2, scrolledwindow1, TRUE, TRUE, 0);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolledwindow1), GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
	
	news_text = gtk_text_view_new ();
	gtk_text_view_set_editable (GTK_TEXT_VIEW (news_text), FALSE);
	gtk_text_view_set_cursor_visible (GTK_TEXT_VIEW (news_text), FALSE);
	gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (news_text), GTK_WRAP_WORD);
	gtkhx_widget_set_child(scrolledwindow1, news_text);
	gtk_window_present(GTK_WINDOW(news_window));

	gcnews->window = news_window;
	gcnews->news_tree = news_tree;
	gcnews->news_text = news_text;
	gcnews->subjectlbl = subjectlbl;
	gcnews->datelbl = datelbl;
	gcnews->authorlbl = authorlbl;
	gcnews_list = gcnews;

	init_keyaccel(news_window);

	return gcnews;
}

void output_news_catalog(struct gnews_catalog *gcnews)
{
	struct news_group *group = gcnews->group;
	GtkTreeStore *store = gcnews->news_store;
	int i;

	gcnews->group->path = gcnews->path;

	if (!store)
		return;

	gtk_tree_store_clear(store);

	/* Detach the view from the model during the bulk insert so the
	 * row-inserted signal doesn't trigger O(N) work per row. This is
	 * the GtkTreeView equivalent of the old gtk_clist_freeze/thaw. */
	g_object_ref(store);
	gtk_tree_view_set_model(GTK_TREE_VIEW(gcnews->news_tree), NULL);

	for (i = 0; i < group->post_count; i++) {
		struct news_item *item = &(group->posts[i]);
		GtkTreeIter *parent_iter = NULL;
		int j;

		for (j = 0; j < group->post_count; j++) {
			if (j != i && group->posts[j].postid == item->parentid) {
				parent_iter = &(group->posts[j].iter);
				break;
			}
		}

		gtk_tree_store_append(store, &item->iter, parent_iter);
		gtk_tree_store_set(store, &item->iter,
						   NEWS_COL_SUBJECT, item->subject ? item->subject : "",
						   NEWS_COL_ITEM, item,
						   -1);
	}

	gtk_tree_view_set_model(GTK_TREE_VIEW(gcnews->news_tree),
							GTK_TREE_MODEL(store));
	g_object_unref(store);
	gtk_tree_view_expand_all(GTK_TREE_VIEW(gcnews->news_tree));

	gcnews->listing = 0;
}
 
static time_t date_to_unix (struct date_time *dt)
{
	/* check if the year is after the epoch */
	if(dt->base_year >= 1970) {
		struct tm timetm;
		time_t timet;
		
		 memset(&timet, 0, sizeof(struct tm));
		 /* the 24*3600 thing is a hack for a weird bug
			where the date would be displayed one day behind from
			the actual date ... quite odd */
		 timetm.tm_sec = dt->seconds+(24*3600);
		 timetm.tm_year = (dt->base_year-1900);
		 if(timetm.tm_year < 0)
			 timetm.tm_year = 1970;
		 timet =  mktime(&timetm);	
		 return timet;
	}
	else {
		/* crackhead base_year detected */
		if(dt->base_year == 1904)
 			return dt->seconds-2082844800U;
	}
	
	return 0;
}

void output_news_thread(struct news_post *post)
{
	struct gnews_catalog *gcnews = gcnews_with_group(post->item->group);
	struct news_item *item = post->item;
	time_t timet;

	if(!gcnews) {
		return;
	}
	
	timet = date_to_unix(&item->date);

	/* news_text is a GtkTextView, not a GtkEditable — gtk_editable_*
	 * functions don't apply and trip a Gtk-CRITICAL on the cast. The
	 * gtk_text_buffer_set_text call below already replaces the buffer's
	 * entire content, so the legacy "clear then write" pattern from the
	 * GtkText era collapses to just "write". */

	if (item) {
		char *date = g_strdup_printf("Date: %s", ctime(&timet));

		/* get rid of the line break ctime() adds in */
		date[strlen(date)-1] = '\0';

		/* preliminary post thread displaying code */
		if(item->sender) {
			char *str = g_strdup_printf("Author: %s", item->sender);
			gtk_label_set_text(GTK_LABEL(gcnews->authorlbl), str);
			g_free(str);
		}
		
		if(item->subject) {
			char *str = g_strdup_printf("Subject: %s", item->subject);
			gtk_label_set_text(GTK_LABEL(gcnews->subjectlbl), str);
			g_free(str);
		}
		
		gtk_label_set_text(GTK_LABEL(gcnews->datelbl), date);
		g_free(date);
	}
	
	/* output the contents of the post */
	{
		GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gcnews->news_text));
		gsize utf8_len;
		char *utf8 = gtkhx_text_to_utf8(post->buf, strlen(post->buf), &utf8_len);
		gtk_text_buffer_set_text(buf, utf8, (gint) utf8_len);
		g_free(utf8);
	}
}

void open_news15(GtkWidget *widget, session *sess)
{
	
	struct gnews_folder *gfnews = create_gfnews_window(NULL);

	hx_news15_fldr_list(&the_session.htlc, gfnews);
}

G_GNUC_END_IGNORE_DEPRECATIONS
/* Phase 4.13: end of file-level deprecation suppression — see top of file. */
