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
#include <netinet/in.h>
#include <ctype.h>
#include "hx.h"
#include "hl_access.h"
#include "gtk_hlist.h"
#include "cicn.h"
#include "network.h"
#include "chat.h"
#include "gtkhx.h"
#include "msg.h"
#include "gtkutil.h"
#include "tasks.h"
#include "rcv.h"

static int user_storow, user_stocolumn;

#define COL_UID 0
#define COL_USERNAME 1
static int user_click_col;

PangoFontDescription *users_font_desc;

GdkRGBA user_colors[8];
GdkRGBA gdk_user_colors[4];

GtkWidget *msgbtn, *kickbtn, *infobtn, *banbtn, *chatbtn, *ignobtn;

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

/* Phase 4.7: GtkMenu + gtk_menu_popup_at_pointer are gone in GTK 4.
 * The user-list right-click menu is now built as a GMenu model, hung
 * off a GtkPopoverMenu with its action group's user/sess context
 * captured per-click in a small UserActionCtx that lives for the life
 * of the popover (released on the popover's "closed" signal). */

struct UserActionCtx {
	session         *sess;
	struct hx_user  *user;
};

static void prompt_chat (session *sess, guint16 uid);

static void
user_action_ctx_free (gpointer data)
{
	g_free (data);
}

static void
on_user_kick (GSimpleAction *action, GVariant *param, gpointer user_data)
{
	struct UserActionCtx *ctx = user_data;
	(void) action; (void) param;
	if (!ctx->user) return;
	hx_kick_user (&ctx->sess->htlc, ctx->user->uid, 0);
}

static void
on_user_ban (GSimpleAction *action, GVariant *param, gpointer user_data)
{
	struct UserActionCtx *ctx = user_data;
	(void) action; (void) param;
	if (!ctx->user) return;
	hx_kick_user (&ctx->sess->htlc, ctx->user->uid, 1);
}

static void
on_user_ignore (GSimpleAction *action, GVariant *param, gpointer user_data)
{
	struct UserActionCtx *ctx = user_data;
	(void) action; (void) param;
	if (!ctx->user) return;
	ctx->user->ignore = 1;
	hx_printf_prefix (&ctx->sess->htlc, 0, INFOPREFIX,
	                  _("ignore: %s is now ignored\n"), ctx->user->name);
}

static void
on_user_unignore (GSimpleAction *action, GVariant *param, gpointer user_data)
{
	struct UserActionCtx *ctx = user_data;
	(void) action; (void) param;
	if (!ctx->user) return;
	ctx->user->ignore = 0;
	hx_printf_prefix (&ctx->sess->htlc, 0, INFOPREFIX,
	                  _("ignore: %s is now unignored\n"), ctx->user->name);
}

static void
on_user_info (GSimpleAction *action, GVariant *param, gpointer user_data)
{
	struct UserActionCtx *ctx = user_data;
	(void) action; (void) param;
	if (!ctx->user) return;
	hx_get_user_info (&ctx->sess->htlc, ctx->user->uid);
}

static void
on_user_msg (GSimpleAction *action, GVariant *param, gpointer user_data)
{
	struct UserActionCtx *ctx = user_data;
	struct msgwin *msg;
	(void) action; (void) param;

	if (!ctx->user) return;

	if ((msg = msgwin_with_uid (ctx->user->uid)))
		gtk_window_present (GTK_WINDOW (msg->window));
	else
		create_msgwin (ctx->user->uid, ctx->user->name);
}

static void
on_user_pchat (GSimpleAction *action, GVariant *param, gpointer user_data)
{
	struct UserActionCtx *ctx = user_data;
	struct gtkhx_chat *gchat;
	int with_cid = 0;
	(void) action; (void) param;

	if (!ctx->user) return;

	for (gchat = ctx->sess->gchat_list; gchat; gchat = gchat->prev)
		if (gchat->cid)
			with_cid = 1;

	if (!with_cid)
		hx_chat_user (&ctx->sess->htlc, ctx->user->uid);
	else
		prompt_chat (ctx->sess, ctx->user->uid);
}

