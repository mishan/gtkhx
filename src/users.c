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
#include <ctype.h>
#include "hx.h"
#include "gtk_hlist.h"
#include "cicn.h"
#include "network.h"
#include "chat.h"
#include "gtkhx.h"
#include "msg.h"
#include "gtkutil.h"
#include "tasks.h"
#include "rcv.h"
#include "pixmaps/msg.xpm"
#include "pixmaps/chat.xpm"
#include "pixmaps/info.xpm"
#include "pixmaps/kick.xpm"
#include "pixmaps/ban.xpm"
#include "pixmaps/ignore.xpm"

static int user_storow, user_stocolumn;

#define COL_UID 0
#define COL_USERNAME 1
static int user_click_col;

GtkStyle *users_style;

GdkFont *users_font;

GdkColor user_colors[8];
GdkColor gdk_user_colors[4];

GtkWidget *msgbtn, *kickbtn, *infobtn, *banbtn, *chatbtn, *ignobtn;
GdkGC *users_gc;
GdkGC *mask_gc;

void hx_change_name_icon (struct htlc_conn *htlc)
{
	guint16 icon16 = htons(htlc->icon);
	
	hlwrite(htlc, HTLC_HDR_USER_CHANGE, 0, 2,
			HTLC_DATA_ICON, 2, &icon16,
			HTLC_DATA_NAME, strlen(htlc->name), htlc->name);
}

void hx_kick_user (struct htlc_conn *htlc, guint16 uid, guint16 ban)
{
	uid = htons(uid);
	task_new(htlc, rcv_task_kick, 0, 0, "kick");
	if (ban) {
		ban = htons(ban);
		hlwrite(htlc, HTLC_HDR_USER_KICK, 0, 2,
				HTLC_DATA_BAN, 2, &ban,
				HTLC_DATA_UID, 2, &uid);
	} else {
		hlwrite(htlc, HTLC_HDR_USER_KICK, 0, 1,
				HTLC_DATA_UID, 2, &uid);
	}
}

void hx_get_user_info (struct htlc_conn *htlc, guint16 uid)
{
	guint16 *_uid = g_malloc(sizeof(guint16));
	*_uid = uid;
	
	task_new(htlc, rcv_task_user_info, (void *)_uid, 0, "info");
	uid = htons(uid);
	hlwrite(htlc, HTLC_HDR_USER_GETINFO, 0, 1,
			HTLC_DATA_UID, 2, &uid);
}


struct hx_user * hx_user_new (struct hx_user **utailp)
{
	struct hx_user *user, *tail = *utailp;

	user = g_malloc0(sizeof(struct hx_user));

	user->next = 0;
	user->prev = tail;
	tail->next = user;
	tail = user;
	*utailp = tail;

	return user;
}

void
hx_user_delete (struct hx_user **utailp, struct hx_user *user)
{
	if (user->next)
		user->next->prev = user->prev;
	if (user->prev)
		user->prev->next = user->next;
	if (*utailp == user)
		*utailp = user->prev;
	g_free(user);
}

struct hx_user * hx_user_with_uid (struct hx_user *ulist, guint16 uid)
{
	struct hx_user *userp;

	for (userp = ulist->next; userp; userp = userp->next)
		if (userp->uid == uid)
			return userp;


	return 0;
}

struct hx_user *hx_user_with_name(struct hx_user *ulist, char *name)
{
	struct hx_user *user;

	for(user = ulist; user; user = user->next) {
		if(strcmp(user->name, name) == 0) {
			return user;
		}
	}

	return 0;
}

static GtkWidget *menu_quick_sub (char *name, GtkWidget * menu)
{
	GtkWidget *sub_menu;
 	GtkWidget *sub_item;

	if (!name)
	 	return menu;

 	sub_menu = gtk_menu_new ();
 	sub_item = gtk_menu_item_new_with_label (name);
 	gtk_menu_append (GTK_MENU (menu), sub_item);
 	gtk_widget_show (sub_item);
 	gtk_menu_item_set_submenu (GTK_MENU_ITEM (sub_item), sub_menu);

	return (sub_menu);
}

static void menu_popup (GtkWidget *menu, GdkEventButton *event)
{
	gtk_menu_popup (GTK_MENU (menu), NULL, NULL, NULL, NULL,
						 event->button, event->time);
	gtk_widget_show (menu);
}

