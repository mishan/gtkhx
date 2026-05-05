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
	task_new(htlc, rcv_task_news_post, item, 0, "news_post");

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
	task_new(htlc, rcv_task_newscat_list, gcnews, 0, "news_category");
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
	task_new(htlc, rcv_task_newsfolder_list, gfnews, 0, "news_folder");

	hlwrite(htlc, HTLC_HDR_NEWSDIRLIST, 0, 1, 
			HTLC_DATA_NEWSPATH, hldirlen, hldir);
	g_free(hldir);
}

void hx_news15_post_thread(struct htlc_conn *htlc, char *path, char *subject,
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

void hx_news15_delete_thread(struct htlc_conn *htlc, char *path, 
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



void hx_news15_delete(struct htlc_conn *htlc, char *path)
{
	guint8 *hldir;
	guint16 hldirlen;
	
	hldir = path_to_hldir(path, &hldirlen, 0);
	task_new(htlc, 0, 0, 0, "news15_rm");
	hlwrite(htlc, HTLC_HDR_DELNEWSDIRCAT, 0, 1,
			HTLC_DATA_NEWSPATH, hldirlen, hldir);
	g_free(hldir);
}

void hx_news15_mkcat(struct htlc_conn *htlc, char *path, char *name)
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

void hx_news15_mkdir(struct htlc_conn *htlc, char *path)
{
	guint8 *hldir;
	guint16 hldirlen;
	
	hldir = path_to_hldir(path, &hldirlen, 0);
	task_new(htlc, 0, 0, 0, "news15_mkdir");
	hlwrite(htlc, HTLC_HDR_MAKENEWSDIR, 0, 1,
			HTLC_DATA_NEWSPATH, hldirlen, hldir);
	g_free(hldir);
}

static void newsf_clicked(GtkWidget *widget, GdkEventButton *event)
{
	struct gnews_folder *gfnews;
	int row, col;

	gfnews = gfnews_with_hlist(widget);
	if(!gfnews)
		return;
	gtk_hlist_get_selection_info(GTK_HLIST(widget),
								 event->x, event->y, &row, &col);

	if(gfnews->listing)
		return;
	if(event->type == GDK_2BUTTON_PRESS) {
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
					struct gnews_folder *gfnews = NULL;

					gfnews = create_gfnews_window(path);
					hx_news15_fldr_list(&the_session.htlc, gfnews);
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

struct gnews_folder *gfnews_with_path (char *path)
{
	struct gnews_folder *gfnews;

	for(gfnews = gfnews_list; gfnews; gfnews = gfnews->prev) {
		if(!strcmp(gfnews->news->path, path)) {
			return gfnews;
		}
	}
	return 0;
}

void delete_gfnews(struct gnews_folder *gfnews)
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

void destroy_gfnews_browser(GtkWidget *widget, gpointer data)
{
	struct gnews_folder *gfnews = g_object_get_data(G_OBJECT(widget), 
													  "gfnews");

	delete_gfnews(gfnews);
	gtk_widget_destroy(widget);
}

static void gfnews_reload_btn(GtkWidget *btn, struct gnews_folder *gfnews)
{
	if(gfnews->listing)
		return;
	gtk_hlist_clear(GTK_HLIST(gfnews->news_list));
	hx_news15_fldr_list(&the_session.htlc, gfnews);
}

static void gfnews_mkdir(GtkWidget *widget, gpointer data)
{
	GtkWidget *entry = (GtkWidget *)g_object_get_data(G_OBJECT(widget),
														"entry");
	struct gnews_folder *gfnews = g_object_get_data(G_OBJECT(widget),
													  "gfnews");
	char *name = gtk_entry_get_text(GTK_ENTRY(entry));
	char pathname[MAXPATHLEN];

	snprintf(pathname, MAXPATHLEN, "%s/%s", gfnews->path, name);

	hx_news15_mkdir(&the_session.htlc, pathname);
	hx_news15_fldr_list(&the_session.htlc, gfnews);

	gtk_widget_destroy(GTK_WIDGET(data));
}
static void gfnews_mkcat(GtkWidget *widget, gpointer data)
{
	GtkWidget *entry = (GtkWidget *)g_object_get_data(G_OBJECT(widget),
														"entry");
	struct gnews_folder *gfnews = g_object_get_data(G_OBJECT(widget),
													  "gfnews");
	char *name = gtk_entry_get_text(GTK_ENTRY(entry));

	hx_news15_mkcat(&the_session.htlc, gfnews->path, name);
	hx_news15_fldr_list(&the_session.htlc, gfnews);

	gtk_widget_destroy(GTK_WIDGET(data));
}

static void gfnews_mkdir_btn(GtkWidget *btn, struct gnews_folder *gfnews)
{
	GtkWidget *dialog;
	GtkWidget *nameEntry;
	GtkWidget *okBtn;
	GtkWidget *cancelBtn;
	GtkWidget *nameEntryLabel;
	GtkWidget *entryHbox;
	GtkWidget *btnHbox;

	dialog = gtk_dialog_new();
	gtk_window_set_title(GTK_WINDOW(dialog), _("New News Folder..."));
	entryHbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

    gtk_container_set_border_width (GTK_CONTAINER(dialog), 5);
	gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), entryHbox ,0, 0, 0);
	nameEntryLabel = gtk_label_new(_("Name: "));
	nameEntry = gtk_entry_new();
	gtk_box_pack_start(GTK_BOX(entryHbox), nameEntryLabel, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(entryHbox), nameEntry, 0, 0, 0);

	okBtn = gtk_button_new_with_label(_("OK"));
	g_object_set_data(G_OBJECT(okBtn), "entry", nameEntry);
	g_object_set_data(G_OBJECT(okBtn), "gfnews", gfnews);
	g_signal_connect(okBtn, "clicked", G_CALLBACK(gfnews_mkdir),
					   dialog);

	cancelBtn = gtk_button_new_with_label(_("Cancel"));
	g_signal_connect_swapped(cancelBtn, "clicked",
							  (GCallback) gtk_widget_destroy,
							  dialog);

	btnHbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_can_default(okBtn, TRUE);
	gtk_container_add(GTK_CONTAINER(gtkhx_dialog_action_area(GTK_DIALOG(dialog))), btnHbox);
	gtk_box_pack_start(GTK_BOX(btnHbox), okBtn, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(btnHbox), cancelBtn, 0, 0, 0);
	gtk_widget_show_all(dialog);
	gtk_widget_grab_default(okBtn);
}

static void gfnews_mkcat_btn(GtkWidget *btn, struct gnews_folder *gfnews)
{
	GtkWidget *dialog;
	GtkWidget *nameEntry;
	GtkWidget *okBtn;
	GtkWidget *cancelBtn;
	GtkWidget *nameEntryLabel;
	GtkWidget *entryHbox;
	GtkWidget *btnHbox;

	dialog = gtk_dialog_new();
	gtk_window_set_title(GTK_WINDOW(dialog), _("New News Category..."));
	entryHbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

    gtk_container_set_border_width (GTK_CONTAINER(dialog), 5);
	gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), entryHbox ,0, 0, 0);
	nameEntryLabel = gtk_label_new(_("Name: "));
	nameEntry = gtk_entry_new();
	gtk_box_pack_start(GTK_BOX(entryHbox), nameEntryLabel, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(entryHbox), nameEntry, 0, 0, 0);

	okBtn = gtk_button_new_with_label(_("OK"));
	g_object_set_data(G_OBJECT(okBtn), "entry", nameEntry);
	g_object_set_data(G_OBJECT(okBtn), "gfnews", gfnews);
	g_signal_connect(okBtn, "clicked", G_CALLBACK(gfnews_mkcat),
					   dialog);

	cancelBtn = gtk_button_new_with_label(_("Cancel"));
	g_signal_connect_swapped(cancelBtn, "clicked",
							  (GCallback) gtk_widget_destroy,
							  dialog);

	btnHbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_can_default(okBtn, TRUE);
	gtk_container_add(GTK_CONTAINER(gtkhx_dialog_action_area(GTK_DIALOG(dialog))), btnHbox);
	gtk_box_pack_start(GTK_BOX(btnHbox), okBtn, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(btnHbox), cancelBtn, 0, 0, 0);
	gtk_widget_show_all(dialog);
	gtk_widget_grab_default(okBtn);
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

static GtkTargetEntry news15_drag[] =
{{"hlist", GTK_TARGET_SAME_APP, 0}};

static void news15_drag_send(GtkWidget *source, GdkDragContext *context,
							 GtkSelectionData *selection, guint targetType,
							 guint eventTime)
{
	/* We are the knights who say "Nee!" */
	gtk_selection_data_set(selection, gtk_selection_data_get_target(selection), 0, NULL, 0);
}

static void news15_drag_receive(GtkWidget *target, GdkDragContext *context,
								int x, int y, GtkSelectionData *selection,
								guint targetType, guint time, gpointer data)
{
	GtkWidget *source = gtk_drag_get_source_widget(context);
	struct gnews_folder *gfnews_target, *gfnews_source;
	struct folder_item *item = NULL;
	char pathf[4096], patht[4096];
	
	return;
	
	gfnews_source = gfnews_with_hlist(source);
	item = gtk_hlist_get_row_data(GTK_HLIST(source), gfnews_source->row);
	if(!item) {
		return;
	}
	
	gfnews_target = gfnews_with_hlist(target);

	/* XXX: this doesn't actually do anything */
	if(strcmp(gfnews_source->path, gfnews_target->path)) {
		g_snprintf(pathf, sizeof(pathf), "%s/%s", gfnews_source->path, item->name);
		g_snprintf(patht, sizeof(patht), "%s/", gfnews_target->path);
		
/*		
	hx_news15_move(&the_session.htlc, pathf, patht);
		
hx_news15_fldr_list(&the_session.htlc, gfnews_target);
hx_news15_fldr_list(&the_session.htlc, gfnews_source); 
*/
	}
}


struct gnews_folder *create_gfnews_window(char *path)
{
	struct gnews_folder *gfnews = g_malloc(sizeof(struct gnews_folder));
	GtkWidget *news_window;
	GtkWidget *news_list;
	GtkWidget *news_scroll;
	GtkWidget *topframe;
	GtkWidget *hbuttonbox;
	GtkWidget *vbox;
	GtkWidget *parentbtn;
	GtkWidget *reloadbtn;
	GtkWidget *deletebtn;
	GtkWidget *mkdirbtn;
	GtkWidget *mkcatbtn;
	GtkStyle *style;
	GdkBitmap *mask;
	GdkPixmap *icon;
	GtkWidget *pix;

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

	news_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_resizable(GTK_WINDOW(news_window), TRUE);

	gtk_widget_realize(news_window);
	style = gtk_widget_get_style(news_window);
	gtk_widget_set_size_request(news_window, 264, 400);
	gtk_window_set_title(GTK_WINDOW(news_window), gfnews->path);
	g_object_set_data(G_OBJECT(news_window), "gfnews", gfnews);
	g_signal_connect(news_window, "delete_event", 
					   G_CALLBACK(destroy_gfnews_browser), 0);

	news_scroll = gtk_scrolled_window_new(0, 0);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(news_scroll), 
								   GTK_POLICY_AUTOMATIC, GTK_POLICY_ALWAYS);

	news_list = gtk_hlist_new(1);
	gtk_hlist_set_column_width(GTK_HLIST(news_list), 0, 64);
/*	gtk_hlist_set_column_width(GTK_HLIST(news_list), 1, 240); */
	gtk_hlist_set_row_height(GTK_HLIST(news_list), 18);
	gtk_hlist_set_shadow_type(GTK_HLIST(news_list), GTK_SHADOW_NONE);
	gtk_hlist_set_column_justification(GTK_HLIST(news_list), 0, 
									   GTK_JUSTIFY_LEFT);
	g_signal_connect(news_list, "button_press_event", 
					   G_CALLBACK(newsf_clicked), 0);

	g_signal_connect(news_list, "drag_data_get",
					   G_CALLBACK(news15_drag_send), 0);
	gtk_drag_source_set(news_list, GDK_BUTTON1_MASK, news15_drag, 1,
						GDK_ACTION_MOVE);

	g_signal_connect(news_list, "drag_data_received",
					   G_CALLBACK(news15_drag_receive), 0);
	gtk_drag_dest_set(news_list, GTK_DEST_DEFAULT_ALL, news15_drag, 1,
					  GDK_ACTION_MOVE|GDK_ACTION_LINK);

	topframe = gtk_frame_new(0);
	gtk_widget_set_size_request(topframe, -1, 30);
	gtk_frame_set_shadow_type(GTK_FRAME(topframe), GTK_SHADOW_OUT);

	hbuttonbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

	parentbtn =  gtk_button_new();
	g_signal_connect(parentbtn, "clicked",
					   G_CALLBACK(gfnews_up_btn), gfnews);
	gtk_widget_set_tooltip_text(parentbtn, _("Parent Directory"));
	icon = (GdkPixmap *)gdk_pixbuf_new_from_resource("/com/nasledov/gtkhx/pixmaps/up.xpm", NULL);
	pix = gtk_image_new_from_pixbuf((GdkPixbuf *)icon);
	gtk_container_add(GTK_CONTAINER(parentbtn), pix);
	gtk_widget_set_sensitive(parentbtn, gtkhx_prefs.news_samewin);
	gfnews->up_btn = parentbtn;
	pix = 0, icon = 0, mask = 0;

	reloadbtn =  gtk_button_new();
	g_signal_connect(reloadbtn, "clicked",
					   G_CALLBACK(gfnews_reload_btn), gfnews);
	gtk_widget_set_tooltip_text(reloadbtn, _("Reload"));
	icon = (GdkPixmap *)gdk_pixbuf_new_from_resource("/com/nasledov/gtkhx/pixmaps/refresh.xpm", NULL);
	pix = gtk_image_new_from_pixbuf((GdkPixbuf *)icon);
	gtk_container_add(GTK_CONTAINER(reloadbtn), pix);
	pix = 0, icon = 0, mask = 0;

	deletebtn =  gtk_button_new();
	g_signal_connect(deletebtn, "clicked",
					   G_CALLBACK(gfnews_delete_btn), gfnews);
	gtk_widget_set_tooltip_text(deletebtn, _("Delete"));
	icon = (GdkPixmap *)gdk_pixbuf_new_from_resource("/com/nasledov/gtkhx/pixmaps/trash.xpm", NULL);

	pix = gtk_image_new_from_pixbuf((GdkPixbuf *)icon);
	gtk_container_add(GTK_CONTAINER(deletebtn), pix);
	pix = 0, icon = 0, mask = 0;

	mkdirbtn =  gtk_button_new();
	g_signal_connect(mkdirbtn, "clicked",
					   G_CALLBACK(gfnews_mkdir_btn), gfnews);
	gtk_widget_set_tooltip_text(mkdirbtn, _("New Folder"));
	icon = (GdkPixmap *)gdk_pixbuf_new_from_resource("/com/nasledov/gtkhx/pixmaps/newsfld.xpm", NULL);

	pix = gtk_image_new_from_pixbuf((GdkPixbuf *)icon);
	gtk_container_add(GTK_CONTAINER(mkdirbtn), pix);
	pix = 0, icon = 0, mask = 0;

	mkcatbtn =  gtk_button_new();
	g_signal_connect(mkcatbtn, "clicked",
					   G_CALLBACK(gfnews_mkcat_btn), gfnews);
	gtk_widget_set_tooltip_text(mkcatbtn, _("New Category"));
	icon = (GdkPixmap *)gdk_pixbuf_new_from_resource("/com/nasledov/gtkhx/pixmaps/newscat.xpm", NULL);

	pix = gtk_image_new_from_pixbuf((GdkPixbuf *)icon);
	gtk_container_add(GTK_CONTAINER(mkcatbtn), pix);
	pix = 0, icon = 0, mask = 0;

	gtk_box_pack_start(GTK_BOX(hbuttonbox), parentbtn, 0, 0, 2);
	gtk_box_pack_start(GTK_BOX(hbuttonbox), reloadbtn, 0, 0, 2);
	gtk_box_pack_start(GTK_BOX(hbuttonbox), mkdirbtn, 0, 0, 2);
	gtk_box_pack_start(GTK_BOX(hbuttonbox), mkcatbtn, 0, 0, 2);
	gtk_box_pack_start(GTK_BOX(hbuttonbox), deletebtn, 0, 0, 2);

	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_size_request(vbox, 240, 400);
	gtk_container_add(GTK_CONTAINER(topframe), hbuttonbox);
	gtk_box_pack_start(GTK_BOX(vbox), topframe, 0, 0, 0);
	gtk_container_add(GTK_CONTAINER(news_scroll), news_list);
	gtk_box_pack_start(GTK_BOX(vbox), news_scroll, 1, 1, 0);
	gtk_container_add(GTK_CONTAINER(news_window), vbox);

	gfnews->window = news_window;
	gfnews->news_list = news_list;

	gtk_widget_show_all(news_window);
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
	GdkBitmap *mask;
	GtkStyle *style;
	GdkColor col = {0,0,0};
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

	style = gtk_widget_get_style(news_list);

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
		gtk_hlist_set_foreground(GTK_HLIST(news_list), row, &col);
	}
	gtk_hlist_thaw(GTK_HLIST(news_list));

	gfnews->listing = 0;
}