static const GActionEntry user_action_entries[] = {
	{ "kick",     on_user_kick,     NULL, NULL, NULL, {0} },
	{ "ban",      on_user_ban,      NULL, NULL, NULL, {0} },
	{ "ignore",   on_user_ignore,   NULL, NULL, NULL, {0} },
	{ "unignore", on_user_unignore, NULL, NULL, NULL, {0} },
	{ "info",     on_user_info,     NULL, NULL, NULL, {0} },
	{ "msg",      on_user_msg,      NULL, NULL, NULL, {0} },
	{ "pchat",    on_user_pchat,    NULL, NULL, NULL, {0} },
};

static void
user_popover_closed (GtkPopover *popover, gpointer data)
{
	GtkWidget *anchor;
	(void) data;

	/* Phase 5: clear the "user" action group we installed on the
	 * anchor in user_popup. Each right-click installs a fresh
	 * action group with handlers bound to the just-clicked user;
	 * leaving the previous group on the anchor would leak the ctx
	 * + run handlers against a stale user pointer if the popover
	 * is somehow re-shown without re-creating it. Passing NULL
	 * clears the slot. */
	anchor = g_object_get_data (G_OBJECT (popover), "user-action-anchor");
	if (anchor)
		gtk_widget_insert_action_group (anchor, "user", NULL);

	/* Phase 4.7: the GtkPopoverMenu was parented to the user list with
	 * gtk_widget_set_parent; reverse it on close so the popover and its
	 * action context are released. */
	gtk_widget_unparent (GTK_WIDGET (popover));
}