static GtkWidget * menu_quick_item ( char *label, GtkWidget * menu,
									 int sensitive, gpointer userdata)
{
	GtkWidget *item;
	if (!label)
		item = gtk_menu_item_new ();
	else
		item = gtk_menu_item_new_with_label (label);
	gtk_menu_append (GTK_MENU (menu), item);
	gtk_object_set_user_data (GTK_OBJECT (item), userdata);

	if (!sensitive)
		gtk_widget_set_sensitive (GTK_WIDGET (item), FALSE);

	gtk_widget_show (item);

	return item;
}

void
menu_quick_item_with_callback (session *sess, void *callback, char *label,
							   GtkWidget * menu, void *arg)
{
	GtkWidget *item;

	item = gtk_menu_item_new_with_label (label);
	gtk_menu_append (GTK_MENU (menu), item);
	gtk_signal_connect (GTK_OBJECT (item), "activate",
							  GTK_SIGNAL_FUNC (callback), arg);
	gtk_object_set_data(GTK_OBJECT(item), "sess", sess);
	gtk_widget_show (item);
}

static void user_kick_menu (GtkWidget *menu, struct hx_user *user)
{
	session *sess = gtk_object_get_data(GTK_OBJECT(menu), "sess");

	if (!user)
		return;
	hx_kick_user(&sess->htlc, user->uid, 0);
}

static void user_ban_menu (GtkWidget *menu, struct hx_user *user)
{
	session *sess = gtk_object_get_data(GTK_OBJECT(menu), "sess");

	if (!user)
		return;
	hx_kick_user(&sess->htlc, user->uid, 1);
}

static void user_igno_menu (GtkWidget *menu, struct hx_user *user)
{
	session *sess = gtk_object_get_data(GTK_OBJECT(menu), "sess");

	if(!user) {
		return;
	}

	user->ignore = 1;
	hx_printf_prefix(&sess->htlc, 0, INFOPREFIX, _("ignore: %s is now ignored\n"),
					 user->name);
}

static void user_unigno_menu (GtkWidget *menu, struct hx_user *user)
{
	session *sess = gtk_object_get_data(GTK_OBJECT(menu), "sess");

	if(!user) {
		return;
	}

	user->ignore = 0;
	hx_printf_prefix(&sess->htlc, 0, INFOPREFIX, _("ignore: %s is now unignored\n"),
					 user->name);
}

static void user_info_menu(GtkWidget *menu, struct hx_user *user)
{
	session *sess = gtk_object_get_data(GTK_OBJECT(menu), "sess");

	if (!user)
		return;
	hx_get_user_info(&sess->htlc, user->uid);
}

static void user_msg_menu(GtkWidget *menu, struct hx_user *user)
{
	struct msgwin *msg;

	if (!user)
		return;

	if((msg = msgwin_with_uid(user->uid))) {
		gdk_window_raise(msg->window->window);
	}
	else {
		create_msgwin(user->uid, user->name);
	}
}

static void prompt_chat (session *sess, guint16 uid);

static void user_pchat_menu(GtkWidget *menu, struct hx_user *user)
{
	int with_cid = 0;
	struct gtkhx_chat *gchat;
	session *sess = gtk_object_get_data(GTK_OBJECT(menu), "sess");


	if(!user)
		return;
	for(gchat = sess->gchat_list; gchat; gchat = gchat->prev) {
		if(gchat->cid) {
			with_cid = 1;
		}
	}
	if(!with_cid)
		hx_chat_user(&sess->htlc, user->uid);
	else
		prompt_chat(sess, user->uid);
}

static void user_popup(struct hx_user *user, GdkEventButton *event,
					   session *sess)
{

	GtkWidget *menu = gtk_menu_new ();
	GtkWidget *sub;
	char buf[128];

	sub = menu_quick_sub(user->name, menu);
	gtk_object_set_data(GTK_OBJECT(menu), "sess", sess);

	sprintf(buf, _("Icon: %d"), user->icon);
	menu_quick_item(buf, sub, 1, 0);
	sprintf(buf, _("UID: %d"), user->uid);
	menu_quick_item(buf, sub, 1, 0);
	sprintf(buf, _("Status: %s%s"), user->color >= 2 ? _("Admin") :
			_("Guest"), user->color % 2 ? _(" (Away)") : "");
	menu_quick_item(buf, sub, 1, 0);

	menu_quick_item(0, menu, 0, 0);

	menu_quick_item_with_callback(sess, user_kick_menu, _("Kick"), menu, user);
	menu_quick_item_with_callback(sess, user_ban_menu, _("Ban"), menu, user);
	menu_quick_item_with_callback(sess, user_igno_menu, _("Ignore"),
								  menu, user);
	menu_quick_item_with_callback(sess, user_unigno_menu, _("UnIgnore"),
								  menu, user);
	menu_quick_item_with_callback(sess, user_info_menu, _("Get User Info"),
								  menu, user);
	menu_quick_item_with_callback(sess, user_msg_menu, _("Private Message"),
								  menu, user);
	menu_quick_item_with_callback(sess, user_pchat_menu, _("Private Chat"),
								  menu, user);

	menu_popup (menu, event);
}