struct gnews_catalog *gcnews_with_path(char *path)
{
	struct gnews_catalog *gcnews;

	for(gcnews = gcnews_list; gcnews; gcnews = gcnews->prev) {
		if(!strcmp(path, gcnews->path)) {
			return gcnews;
		}
	}
	return 0;
}

struct gnews_catalog *gcnews_with_group (struct news_group *group)
{
	struct gnews_catalog *gcnews;

	for(gcnews = gcnews_list; gcnews; gcnews = gcnews->prev) {
		if(group == gcnews->group) {
			return gcnews;
		}
	}

	return 0;
}

void delete_gcnews(struct gnews_catalog *gcnews)
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


static void destroy_gcnews_browser(GtkWidget *widget, gpointer data)
{
	struct gnews_catalog *gcnews = g_object_get_data(G_OBJECT(widget), 
													   "gcnews");

	delete_gcnews(gcnews);
	gtk_widget_destroy(widget);
}

/* Phase 2.8: now the GtkTreeView "cursor-changed" handler. */
void newsc_clicked (GtkTreeView *tree, struct gnews_catalog *gcnews)
{
	struct news_item *item = gcnews_selected_item(gcnews);

	if (item)
		hx_news15_get_post(&the_session.htlc, item);
	(void)tree;
}