static void
user_popup (GtkWidget *anchor, struct hx_user *user, session *sess,
            double x, double y)
{
	GMenu *model;
	GMenu *moderate_section;
	GMenu *ignore_section;
	GMenu *interact_section;
	GtkWidget *popover;
	GtkWidget *info_label;
	GSimpleActionGroup *actions;
	struct UserActionCtx *ctx;
	char *info_markup;
	int i;

	if (!user || !sess)
		return;

	ctx = g_new0 (struct UserActionCtx, 1);
	ctx->sess = sess;
	ctx->user = user;

	model = g_menu_new ();

	/* Phase 5: info section was three disabled "noop" menu items at
	 * the top showing the user's name / icon-uid / admin-status.
	 * That layout broke hover routing in GtkPopoverMenu — once the
	 * cursor entered the menu the highlight got stuck on the first
	 * non-disabled item below the noops. The fix is to render the
	 * info as a real label widget via gtk_popover_menu_add_child +
	 * a "custom" GMenuItem rather than abusing disabled menu items.
	 * Built as Pango markup so the user's name reads bold. */
	/* Phase 5: single-line info header — was two lines (name on
	 * top, details in <small> below) but the extra line plus
	 * generous margins pushed the popup tall enough that the
	 * bottom items got clipped by the Users window's lower edge
	 * on the typical default-sized window. Bold name + dim-label
	 * details on one line is dense but readable, and frees ~24px
	 * of vertical space the menu items need. */
	info_markup = g_markup_printf_escaped (
		"<b>%s</b>  <span alpha=\"60%%\">"
		"UID %d · Icon %d · %s%s</span>",
		user->name, user->uid, user->icon,
		user->color >= 2 ? _("Admin") : _("Guest"),
		user->color % 2 ? _(" (Away)") : "");
	info_label = gtk_label_new (NULL);
	gtk_label_set_markup (GTK_LABEL (info_label), info_markup);
	gtk_label_set_xalign (GTK_LABEL (info_label), 0.0);
	gtk_widget_set_margin_start  (info_label, 12);
	gtk_widget_set_margin_end    (info_label, 12);
	gtk_widget_set_margin_top    (info_label, 6);
	gtk_widget_set_margin_bottom (info_label, 2);
	g_free (info_markup);
	{
		GMenu *info_section = g_menu_new ();
		GMenuItem *info_item = g_menu_item_new (NULL, NULL);
		g_menu_item_set_attribute (info_item, "custom", "s",
		                           "user-info");
		g_menu_append_item (info_section, info_item);
		g_object_unref (info_item);
		g_menu_append_section (model, NULL, G_MENU_MODEL (info_section));
		g_object_unref (info_section);
	}

	/* Kick / Ban — only show when the account has DISCONNECT_USERS.
	 * mhxd's struct hl_access_bits has a single bit (22) that gates
	 * both kick and ban; servers that don't grant it reject the
	 * commands at the wire level anyway, so hiding the menu entries
	 * keeps users from clicking buttons that won't work. The whole
	 * section is omitted (rather than disabled-but-visible) when the
	 * bit is off — the section separator goes with it, so the popup
	 * collapses cleanly. */
	if (hl_access_has ((const guint8 *) &sess->htlc.access,
	                   HL_ACCESS_DISCONNECT_USERS)) {
		moderate_section = g_menu_new ();
		g_menu_append (moderate_section, _("Kick"), "user.kick");
		g_menu_append (moderate_section, _("Ban"),  "user.ban");
		g_menu_append_section (model, NULL, G_MENU_MODEL (moderate_section));
		g_object_unref (moderate_section);
	}

	/* Ignore / UnIgnore */
	ignore_section = g_menu_new ();
	g_menu_append (ignore_section, _("Ignore"),   "user.ignore");
	g_menu_append (ignore_section, _("UnIgnore"), "user.unignore");
	g_menu_append_section (model, NULL, G_MENU_MODEL (ignore_section));
	g_object_unref (ignore_section);

	/* Interact */
	interact_section = g_menu_new ();
	g_menu_append (interact_section, _("Get User Info"),    "user.info");
	g_menu_append (interact_section, _("Private Message"),  "user.msg");
	g_menu_append (interact_section, _("Private Chat"),     "user.pchat");
	g_menu_append_section (model, NULL, G_MENU_MODEL (interact_section));
	g_object_unref (interact_section);

	/* Phase 5: build the GSimpleActionGroup BEFORE the popover, then
	 * install it on the anchor widget. The earlier code created the
	 * popover from the menu model first and inserted the action group
	 * onto the popover afterwards — which left the menu items stuck
	 * in a "no action available" disabled state at construction time
	 * (the user reported "popup menu does not work; items can't be
	 * selected"). Moving the action group onto the anchor first means
	 * gtk_popover_menu_new_from_model resolves user.kick / user.ban /
	 * user.info / user.msg / user.pchat to live actions as it builds
	 * the GtkButton items, so they come up enabled and clickable. */
	actions = g_simple_action_group_new ();
	for (i = 0; i < (int) G_N_ELEMENTS (user_action_entries); i++) {
		const GActionEntry *e = &user_action_entries[i];
		GSimpleAction *act = g_simple_action_new (e->name, NULL);
		/* ctx is owned by the popover (set below with destroy notify);
		 * each action's closure holds a borrowed pointer that is valid
		 * for as long as the popover — and thus the action group — is
		 * alive. Synchronous activation means the borrow is always safe
		 * inside the handler. */
		g_signal_connect (act, "activate", G_CALLBACK (e->activate), ctx);
		g_action_map_add_action (G_ACTION_MAP (actions), G_ACTION (act));
		g_object_unref (act);
	}
	gtk_widget_insert_action_group (anchor, "user", G_ACTION_GROUP (actions));
	g_object_unref (actions);

	popover = gtk_popover_menu_new_from_model (G_MENU_MODEL (model));
	g_object_unref (model);
	gtk_popover_set_has_arrow (GTK_POPOVER (popover), FALSE);
	gtk_popover_set_pointing_to (GTK_POPOVER (popover),
	                             &(GdkRectangle) { (int) x, (int) y, 1, 1 });
	gtk_widget_set_halign (popover, GTK_ALIGN_START);

	/* Hang the ctx off the popover so its lifetime matches the popover's
	 * — when the popover is unparented the destroy notify frees it.
	 * The action group on the anchor outlives the popover but its
	 * activate handlers borrow ctx, so we also unhook the action group
	 * from the anchor on close (see user_popover_closed). */
	g_object_set_data_full (G_OBJECT (popover), "user-action-ctx",
	                        ctx, user_action_ctx_free);
	g_object_set_data (G_OBJECT (popover), "user-action-anchor", anchor);

	/* Phase 5: parent the popover to the anchor first, then bind the
	 * custom info_label child. Doing the add_child before set_parent
	 * left the popover's internal CSS tree disconnected from the
	 * anchor's, and the subsequent set_parent triggered a
	 * gtk_css_node_insert_after critical when GTK reattached the
	 * subtree. Parent-then-add_child keeps the CSS hierarchy
	 * connected for the entire add_child operation. */
	gtk_widget_set_parent (popover, anchor);

	/* Phase 5: bind the info_label to the "user-info" custom menu
	 * item we declared above. GtkPopoverMenu renders the named
	 * widget in place of the section content. */
	gtk_popover_menu_add_child (GTK_POPOVER_MENU (popover),
	                            info_label, "user-info");

	g_signal_connect (popover, "closed",
	                  G_CALLBACK (user_popover_closed), NULL);

	gtk_popover_popup (GTK_POPOVER (popover));
}