int users_sort(GtkCList *clist, const GtkHListRow *ptr1,
			   const GtkHListRow *ptr2)
{
	struct hx_user *usr1 = ptr1->data;
	struct hx_user *usr2 = ptr2->data;

	if(user_click_col == COL_UID) {
		if(usr1->uid < usr2->uid)
			return -1;
		else
			return 1;
	}
	else {
		int len1 = strlen(usr1->name), len2 = strlen(usr2->name);
		int len = len1 < len2 ? len1 : len2;
		int i;

		for(i = 0; i < len; i++) {
			if(tolower(usr1->name[i]) < tolower(usr2->name[i]))
				return -1;
			else if(tolower(usr2->name[i]) < tolower(usr1->name[i]))
				return 1;
		}

		return 0;
	}

	return 0;
}

void usercol_clicked(GtkWidget *clist, gint col, gpointer data)
{
	user_click_col = col;
	gtk_hlist_sort(GTK_HLIST(clist));
}

void user_clicked (GtkWidget *widget, GdkEventButton *event, gpointer data)
{
	int row;
	int column;
	session *sess = data;

	gtk_hlist_get_selection_info(GTK_HLIST(widget),
				     event->x, event->y, &row, &column);
	if(event->button == 1) {
		if (event->type == GDK_2BUTTON_PRESS) {
			struct hx_user *user;


			user = gtk_hlist_get_row_data(GTK_HLIST(widget), row);
			if (user) {
				struct msgwin *msg = NULL;

				if((msg = msgwin_with_uid(user->uid))) {
					gdk_window_raise(msg->window->window);
				}
				else {
					create_msgwin(user->uid, user->name);
				}
			}
		}
		else {
			user_storow = row;
			user_stocolumn = column;
		}
	}
	else if(event->button == 3) {
		struct hx_user *user;


		user = gtk_hlist_get_row_data(GTK_HLIST(widget), row);
		if(user) {
			gtk_hlist_select_row(GTK_HLIST(widget), row, 0);
			user_popup(user, event, sess);
		}
	}
}

void open_message_btn (GtkWidget *widget, gpointer data)
{
	struct hx_user *user;
	GtkWidget *users_list = data;

	if (!users_list)
		return;
	user = gtk_hlist_get_row_data(GTK_HLIST(users_list), user_storow);
	if (!user)
		return;
	create_msgwin(user->uid, user->name);
}

void user_info_btn (GtkWidget *widget, gpointer data)
{
	struct hx_user *user;
	GtkWidget *users_list = data;
	session *sess = gtk_object_get_data(GTK_OBJECT(widget), "sess");

	if (!users_list)
		return;
	user = gtk_hlist_get_row_data(GTK_HLIST(users_list), user_storow);
	if (!user)
		return;

	hx_get_user_info(&sess->htlc, user->uid);
}

void user_kick_btn (GtkWidget *widget, gpointer data)
{
	struct hx_user *user;
	GtkWidget *users_list = data;
	session *sess = gtk_object_get_data(GTK_OBJECT(widget), "sess");

	if (!users_list)
		return;
	user = gtk_hlist_get_row_data(GTK_HLIST(users_list), user_storow);
	if (!user)
		return;
	hx_kick_user(&sess->htlc, user->uid, 0);
}

void user_igno_btn (GtkWidget *widget, gpointer data)
{
	struct hx_user *user;
	GtkWidget *users_list = data;
	session *sess = gtk_object_get_data(GTK_OBJECT(widget), "sess");

	if(!users_list) {
		return;
	}

	user = gtk_hlist_get_row_data(GTK_HLIST(users_list), user_storow);
	if(!user) {
		return;
	}

	user->ignore ^= 1;
	hx_printf_prefix(&sess->htlc, 0, INFOPREFIX, user->ignore ? _("ignore: %s is now ignored\n") : _("ignore: %s is now unignored"), user->name);
}