void news15_do_reply(GtkWidget *btn, struct gnews_catalog *gcnews)
{
	GtkWidget *text = g_object_get_data(G_OBJECT(btn), "text");
	GtkWidget *reply = g_object_get_data(G_OBJECT(btn), "reply");
	GtkWidget *subject = g_object_get_data(G_OBJECT(btn), "subject");
	GtkWidget *window = g_object_get_data(G_OBJECT(btn), "window");
	char *textbuf = gtk_editable_get_chars(GTK_EDITABLE(text), 0, -1);
	guint32 postid = atoi(gtk_entry_get_text(GTK_ENTRY(reply)));
	char *subjectbuf = gtk_entry_get_text(GTK_ENTRY(subject));

	
	hx_news15_post_thread(&the_session.htlc, gcnews->path, subjectbuf,
						  postid, textbuf);
	hx_news15_cat_list(&the_session.htlc, gcnews);
	gtk_widget_destroy(window);
}

void news15_cancel_post(GtkWidget *btn, GtkWidget *window)
{
	gtk_widget_destroy(window);
}

void news15_delete(GtkWidget *btn, struct gnews_catalog *gcnews)
{
	struct news_item *item = gcnews_selected_item(gcnews);

	if (!item)
		return;

	hx_news15_delete_thread(&the_session.htlc, gcnews->path, item->postid);
	hx_news15_cat_list(&the_session.htlc, gcnews);
	(void)btn;
}