int users_sort(GtkHList *hlist, gconstpointer ptr1, gconstpointer ptr2)
{
	const GtkHListRow *row1 = ptr1;
	const GtkHListRow *row2 = ptr2;
	struct hx_user *usr1 = row1->data;
	struct hx_user *usr2 = row2->data;

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

/* Phase 4.5: GTK 4 widgets don't emit button-press-event. Clicks come
 * via a GtkGestureClick controller; the "pressed" signal carries
 * (gesture, n_press, x, y, user_data). The button is read off the
 * gesture itself with gtk_gesture_single_get_current_button. */
static void
user_pressed (GtkGestureClick *gesture, int n_press,
              double x, double y, gpointer data)
{
	GtkWidget *list = gtk_event_controller_get_widget (
		GTK_EVENT_CONTROLLER (gesture));
	session  *sess = data;
	guint     button = gtk_gesture_single_get_current_button (
		GTK_GESTURE_SINGLE (gesture));
	int row = -1, column = -1;

	gtk_hlist_get_selection_info (GTK_HLIST (list), (int) x, (int) y,
	                              &row, &column);

	if (button == GDK_BUTTON_PRIMARY) {
		if (n_press == 2) {
			struct hx_user *user =
				gtk_hlist_get_row_data (GTK_HLIST (list), row);
			if (user) {
				struct msgwin *msg = msgwin_with_uid (user->uid);
				if (msg)
					gtk_window_present (GTK_WINDOW (msg->window));
				else
					create_msgwin (user->uid, user->name);
			}
		} else if (row >= 0) {
			user_storow    = row;
			user_stocolumn = column;
		}
	} else if (button == GDK_BUTTON_SECONDARY) {
		struct hx_user *user =
			gtk_hlist_get_row_data (GTK_HLIST (list), row);
		if (user) {
			gtk_hlist_select_row (GTK_HLIST (list), row, 0);
			user_popup (list, user, sess, x, y);
		}
	}
}

void
users_attach_click_gesture (GtkWidget *list, session *sess)
{
	/* Phase 4.5: a GtkGestureClick with button=0 fires for every button.
	 * GTK_PHASE_BUBBLE so the GtkTreeView's own selection click runs first
	 * (it does single-click row selection and is the source of the row
	 * highlight we want under our right-click menu). */
	GtkGesture *click = gtk_gesture_click_new ();

	gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), 0);
	gtk_event_controller_set_propagation_phase (
		GTK_EVENT_CONTROLLER (click), GTK_PHASE_BUBBLE);
	g_signal_connect (click, "pressed", G_CALLBACK (user_pressed), sess);
	gtk_widget_add_controller (list, GTK_EVENT_CONTROLLER (click));
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
	session *sess = g_object_get_data(G_OBJECT(widget), "sess");

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
	session *sess = g_object_get_data(G_OBJECT(widget), "sess");

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
	session *sess = g_object_get_data(G_OBJECT(widget), "sess");

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
	session *sess = g_object_get_data(G_OBJECT(widget), "sess");

	if(!users_list)
		return;
	user = gtk_hlist_get_row_data(GTK_HLIST(users_list), user_storow);
	if(!user)
		return;
	hx_kick_user(&sess->htlc, user->uid, 1);
}