void user_ban_btn (GtkWidget *widget, gpointer data)
{
	struct hx_user *user;
	GtkWidget *users_list = data;
	session *sess = gtk_object_get_data(GTK_OBJECT(widget), "sess");

	if(!users_list)
		return;
	user = gtk_hlist_get_row_data(GTK_HLIST(users_list), user_storow);
	if(!user)
		return;
	hx_kick_user(&sess->htlc, user->uid, 1);
}


static void invite_u_to_chat(GtkWidget *widget, gpointer data)
{
	GtkWidget *dialog = (GtkWidget *)gtk_object_get_data(GTK_OBJECT(widget),
														 "dialog");
	GtkWidget *list = (GtkWidget *)gtk_object_get_data(GTK_OBJECT(widget),
													   "list");
	session *sess = gtk_object_get_data(GTK_OBJECT(widget), "sess");
	guint32 *cid = gtk_object_get_data(GTK_OBJECT(widget), "cid");
	char *titles[2] = {};

	gtk_clist_get_text(GTK_CLIST(list), *cid, 0, titles);

	hx_invite_user(&sess->htlc, GPOINTER_TO_INT(data), atou32(titles[0]));
	gtk_widget_destroy(dialog);
	g_free(cid);
}

static void create_new_chat(GtkWidget *widget, gpointer data)
{
	GtkWidget *dialog = (GtkWidget *)gtk_object_get_data(GTK_OBJECT(widget),
														 "dialog");
	session *sess = gtk_object_get_data(GTK_OBJECT(widget), "sess");
	guint32 *cid = gtk_object_get_data(GTK_OBJECT(widget), "cid");
	guint16 *uid = data;

	g_free(cid);
	hx_chat_user(&sess->htlc, *uid);
	gtk_widget_destroy(dialog);
}

void select_cid(GtkWidget *widget, gint row, gint col, GdkEventButton *event,
				gpointer data)
{
	guint32 *cid = data; *cid = row;
}

static void prompt_chat(session *sess, guint16 _uid)
{
	GtkWidget *dialog;
	GtkWidget *invite;
	GtkWidget *cancel;
	GtkWidget *btnhbox;
	GtkWidget *new;
	GtkWidget *list;
	GtkWidget *scroll;
	char *titles[] = {"CID", "Subject"};
	char *entry[2];
	struct gtkhx_chat *gchat = 0;
	guint32 *cid = g_malloc(sizeof(guint32));
	guint16 *uid = g_malloc(sizeof(guint16));
	
	*uid = _uid;

	dialog = gtk_dialog_new();
	gtk_window_set_title(GTK_WINDOW(dialog), _("Private Chat Invitation"));
	gtk_container_border_width (GTK_CONTAINER(dialog), 5);

	invite = gtk_button_new_with_label(_("Invite"));
	gtk_object_set_data(GTK_OBJECT(invite), "cid", cid);

	gtk_object_set_data(GTK_OBJECT(invite), "dialog", dialog);

	gtk_object_set_data(GTK_OBJECT(invite), "sess", sess);
	GTK_WIDGET_SET_FLAGS(invite, GTK_CAN_DEFAULT);

	cancel = gtk_button_new_with_label(_("Cancel"));
	gtk_signal_connect_object(GTK_OBJECT(cancel), "clicked",
							  (GtkSignalFunc)gtk_widget_destroy,
							  GTK_OBJECT(dialog));

	new = gtk_button_new_with_label(_("New"));
	gtk_signal_connect(GTK_OBJECT(new), "clicked",
					   GTK_SIGNAL_FUNC(create_new_chat), uid);
	gtk_object_set_data(GTK_OBJECT(new), "cid", cid);
	gtk_object_set_data(GTK_OBJECT(new), "dialog", dialog);
	gtk_object_set_data(GTK_OBJECT(new), "sess", sess);

	btnhbox = gtk_hbox_new(0,0);


	scroll = gtk_scrolled_window_new(0, 0);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
				       GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);

	list = gtk_clist_new_with_titles(2, titles);
	gtk_clist_set_selection_mode(GTK_CLIST(list), GTK_SELECTION_EXTENDED);
	gtk_clist_set_column_width (GTK_CLIST (list), 0, 80);
	gtk_signal_connect(GTK_OBJECT(list), "select_row",
					   GTK_SIGNAL_FUNC(select_cid), cid);

	gtk_container_add(GTK_CONTAINER(scroll), list);
	gtk_widget_set_usize(list, 350, 200);

	for(gchat = sess->gchat_list; gchat; gchat = gchat->prev) {
		if(!gchat->cid)
			continue;

		entry[0] = g_strdup_printf("0x%08x", gchat->chat->cid);
		entry[1] = gchat->chat->subject;


		gtk_clist_append(GTK_CLIST(list), entry);
	}
	gtk_object_set_data(GTK_OBJECT(invite), "list", list);
	gtk_signal_connect(GTK_OBJECT(invite), "clicked",
					   GTK_SIGNAL_FUNC(invite_u_to_chat), GINT_TO_POINTER(uid));

	gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dialog)->vbox), scroll, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dialog)->action_area), btnhbox, 0, 0,
					   0);
	gtk_box_pack_start(GTK_BOX(btnhbox), invite, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(btnhbox), new, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(btnhbox), cancel, 0, 0, 0);
	gtk_widget_grab_default(invite);
	gtk_widget_show_all(dialog);
}