void news15_reply (GtkWidget *btn, struct gnews_catalog *gcnews)
{
	struct news_item *item = NULL;
	GtkWidget *window;
	GtkWidget *inreplyto;
	GtkWidget *replylbl;
	GtkWidget *subject;
	GtkWidget *subjectlbl;
	GtkWidget *text;
	GtkWidget *textlbl;
	GtkWidget *vscroll;
	GtkWidget *post, *cancel;
	GtkWidget *hbox, *vbox;
	GtkWidget *table;

	item = gcnews_selected_item(gcnews);

	window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_widget_set_size_request(window, 320, 250);
	gtk_window_set_title(GTK_WINDOW(window), _("Post News (1.5+)"));
    gtk_container_set_border_width (GTK_CONTAINER(window), 5);

	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	table = gtk_table_new(3, 2, 0);

	replylbl = gtk_label_new("In Reply To Post #: ");
	inreplyto = gtk_entry_new();
	if(item) {
		char *buf = g_strdup_printf("%d", item->postid);
		
		gtk_entry_set_text(GTK_ENTRY(inreplyto), buf);
		g_free(buf);
	}

	gtk_table_attach(GTK_TABLE(table), replylbl, 0, 1, 0, 1, GTK_EXPAND|GTK_FILL, 0, 0, 0);
	gtk_table_attach(GTK_TABLE(table), inreplyto, 1, 2, 0, 1, GTK_EXPAND|GTK_FILL, 0, 0, 0);

	subjectlbl = gtk_label_new("Subject: ");
	subject = gtk_entry_new();

	if(item) {
		if(strncasecmp(item->subject, "re:", 3)) {
			char *buf = g_strdup_printf("Re: %s", item->subject);
			gtk_entry_set_text(GTK_ENTRY(subject), buf);
			g_free(buf);
		}
		else {
			gtk_entry_set_text(GTK_ENTRY(subject), item->subject);
		}
	}

	gtk_table_attach(GTK_TABLE(table), subjectlbl, 0, 1, 1, 2, GTK_EXPAND|GTK_FILL, 0, 0, 0);
	gtk_table_attach(GTK_TABLE(table), subject, 1, 2, 1, 2, GTK_EXPAND|GTK_FILL, 0, 0, 0);

	textlbl = gtk_label_new(_("Body: "));
	
	gtk_table_attach(GTK_TABLE(table), textlbl, 0, 1, 2, 3, GTK_EXPAND|GTK_FILL, 0, 0, 0);

	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	text = gtk_text_view_new();
	gtk_text_view_set_editable(GTK_TEXT_VIEW(text), TRUE);
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text), GTK_WRAP_WORD);
	{
		GtkWidget *text_scroll = gtk_scrolled_window_new(NULL, NULL);
		gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(text_scroll),
		                               GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
		gtk_container_add(GTK_CONTAINER(text_scroll), text);
		gtk_box_pack_start(GTK_BOX(hbox), text_scroll, 1, 1, 0);
	}

	gtk_box_pack_start(GTK_BOX(vbox), table, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(vbox), hbox, 0, 0, 10);

	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

	post = gtk_button_new_with_label(_("Post"));
	g_object_set_data(G_OBJECT(post), "text", text);
	g_object_set_data(G_OBJECT(post), "reply", inreplyto);
	g_object_set_data(G_OBJECT(post), "subject", subject);
	g_object_set_data(G_OBJECT(post), "window", window);
	g_signal_connect(post, "clicked", G_CALLBACK(news15_do_reply), gcnews);

	cancel = gtk_button_new_with_label(_("Cancel"));
	g_signal_connect(cancel, "clicked", G_CALLBACK(news15_cancel_post), 
					   window);

	gtk_box_pack_start(GTK_BOX(hbox), post, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(hbox), cancel, 0, 0, 0);

	gtk_box_pack_start(GTK_BOX(vbox), hbox, 0, 0, 0);

	gtk_container_add(GTK_CONTAINER(window), vbox);
	init_keyaccel(window);
	gtk_widget_show_all(window);
}