/* Phase 4.5: GdkEventButton is gone; the gtk_hlist_compat shim emits
 * "select_row" with a NULL GdkEvent so the parameter is just typed
 * as the bare GdkEvent* now. The handler only reads `row'. */
static void
select_cid (GtkWidget *widget, gint row, gint col, GdkEvent *event,
            gpointer data)
{
	guint32 *cid = data;
	(void) widget; (void) col; (void) event;
	*cid = row;
}

/* Phase 5: AdwAlertDialog with three responses (Cancel / New / Invite)
 * replaces the hand-rolled GtkDialog. The chat list (GtkHList of
 * existing pchat sessions) goes in extra-child. The response
 * handler dispatches by id: "invite" reads the selected row's
 * stashed cid and calls hx_invite_user, "new" creates a fresh pchat
 * via hx_chat_user, "cancel" does nothing.
 *
 * State carried through user_data: sess + uid + list + selected_row.
 * The list pointer lets the response handler reach back into the
 * GtkHList for row_data; selected_row is updated by select_cid as
 * the user picks a different row. Both freed via the "closed"
 * signal once the response handler returns. */
struct prompt_chat_ctx {
	session  *sess;
	guint16   uid;
	GtkWidget *list;       /* the GtkHList of existing pchats */
	guint32   selected_row;
};

static void
prompt_chat_response (AdwAlertDialog *dialog, const char *response, gpointer data)
{
	struct prompt_chat_ctx *ctx = data;
	(void) dialog;

	if (g_strcmp0 (response, "invite") == 0) {
		guint32 chat_cid = GPOINTER_TO_UINT (
			gtk_hlist_get_row_data (GTK_HLIST (ctx->list),
			                        ctx->selected_row));
		hx_invite_user (&ctx->sess->htlc, ctx->uid, chat_cid);
	} else if (g_strcmp0 (response, "new") == 0) {
		hx_chat_user (&ctx->sess->htlc, ctx->uid);
	}
}

static void
prompt_chat_closed (AdwDialog *dialog, gpointer data)
{
	(void) dialog;
	g_free (data);
}