void user_chat_btn(GtkWidget *widget, gpointer data)
{
	struct hx_user *user;
	int with_cid = 0;
	struct gtkhx_chat *gchat;
	GtkWidget *users_list = data;
	session *sess = gtk_object_get_data(GTK_OBJECT(widget), "sess");

	if(!users_list)
		return;

	user = gtk_hlist_get_row_data(GTK_HLIST(users_list), user_storow);

	if(!user)
		return;
	for(gchat = sess->gchat_list; gchat; gchat = gchat->prev) {
		if(gchat->cid) {
			with_cid = 1;
		}
	}
	if(!with_cid)
		hx_chat_user(&sess->htlc, user->uid);
	else
		prompt_chat(sess, user->uid);
}



static void close_users_window (GtkWidget *widget, gpointer data)
{
	session *sess = data;

	sess->users_window = 0;
	sess->users_list = 0;
	gtk_widget_destroy(widget);
	gtkhx_prefs.geo.users.open = 0;
	gtkhx_prefs.geo.users.init = 0;
}

static void users_move(GtkWidget *w, GdkEventConfigure *e, gpointer data)
{
	int x, y, width, height;
	session *sess = data;

	gdk_window_get_root_origin(sess->users_window->window, &x, &y);
	gdk_window_get_size(sess->users_window->window, &width, &height);
	if(e->send_event) { /* Is a position event */
		gtkhx_prefs.geo.users.xpos = x;
		gtkhx_prefs.geo.users.ypos = y;
	} else { /* Is a size event */
		gtkhx_prefs.geo.users.xsize = width;
		gtkhx_prefs.geo.users.ysize = height;
	}
}

void user_list (session *sess)
{
	struct hx_user *user;

	if (!sess->users_window)
		return;

	gtk_hlist_freeze(GTK_HLIST(sess->users_list));
	gtk_hlist_clear(GTK_HLIST(sess->users_list));
	for (user = sess->chat_front->user_list->next; user; user = user->next) {
		hx_output.user_create(&sess->htlc, sess->chat_front, user, user->name,
							  user->icon, user->color);
	}
	gtk_hlist_thaw(GTK_HLIST(sess->users_list));
}