void news15_do_post(GtkWidget *btn, struct gnews_catalog *gcnews)
{
	GtkWidget *text = g_object_get_data(G_OBJECT(btn), "text");
	GtkWidget *subject = g_object_get_data(G_OBJECT(btn), "subject");
	GtkWidget *window = g_object_get_data(G_OBJECT(btn), "window");
	GtkTextBuffer *tbuf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text));
	GtkTextIter tstart, tend;
	char *textbuf;
	const char *subjectbuf = gtk_entry_get_text(GTK_ENTRY(subject));

	gtk_text_buffer_get_start_iter(tbuf, &tstart);
	gtk_text_buffer_get_end_iter(tbuf, &tend);
	textbuf = gtk_text_buffer_get_text(tbuf, &tstart, &tend, FALSE);

	hx_news15_post_thread(&the_session.htlc, gcnews->path, subjectbuf,
						  0, textbuf);
	g_free(textbuf);

	hx_news15_cat_list(&the_session.htlc, gcnews);
	gtk_widget_destroy(window);
}

void news15_post (GtkWidget *btn, struct gnews_catalog *gcnews)
{
	GtkWidget *window;
	GtkWidget *subject;
	GtkWidget *subjectlbl;
	GtkWidget *text;
	GtkWidget *textlbl;
	GtkWidget *vscroll;
	GtkWidget *post, *cancel;
	GtkWidget *hbox, *vbox;
	GtkWidget *table;

	window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_widget_set_size_request(window, 320, 250);
	gtk_window_set_title(GTK_WINDOW(window), _("Post News (1.5+)"));
    gtk_container_set_border_width (GTK_CONTAINER(window), 5);

	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	table = gtk_table_new(2, 2, 0);

	subjectlbl = gtk_label_new(_("Subject: "));
	subject = gtk_entry_new();

	gtk_table_attach(GTK_TABLE(table), subjectlbl, 0, 1, 0, 1, GTK_EXPAND|GTK_FILL, 0, 0, 0);
	gtk_table_attach(GTK_TABLE(table), subject, 1, 2, 0, 1, GTK_EXPAND|GTK_FILL, 0, 0, 0);

	textlbl = gtk_label_new(_("Body: "));
	
	gtk_table_attach(GTK_TABLE(table), textlbl, 0, 1, 1, 2, GTK_EXPAND|GTK_FILL, 0, 0, 0);

	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	text = gtk_text_view_new();
	gtk_text_view_set_editable(GTK_TEXT_VIEW(text), TRUE);
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text), GTK_WRAP_WORD);
	{
		GtkWidget *post_scroll = gtk_scrolled_window_new(NULL, NULL);
		gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(post_scroll),
		                               GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
		gtk_container_add(GTK_CONTAINER(post_scroll), text);
		gtk_box_pack_start(GTK_BOX(hbox), post_scroll, 1, 1, 0);
	}

	gtk_box_pack_start(GTK_BOX(vbox), table, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(vbox), hbox, 0, 0, 10);

	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

	post = gtk_button_new_with_label(_("Post"));
	g_object_set_data(G_OBJECT(post), "text", text);
	g_object_set_data(G_OBJECT(post), "subject", subject);
	g_object_set_data(G_OBJECT(post), "window", window);
	g_signal_connect(post, "clicked", G_CALLBACK(news15_do_post), gcnews);

	cancel = gtk_button_new_with_label(_("Cancel"));
	g_signal_connect(cancel, "clicked", G_CALLBACK(news15_cancel_post), 
					   window);

	gtk_box_pack_start(GTK_BOX(hbox), post, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(hbox), cancel, 0, 0, 0);

	gtk_box_pack_start(GTK_BOX(vbox), hbox, 0, 0, 0);

	gtk_container_add(GTK_CONTAINER(window), vbox);
	init_keyaccel(window);
	gtk_widget_show_all(window);
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
	GtkWidget *hbuttonbox1;
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
	GtkWidget *viewport1;
	GtkWidget *topframe;
	GdkBitmap *mask;
	GdkPixmap *icon;
	GtkWidget *pix;
	GtkStyle *style;

	gcnews->listing = 0;
	gcnews->prev = gcnews_list;
	gcnews->next = 0;
	gcnews->path = strdup(path);



	if(gcnews_list) {
		gcnews_list->next = gcnews;
	}

	news_window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
	gtk_window_set_resizable(GTK_WINDOW(news_window), TRUE);
	gtk_widget_realize(news_window);
	style = gtk_widget_get_style(news_window);
	gtk_widget_set_size_request(news_window, 570, 375); 
	gtk_window_set_title(GTK_WINDOW(news_window), path);
	g_object_set_data(G_OBJECT(news_window), "gcnews", gcnews);
	g_signal_connect(news_window, "delete_event", 
					   G_CALLBACK(destroy_gcnews_browser), 0);

	hpaned1 = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_container_add (GTK_CONTAINER (news_window), hpaned1);
	gtk_container_set_border_width (GTK_CONTAINER (hpaned1), 4);
	gtk_paned_set_position (GTK_PANED (hpaned1), 285);
	
	vbox1 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_paned_pack1 (GTK_PANED (hpaned1), vbox1, FALSE, TRUE);

	topframe = gtk_frame_new(0);
	gtk_widget_set_size_request(topframe, -1, 30);
	gtk_frame_set_shadow_type(GTK_FRAME(topframe), GTK_SHADOW_OUT);
	gtk_box_pack_start(GTK_BOX(vbox1), topframe, 0, 0, 0);
	
	hbuttonbox1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_container_add(GTK_CONTAINER(topframe), hbuttonbox1);
	
	reloadbtn =  gtk_button_new();
	g_signal_connect(reloadbtn, "clicked",
					   G_CALLBACK(gcnews_reload_btn), gcnews);
	gtk_widget_set_tooltip_text(reloadbtn, _("Reload"));
	icon = (GdkPixmap *)gdk_pixbuf_new_from_resource("/com/nasledov/gtkhx/pixmaps/refresh.xpm", NULL);
	pix = gtk_image_new_from_pixbuf((GdkPixbuf *)icon);
	gtk_container_add(GTK_CONTAINER(reloadbtn), pix);
	pix = 0, icon = 0, mask = 0;

	postbtn =  gtk_button_new();
	g_signal_connect(postbtn, "clicked",
					   G_CALLBACK(news15_post), gcnews);
	gtk_widget_set_tooltip_text(postbtn, _("Post Thread"));
	icon = (GdkPixmap *)gdk_pixbuf_new_from_resource("/com/nasledov/gtkhx/pixmaps/postnews.xpm", NULL);
	pix = gtk_image_new_from_pixbuf((GdkPixbuf *)icon);
	gtk_container_add(GTK_CONTAINER(postbtn), pix);
	pix = 0, icon = 0, mask = 0;
	
	replybtn = gtk_button_new_with_label ("[ R ]");
	g_signal_connect(replybtn, "clicked",
					   G_CALLBACK(news15_reply), gcnews);
	gtk_widget_set_tooltip_text(replybtn, _("Reply To Thread"));


	deletebtn =  gtk_button_new();
	g_signal_connect(deletebtn, "clicked",
					   G_CALLBACK(news15_delete), gcnews);
	gtk_widget_set_tooltip_text(deletebtn, _("Delete Thread"));
	icon = (GdkPixmap *)gdk_pixbuf_new_from_resource("/com/nasledov/gtkhx/pixmaps/trash.xpm", NULL);
	pix = gtk_image_new_from_pixbuf((GdkPixbuf *)icon);
	gtk_container_add(GTK_CONTAINER(deletebtn), pix);
	pix = 0, icon = 0, mask = 0;

	gtk_box_pack_start(GTK_BOX(hbuttonbox1), reloadbtn, 0, 0, 2);
	gtk_box_pack_start(GTK_BOX(hbuttonbox1), postbtn, 0, 0, 2);
	gtk_box_pack_start(GTK_BOX(hbuttonbox1), replybtn, 0, 0, 2);
	gtk_box_pack_start(GTK_BOX(hbuttonbox1), deletebtn, 0, 0, 2);

	/* Phase 3.9: dropped a GtkAlignment(0.5, 0.5, 1, 1) wrapper around
	 * the scrolled window. The xscale/yscale=1 made it expand to fill,
	 * which is what gtk_box_pack_start(TRUE, TRUE) already provides;
	 * GtkAlignment is gone in GTK 4 anyway. */
	scrolledwindow2 = gtk_scrolled_window_new (NULL, NULL);
	gtk_box_pack_start (GTK_BOX (vbox1), scrolledwindow2, TRUE, TRUE, 0);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolledwindow2), GTK_POLICY_AUTOMATIC, GTK_POLICY_ALWAYS);

	viewport1 = gtk_viewport_new (NULL, NULL);
	gtk_container_add (GTK_CONTAINER (scrolledwindow2), viewport1);
	
	/* Phase 2.8: GtkTreeView (in tree mode) replaces GtkCTree.
	 * news_store holds the model; news_tree is the view. */
	gcnews->news_store = gtk_tree_store_new(NEWS_N_COLS,
											G_TYPE_STRING,
											G_TYPE_POINTER);
	news_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(gcnews->news_store));
	g_object_unref(gcnews->news_store);  /* the view holds the only ref now */
	gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(news_tree), FALSE);
	{
		GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
		GtkTreeViewColumn *col;
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
	gtk_container_add (GTK_CONTAINER (viewport1), news_tree);

	vbox2 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_paned_pack2 (GTK_PANED (hpaned1), vbox2, TRUE, TRUE);
	
	authorlbl = gtk_label_new (_("Author: "));
	gtk_label_set_justify(GTK_LABEL(authorlbl), GTK_JUSTIFY_LEFT);
	gtk_box_pack_start (GTK_BOX (vbox2), authorlbl, 0, 1, 0);
	
	datelbl = gtk_label_new (_("Date: "));
	gtk_label_set_justify(GTK_LABEL(datelbl), GTK_JUSTIFY_LEFT);
	gtk_box_pack_start (GTK_BOX (vbox2), datelbl, 0, 1, 0);
	
	subjectlbl = gtk_label_new (_("Subject: "));
	gtk_label_set_justify(GTK_LABEL(subjectlbl), GTK_JUSTIFY_LEFT);
	gtk_box_pack_start (GTK_BOX (vbox2), subjectlbl, 0, 1, 0);
	
	scrolledwindow1 = gtk_scrolled_window_new (NULL, NULL);
	gtk_box_pack_start (GTK_BOX (vbox2), scrolledwindow1, TRUE, TRUE, 0);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolledwindow1), GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
	
	news_text = gtk_text_view_new ();
	gtk_text_view_set_editable (GTK_TEXT_VIEW (news_text), FALSE);
	gtk_text_view_set_cursor_visible (GTK_TEXT_VIEW (news_text), FALSE);
	gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (news_text), GTK_WRAP_WORD);
	gtk_container_add (GTK_CONTAINER (scrolledwindow1), news_text);
	gtk_widget_show_all(news_window);

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
		
	gtk_editable_delete_text(GTK_EDITABLE(gcnews->news_text), 0, -1);

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
		gtk_text_buffer_set_text(buf, post->buf, strlen(post->buf));
	}
}

void open_news15(GtkWidget *widget, session *sess)
{
	
	struct gnews_folder *gfnews = create_gfnews_window(NULL);

	hx_news15_fldr_list(&the_session.htlc, gfnews);
}