static void prompt_chat (session *sess, guint16 _uid)
{
	AdwDialog *dialog;
	GtkWidget *list, *scroll;
	struct gtkhx_chat *gchat;
	struct prompt_chat_ctx *ctx;
	char *titles[] = { "CID", "Subject" };
	char *entry[2];

	dialog = adw_alert_dialog_new (
		_("Private Chat Invitation"),
		_("Invite this user to an existing private chat, "
		  "or create a new one."));
	adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog),
	                               "cancel", _("_Cancel"));
	adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog),
	                               "new",    _("_New Chat"));
	adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog),
	                               "invite", _("_Invite"));
	adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog),
	                                          "invite",
	                                          ADW_RESPONSE_SUGGESTED);
	adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog),
	                                       "invite");
	adw_alert_dialog_set_close_response   (ADW_ALERT_DIALOG (dialog),
	                                       "cancel");

	ctx = g_new0 (struct prompt_chat_ctx, 1);
	ctx->sess = sess;
	ctx->uid  = _uid;

	scroll = gtk_scrolled_window_new ();
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
	                                GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);

	list = gtk_hlist_new_with_titles (2, titles);
	gtk_hlist_set_selection_mode (GTK_HLIST (list), GTK_SELECTION_SINGLE);
	gtk_hlist_set_column_width   (GTK_HLIST (list), 0, 80);
	g_signal_connect (list, "select_row",
	                  G_CALLBACK (select_cid), &ctx->selected_row);
	gtkhx_widget_set_child (scroll, list);
	gtk_widget_set_size_request (scroll, 350, 200);

	for (gchat = sess->gchat_list; gchat; gchat = gchat->prev) {
		gint row;
		if (!gchat->cid)
			continue;
		entry[0] = g_strdup_printf ("0x%08x", gchat->chat->cid);
		entry[1] = gchat->chat->subject;
		row = gtk_hlist_append (GTK_HLIST (list), entry);
		/* Stash the cid as row_data so the response handler can
		 * recover it without parsing back the display string. */
		gtk_hlist_set_row_data (GTK_HLIST (list), row,
		                        GUINT_TO_POINTER (gchat->chat->cid));
	}
	ctx->list = list;

	adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dialog), scroll);

	g_signal_connect (dialog, "response",
	                  G_CALLBACK (prompt_chat_response), ctx);
	g_signal_connect (dialog, "closed",
	                  G_CALLBACK (prompt_chat_closed), ctx);

	adw_dialog_present (dialog,
	                    sess && sess->users_window
	                    ? GTK_WIDGET (sess->users_window)
	                    : NULL);
}