void create_users_window (GtkWidget *widget, gpointer data)
{
	GtkWidget *users_window_scroll;
	GtkWidget *vbox;
	GtkWidget *hbuttonbox, *topframe;
	GtkStyle *style;
	GdkBitmap *mask;
	GtkWidget *pix;
	GdkPixmap *icon;
	GtkWidget *users_list;
	GtkWidget *users_window;
	gchar *titles[2];
	session *sess = data;
	titles[0] = _("UID");
	titles[1] = _("Name");

	if (gtkhx_prefs.geo.users.open) {
		gdk_window_raise(sess->users_window->window);
		return;
	}

	users_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_wmclass(GTK_WINDOW(users_window), "users", "GtkHx");
	gtk_widget_realize(users_window);
	style = gtk_widget_get_style(users_window);
	gtk_window_set_title(GTK_WINDOW(users_window), _("Users"));
	gtk_widget_set_usize(users_window, 264, 400);
	gtk_window_set_policy(GTK_WINDOW(users_window), 1, 1, 0);
	gtk_signal_connect(GTK_OBJECT(users_window), "delete_event",
			   GTK_SIGNAL_FUNC(close_users_window), sess);


	users_list = gtk_hlist_new_with_titles(2, titles);
	gtk_hlist_set_column_width(GTK_HLIST(users_list), 0, 35);
	gtk_hlist_set_column_width(GTK_HLIST(users_list), 1, 240);
	gtk_hlist_set_row_height(GTK_HLIST(users_list), 18);
	gtk_hlist_set_shadow_type(GTK_HLIST(users_list), GTK_SHADOW_NONE);
	gtk_hlist_set_column_justification(GTK_HLIST(users_list), 1,
									   GTK_JUSTIFY_LEFT);
	gtk_hlist_set_compare_func(GTK_HLIST(users_list),
							   (GtkHListCompareFunc) users_sort);
	gtk_signal_connect(GTK_OBJECT(users_list), "click_column",
					   GTK_SIGNAL_FUNC(usercol_clicked), sess);
	gtk_signal_connect(GTK_OBJECT(users_list), "button_press_event",
					   GTK_SIGNAL_FUNC(user_clicked), sess);

	if (!users_style) {
		if (!users_font)
			users_font = gdk_font_load("-adobe-helvetica-normal-r-*-*-10-140-*\
-*-*-*-*-*");
		if (users_font) {
			users_style = gtk_style_new();
			users_style->font = users_font;
		}
	}
	if (users_style)
		gtk_widget_set_style(users_list, users_style);

	users_window_scroll = gtk_scrolled_window_new(0, 0);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(users_window_scroll),
				       GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
	gtk_widget_set_usize(users_window_scroll, 240, 400);
	gtk_container_add(GTK_CONTAINER(users_window_scroll), users_list);

	msgbtn = gtk_button_new();
	gtk_object_set_data(GTK_OBJECT(msgbtn), "sess", sess);
	icon = gdk_pixmap_create_from_xpm_d (users_window->window, &mask,
										 &style->bg[GTK_STATE_NORMAL], msg_xpm);
	pix = gtk_pixmap_new(icon, mask);
	gtk_container_add(GTK_CONTAINER(msgbtn), pix);
	gtk_signal_connect(GTK_OBJECT(msgbtn), "clicked",
					   GTK_SIGNAL_FUNC(open_message_btn), users_list);
	gtk_tooltips_set_tip(tooltips, msgbtn, _("Msg"), 0);
	icon = 0, pix = 0, mask = 0;

	kickbtn = gtk_button_new();
	gtk_object_set_data(GTK_OBJECT(kickbtn), "sess", sess);
	icon = gdk_pixmap_create_from_xpm_d (users_window->window, &mask,
										 &style->bg[GTK_STATE_NORMAL],
										 kick_xpm);
    pix = gtk_pixmap_new(icon, mask);
	gtk_container_add(GTK_CONTAINER(kickbtn), pix);
	gtk_signal_connect(GTK_OBJECT(kickbtn), "clicked",
					   GTK_SIGNAL_FUNC(user_kick_btn), users_list);
	gtk_tooltips_set_tip(tooltips, kickbtn, _("Kick"), 0);
	icon = 0, pix = 0, mask = 0;

	infobtn = gtk_button_new();
	gtk_object_set_data(GTK_OBJECT(infobtn), "sess", sess);
	icon = gdk_pixmap_create_from_xpm_d (users_window->window, &mask,
										 &style->bg[GTK_STATE_NORMAL],
										 info_xpm);
    pix = gtk_pixmap_new(icon, mask);
	gtk_container_add(GTK_CONTAINER(infobtn), pix);
	gtk_signal_connect(GTK_OBJECT(infobtn), "clicked",
					   GTK_SIGNAL_FUNC(user_info_btn), users_list);
	gtk_tooltips_set_tip(tooltips, infobtn, _("User Info"), 0);
	icon = 0, pix = 0, mask = 0;

	banbtn = gtk_button_new();
	gtk_object_set_data(GTK_OBJECT(banbtn), "sess", sess);
	gtk_signal_connect(GTK_OBJECT(banbtn), "clicked",
					   GTK_SIGNAL_FUNC(user_ban_btn), users_list);
	icon = gdk_pixmap_create_from_xpm_d(users_window->window, &mask,
										&style->bg[GTK_STATE_NORMAL], ban_xpm);
	pix = gtk_pixmap_new(icon, mask);
	gtk_container_add(GTK_CONTAINER(banbtn), pix);
	gtk_tooltips_set_tip(tooltips, banbtn, _("Ban"), 0);
	icon = 0, pix = 0, mask = 0;

	chatbtn = gtk_button_new();
	gtk_object_set_data(GTK_OBJECT(chatbtn), "sess", sess);
	gtk_tooltips_set_tip(tooltips, chatbtn, _("Private Chat"), 0);
	gtk_signal_connect(GTK_OBJECT(chatbtn), "clicked",
					   GTK_SIGNAL_FUNC(user_chat_btn), users_list);
	icon = gdk_pixmap_create_from_xpm_d (users_window->window, &mask,
										 &style->bg[GTK_STATE_NORMAL],
										 chat_xpm);
    pix = gtk_pixmap_new(icon, mask);
	gtk_container_add(GTK_CONTAINER(chatbtn), pix);
	icon = 0, pix = 0, mask = 0;

	ignobtn = gtk_button_new();
	gtk_object_set_data(GTK_OBJECT(ignobtn), "sess", sess);
	gtk_tooltips_set_tip(tooltips, ignobtn, _("Ignore"), 0);
	gtk_signal_connect(GTK_OBJECT(ignobtn), "clicked",
					   GTK_SIGNAL_FUNC(user_igno_btn), users_list);
	icon = gdk_pixmap_create_from_xpm_d(users_window->window, &mask,
										&style->bg[GTK_STATE_NORMAL],
										ignore_xpm);
	pix = gtk_pixmap_new(icon, mask);
	gtk_container_add(GTK_CONTAINER(ignobtn), pix);

	gtk_widget_set_sensitive(msgbtn, FALSE);
	gtk_widget_set_sensitive(kickbtn, FALSE);
	gtk_widget_set_sensitive(infobtn, FALSE);
	gtk_widget_set_sensitive(banbtn, FALSE);
	gtk_widget_set_sensitive(chatbtn, FALSE);
	gtk_widget_set_sensitive(ignobtn, FALSE);

	vbox = gtk_vbox_new(0, 0);
	gtk_widget_set_usize(vbox, 240, 400);

	topframe = gtk_frame_new(0);
	gtk_box_pack_start(GTK_BOX(vbox), topframe, 0, 0, 0);
	gtk_widget_set_usize(topframe, -2, 30);
	gtk_frame_set_shadow_type(GTK_FRAME(topframe), GTK_SHADOW_OUT);

	hbuttonbox = gtk_hbox_new(0,0);
	gtk_container_add(GTK_CONTAINER(topframe), hbuttonbox);

	gtk_box_pack_start(GTK_BOX(hbuttonbox), msgbtn, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(hbuttonbox), chatbtn, 0, 0, 2);
	gtk_box_pack_start(GTK_BOX(hbuttonbox), infobtn, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(hbuttonbox), kickbtn, 0, 0, 2);
	gtk_box_pack_start(GTK_BOX(hbuttonbox), banbtn, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(hbuttonbox), ignobtn, 0, 0, 2);

	gtk_box_pack_start(GTK_BOX(vbox), users_window_scroll, 1, 1, 0);

	gtk_container_add(GTK_CONTAINER(users_window), vbox);

	init_keyaccel(users_window);

	gtk_signal_connect(GTK_OBJECT(users_window), "configure_event",
					   GTK_SIGNAL_FUNC(users_move), sess);
	gtk_widget_set_usize(users_window, gtkhx_prefs.geo.users.xsize,
						 gtkhx_prefs.geo.users.ysize);
	gtk_widget_set_uposition(users_window, gtkhx_prefs.geo.users.xpos,
							 gtkhx_prefs.geo.users.ypos);
	gtk_widget_show_all(users_window);

	gtkhx_prefs.geo.users.open = 1;
	gtkhx_prefs.geo.users.init = 1;

	sess->users_list = users_list;
	sess->users_window = users_window;

	if(connected == 1) {
		changetitlespecific(users_window, _("Users"));
		gtk_widget_set_sensitive(msgbtn, TRUE);
		gtk_widget_set_sensitive(banbtn, TRUE);
		gtk_widget_set_sensitive(infobtn, TRUE);
		gtk_widget_set_sensitive(kickbtn, TRUE);
		gtk_widget_set_sensitive(chatbtn, TRUE);
		gtk_widget_set_sensitive(ignobtn, TRUE);
		user_list(sess);
	}


}


void users_clear (struct htlc_conn *htlc, struct chat *chat)
{
	session *sess = sess_from_htlc(htlc);

	if (!sess->users_list || !gtkhx_prefs.geo.users.open)
		return;

	gtk_hlist_clear(GTK_HLIST(sess->users_list));
}

#define colorgdk(color) (&gdk_user_colors[color % 4])

void user_create (struct htlc_conn *htlc, struct chat *chat,
				  struct hx_user *user, const char *nam, guint16 icon,
				  guint16 color)
{
	GdkPixmap *pixmap;
	GdkBitmap *mask;
	gint row;
	session *sess = sess_from_htlc(htlc);
	GtkWidget *losers_list = gtkhx_prefs.geo.users.open ?sess->users_list : 0;
	gchar *nulls[2];
	struct gtkhx_chat *gchat;

	if(chat->cid) {
		gchat = gchat_with_cid(sess, chat->cid);
		if(!gchat)
			gchat = create_pchat_window(htlc, chat);
		losers_list = gchat->userlist;
	}


	if(!losers_list)
		return;

	nulls[0] = g_strdup_printf("%u", user->uid);
	nulls[1] = 0;
	row = gtk_hlist_append(GTK_HLIST(losers_list), nulls);
	gtk_hlist_set_row_data(GTK_HLIST(losers_list), row, user);
	g_free(nulls[0]);
	gtk_hlist_set_foreground(GTK_HLIST(losers_list), row, colorgdk(color));
	load_icon(losers_list, icon, &icon_files, 1, &pixmap, &mask);
	if (!pixmap)
		gtk_hlist_set_text(GTK_HLIST(losers_list), row, 1, nam);
	else
		gtk_hlist_set_pixtext(GTK_HLIST(losers_list), row, 1, nam, 34, pixmap,
							  mask);
}