void user_chat_btn(GtkWidget *widget, gpointer data)
{
	struct hx_user *user;
	int with_cid = 0;
	struct gtkhx_chat *gchat;
	GtkWidget *users_list = data;
	session *sess = g_object_get_data(G_OBJECT(widget), "sess");

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



/* Phase 4.5: GTK 4 fires "close-request" instead of "delete-event"
 * when a window's titlebar close button is clicked. The handler returns
 * FALSE to allow the default destroy to proceed. */
static gboolean close_users_window (GtkWindow *window, gpointer data)
{
	session *sess = data;
	(void) window;

	sess->users_window = 0;
	sess->users_list = 0;
	gtkhx_prefs.geo.users.open = 0;
	gtkhx_prefs.geo.users.init = 0;
	return FALSE;
}

/* Phase 4.5: the configure-event size tracker is gone — GTK 4 widgets
 * don't fire configure-event. Window size is now captured at hx_quit()
 * time alongside the position; see gtkhx.c gtkhx_save_window_positions. */

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
	GtkWidget *users_list;
	GtkWidget *users_window;
	gchar *titles[2];
	session *sess = data;
	titles[0] = _("UID");
	titles[1] = _("Name");

	if (gtkhx_prefs.geo.users.open) {
		gtk_window_present(GTK_WINDOW(sess->users_window));
		return;
	}

	users_window = gtk_window_new();
	/* Phase 3.x: dropped a GTK 1.2-era gtk_widget_realize + get_style
	 * pair that left `style' unread. Forcing realize on a toplevel
	 * before its children are packed is a footgun under GTK 3 Wayland —
	 * it creates the wl_surface in an under-committed state. */
	gtk_window_set_title(GTK_WINDOW(users_window), _("Users"));
	/* Phase 5: default-size, not size_request — set_size_request
	 * sets both min AND natural in GTK 4, baking in a floor that
	 * (a) wasn't intended and (b) made the right-click popover
	 * clip its bottom items on the typical 400px-tall window.
	 * 320 wide × 480 tall gives the popup full breathing room
	 * (5 menu items + 1 info header + section gaps ≈ 280px) and
	 * is still resizable down for narrow displays. */
	gtk_window_set_default_size(GTK_WINDOW(users_window), 320, 480);
	gtk_window_set_resizable(GTK_WINDOW(users_window), TRUE);
	g_signal_connect(users_window, "close-request",
			   G_CALLBACK(close_users_window), sess);


	users_list = gtk_hlist_new_with_titles(2, titles);
	gtk_hlist_set_column_width(GTK_HLIST(users_list), 0, 35);
	gtk_hlist_set_column_width(GTK_HLIST(users_list), 1, 240);
	gtk_hlist_set_row_height(GTK_HLIST(users_list), 18);
	gtk_hlist_set_shadow_type(GTK_HLIST(users_list), GTK_SHADOW_NONE);
	gtk_hlist_set_column_justification(GTK_HLIST(users_list), 1,
									   GTK_JUSTIFY_LEFT);
	gtk_hlist_set_compare_func(GTK_HLIST(users_list),
							   (GtkHListCompareFunc) users_sort);
	g_signal_connect(users_list, "click_column",
					   G_CALLBACK(usercol_clicked), sess);
	/* Phase 4.5: button-press-event is gone in GTK 4. The gesture
	 * controller installed here dispatches double-clicks to the message
	 * window and right-clicks to the user popup. */
	users_attach_click_gesture(users_list, sess);

	if (!users_font_desc)
		users_font_desc = pango_font_description_from_string ("Sans 10");
	gtkhx_refresh_userlist_css (users_font_desc);
	gtkhx_apply_userlist_style (users_list);

	users_window_scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(users_window_scroll),
				       GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
	/* Phase 5: dropped explicit 240x400 size_request; vexpand on
	 * the scroll lets it grow with the window naturally, and the
	 * 240px min width was a holdover that prevented the user from
	 * shrinking the window narrower than that. */
	gtk_widget_set_vexpand (users_window_scroll, TRUE);
	gtkhx_widget_set_child(users_window_scroll, users_list);

	/* Phase 5: per-user action buttons via the shared
	 * gtkhx_pixmap_button helper (2x scale, matches the toolbar). */
	msgbtn = gtkhx_pixmap_button ("/com/nasledov/gtkhx/pixmaps/msg.xpm",
	                              _("Msg"), 2,
	                              G_CALLBACK (open_message_btn), users_list);
	g_object_set_data (G_OBJECT (msgbtn), "sess", sess);

	kickbtn = gtkhx_pixmap_button ("/com/nasledov/gtkhx/pixmaps/kick.xpm",
	                               _("Kick"), 2,
	                               G_CALLBACK (user_kick_btn), users_list);
	g_object_set_data (G_OBJECT (kickbtn), "sess", sess);

	infobtn = gtkhx_pixmap_button ("/com/nasledov/gtkhx/pixmaps/info.xpm",
	                               _("User Info"), 2,
	                               G_CALLBACK (user_info_btn), users_list);
	g_object_set_data (G_OBJECT (infobtn), "sess", sess);

	banbtn = gtkhx_pixmap_button ("/com/nasledov/gtkhx/pixmaps/ban.xpm",
	                              _("Ban"), 2,
	                              G_CALLBACK (user_ban_btn), users_list);
	g_object_set_data (G_OBJECT (banbtn), "sess", sess);

	chatbtn = gtkhx_pixmap_button ("/com/nasledov/gtkhx/pixmaps/chat.xpm",
	                               _("Private Chat"), 2,
	                               G_CALLBACK (user_chat_btn), users_list);
	g_object_set_data (G_OBJECT (chatbtn), "sess", sess);

	ignobtn = gtkhx_pixmap_button ("/com/nasledov/gtkhx/pixmaps/ignore.xpm",
	                               _("Ignore"), 2,
	                               G_CALLBACK (user_igno_btn), users_list);
	g_object_set_data (G_OBJECT (ignobtn), "sess", sess);

	gtk_widget_set_sensitive(msgbtn, FALSE);
	gtk_widget_set_sensitive(kickbtn, FALSE);
	gtk_widget_set_sensitive(infobtn, FALSE);
	gtk_widget_set_sensitive(banbtn, FALSE);
	gtk_widget_set_sensitive(chatbtn, FALSE);
	gtk_widget_set_sensitive(ignobtn, FALSE);

	/* Phase 5: action buttons live in the AdwHeaderBar instead of an
	 * in-content topframe + hbuttonbox row. Communication actions
	 * (Msg, Private Chat) on the start; moderation actions (Info,
	 * Kick, Ban, Ignore) packed end-to-start so the visible order
	 * reads info / kick / ban / ignore left-to-right with the close
	 * button at the very right. The window's child collapses to the
	 * scrolled user list — no outer vbox or topframe. */
	{
		GtkWidget *header = adw_header_bar_new ();

		adw_header_bar_pack_start (ADW_HEADER_BAR (header), msgbtn);
		adw_header_bar_pack_start (ADW_HEADER_BAR (header), chatbtn);

		adw_header_bar_pack_end (ADW_HEADER_BAR (header), ignobtn);
		adw_header_bar_pack_end (ADW_HEADER_BAR (header), banbtn);
		adw_header_bar_pack_end (ADW_HEADER_BAR (header), kickbtn);
		adw_header_bar_pack_end (ADW_HEADER_BAR (header), infobtn);

		gtk_window_set_titlebar (GTK_WINDOW (users_window), header);
	}

	gtk_window_set_child (GTK_WINDOW (users_window), users_window_scroll);

	init_keyaccel(users_window);

	
	/* Phase 3.x: only apply saved geometry when the prefs file actually
	 * has one. On a fresh install the geo struct is zeroed, and calling
	 * set_size_request(window, 0, 0) collapses the window to 0x0 — under
	 * GTK 3 the compositor then refuses to map it and the window never
	 * appears. The earlier set_size_request(264, 400) call serves as
	 * the default size. */
	if (gtkhx_prefs.geo.users.xsize > 0 && gtkhx_prefs.geo.users.ysize > 0)
		gtk_window_set_default_size(GTK_WINDOW(users_window),
		                            gtkhx_prefs.geo.users.xsize,
		                            gtkhx_prefs.geo.users.ysize);
	if (gtkhx_prefs.geo.users.xpos > 0 || gtkhx_prefs.geo.users.ypos > 0)
		/* Phase 4.2: gtk_window_move removed (Wayland) */
	gtk_window_present(GTK_WINDOW(users_window));

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
	session *sess = &the_session;

	if (!sess->users_list || !gtkhx_prefs.geo.users.open)
		return;

	gtk_hlist_clear(GTK_HLIST(sess->users_list));
}