void user_delete (struct htlc_conn *htlc, struct chat *chat,
				  struct hx_user *user)
{
	gint row;
	struct gtkhx_chat *gchat;
	session *sess = sess_from_htlc(htlc);
	GtkWidget *losers_list = gtkhx_prefs.geo.users.open ? sess->users_list : 0;

	if(chat->cid) {
		gchat = gchat_with_cid(sess, chat->cid);
		if(!gchat)
			return;
		losers_list = gchat->userlist;
	}

	if (!losers_list)
		return;

	row = gtk_hlist_find_row_from_data(GTK_HLIST(losers_list), user);
	gtk_hlist_remove(GTK_HLIST(losers_list), row);
}



void user_change (struct htlc_conn *htlc, struct chat *chat,
				  struct hx_user *user, const char *nam, guint16 icon,
				  guint16 color)
{
	GdkPixmap *pixmap;
	GdkBitmap *mask;
	gint row;
	gchar *rowdat[2];
	struct gtkhx_chat *gchat;
	session *sess = sess_from_htlc(htlc);
	GtkWidget *losers_list = gtkhx_prefs.geo.users.open ? sess->users_list : 0;
	struct chat *cchat;

	if(chat->cid) {
		gchat = gchat_with_cid(sess, chat->cid);
		if(!gchat)
			gchat = create_pchat_window(&sess->htlc, chat);
		losers_list = gchat->userlist;
	}


	if (!losers_list)
		return;

	if(!chat->cid) {
		for(gchat = sess->gchat_list; gchat; gchat = gchat->prev) {
			if(gchat->cid) {
				struct hx_user *u;
				cchat = chat_with_cid(sess, gchat->cid);
				u = hx_user_with_uid(gchat->chat->user_list, user->uid);
				if(u) {
					user_change(&sess->htlc, gchat->chat, u, nam, icon, color);
				}
			}
		}
	}



	rowdat[0] = g_strdup_printf("%u", user->uid);
	rowdat[1] = 0;
	row = gtk_hlist_find_row_from_data(GTK_HLIST(losers_list), user);
	gtk_hlist_remove(GTK_HLIST(losers_list), row);
	gtk_hlist_insert(GTK_HLIST(losers_list), row, rowdat);
	g_free(rowdat[0]);
	gtk_hlist_set_row_data(GTK_HLIST(losers_list), row, user);
	gtk_hlist_set_foreground(GTK_HLIST(losers_list), row, colorgdk(color));
	load_icon(sess->users_window, icon, &icon_files, 1, &pixmap, &mask);
	if (!pixmap)
		gtk_hlist_set_text(GTK_HLIST(losers_list), row, 1, nam);
	else
		gtk_hlist_set_pixtext(GTK_HLIST(losers_list), row, 1, nam, 34, pixmap,
							  mask);
}