/* Phase 5 dark-theme: the four-slot palette is { regular, idle, admin,
 * admin-idle }. The "regular" slot used to be hard-coded black, which
 * is invisible against a dark theme's row background. Returning NULL
 * for that slot lets the cell renderer fall back to the GTK theme's
 * default foreground so regular users read on both light and dark
 * themes. The other three slots stay distinctive enough to convey
 * status under either theme. */
static GdkRGBA *user_color_gdk (guint16 color)
{
	if ((color % 4) == 0)
		return NULL;
	return &gdk_user_colors[color % 4];
}

void user_create (struct htlc_conn *htlc, struct chat *chat,
				  struct hx_user *user, const char *nam, guint16 icon,
				  guint16 color)
{
	GdkPixmap *pixmap;
	GdkBitmap *mask;
	gint row;
	session *sess = &the_session;
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
	gtk_hlist_set_foreground(GTK_HLIST(losers_list), row, user_color_gdk(color));
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
	session *sess = &the_session;
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
	session *sess = &the_session;
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
	gtk_hlist_set_foreground(GTK_HLIST(losers_list), row, user_color_gdk(color));
	load_icon(sess->users_window, icon, &icon_files, 1, &pixmap, &mask);
	if (!pixmap)
		gtk_hlist_set_text(GTK_HLIST(losers_list), row, 1, nam);
	else
		gtk_hlist_set_pixtext(GTK_HLIST(losers_list), row, 1, nam, 34, pixmap,
							  mask);
}
