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
#include <ctype.h>
#include "hx.h"
#include "gtkhx_session.h"
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
#include "users.h"

static int user_storow, user_stocolumn;

#define COL_UID 0
#define COL_USERNAME 1
static int user_click_col;

PangoFontDescription *users_font_desc;

GdkRGBA user_colors[8];
GdkRGBA gdk_user_colors[4];

GtkWidget *msgbtn, *kickbtn, *infobtn, *banbtn, *chatbtn, *ignobtn;

void
hx_change_name_icon (struct htlc_conn *htlc)
{
    guint16 icon16 = htons (htlc->icon);

    /* Phase E2: encode the nick to the negotiated wire encoding.
	 * is_body = FALSE — nicks don't have line endings; we want the
	 * Mac-Roman transcoding (or UTF-8 passthrough) without the
	 * LF→CR substitution. */
    gboolean utf8 = (htlc->caps & HTLC_CAP_TEXT_ENCODING) != 0;
    gsize name_len = 0;
    char *name_wire
        = gtkhx_text_for_wire ((const char *)htlc->name, strlen (htlc->name),
                               utf8, /*is_body=*/FALSE, &name_len);

    /* Colored-Nicknames extension. Include HTLC_DATA_COLOR
	 * (BE u32, 0x00RRGGBB) ONLY when the local pref has set a real
	 * color — we deliberately don't send HX_NICK_COLOR_NONE because
	 * the spec's auto-opt-in marks the session as color-aware on
	 * first DATA_COLOR receipt regardless of the value, and a
	 * "no color" client shouldn't opt in. Servers that don't know
	 * the extension ignore the trailing chunk; supporting servers
	 * mark us color-aware and start decorating other users'
	 * USER_CHANGE pushes for us. */
    if (htlc->nick_color != HX_NICK_COLOR_NONE) {
        guint32 color32 = htonl (htlc->nick_color);
        hlwrite (htlc, HTLC_HDR_USER_CHANGE, 0, 3, HTLC_DATA_ICON, 2, &icon16,
                 HTLC_DATA_NAME, (guint16)name_len, name_wire,
                 HTLC_DATA_COLOR, 4, &color32);
    } else {
        hlwrite (htlc, HTLC_HDR_USER_CHANGE, 0, 2, HTLC_DATA_ICON, 2, &icon16,
                 HTLC_DATA_NAME, (guint16)name_len, name_wire);
    }
    g_free (name_wire);
}

void
hx_kick_user (struct htlc_conn *htlc, guint16 uid, guint16 ban)
{
    uid = htons (uid);
    task_new (htlc, RCV_TASK_FN (rcv_task_kick), 0, 0, "kick");
    if (ban) {
        ban = htons (ban);
        hlwrite (htlc, HTLC_HDR_USER_KICK, 0, 2, HTLC_DATA_BAN, 2, &ban,
                 HTLC_DATA_UID, 2, &uid);
    } else {
        hlwrite (htlc, HTLC_HDR_USER_KICK, 0, 1, HTLC_DATA_UID, 2, &uid);
    }
}

void
hx_get_user_info (struct htlc_conn *htlc, guint16 uid)
{
    guint16 *_uid = g_malloc (sizeof (guint16));
    *_uid = uid;

    task_new (htlc, RCV_TASK_FN (rcv_task_user_info), (void *)_uid, 0, "info");
    uid = htons (uid);
    hlwrite (htlc, HTLC_HDR_USER_GETINFO, 0, 1, HTLC_DATA_UID, 2, &uid);
}

/* Phase 5+: per-chat user list lives in chat->users, a
 * GHashTable<u16 uid, struct hx_user*>. hx_user_new mallocs a fresh
 * hx_user, stamps its uid, and inserts it; hx_user_delete drops it
 * from the table (the table's value-destroy notify g_frees the
 * struct); hx_user_with_uid is an O(1) lookup. */
struct hx_user *
hx_user_new (struct chat *chat, guint16 uid)
{
    struct hx_user *user = g_malloc0 (sizeof (struct hx_user));
    user->uid = uid;
    /* Colored-Nicknames extension. Default to "no color"
	 * — the renderer falls back to the legacy status palette
	 * (Admin/Guest/Away from user->color) unless a server-pushed
	 * DATA_COLOR chunk overrides this. g_malloc0 has already
	 * zeroed the struct so the explicit assignment is just to
	 * avoid a "what does zero mean here?" question on the read
	 * side — HX_NICK_COLOR_NONE = 0xFFFFFFFF is the canonical
	 * sentinel. */
    user->nick_color = HX_NICK_COLOR_NONE;
    g_hash_table_insert (chat->users, GUINT_TO_POINTER ((guint)uid), user);
    return user;
}

void
hx_user_delete (struct chat *chat, struct hx_user *user)
{
    if (!user || !chat || !chat->users) {
        return;
    }
    g_hash_table_remove (chat->users, GUINT_TO_POINTER ((guint)user->uid));
}

struct hx_user *
hx_user_with_uid (struct chat *chat, guint16 uid)
{
    if (!chat || !chat->users) {
        return NULL;
    }
    return g_hash_table_lookup (chat->users, GUINT_TO_POINTER ((guint)uid));
}

/* Name lookup remains a linear scan — only one caller
 * (commands.c handle_command_msg) uses it, and we expect chat
 * membership lists to stay small enough for that not to matter. */
struct hx_user *
hx_user_with_name (struct chat *chat, const char *name)
{
    GHashTableIter iter;
    gpointer val;

    if (!chat || !chat->users) {
        return NULL;
    }
    g_hash_table_iter_init (&iter, chat->users);
    while (g_hash_table_iter_next (&iter, NULL, &val)) {
        struct hx_user *u = val;
        if (strcmp (u->name, name) == 0) {
            return u;
        }
    }
    return NULL;
}

/* Phase 4.7: GtkMenu + gtk_menu_popup_at_pointer are gone in GTK 4.
 * The user-list right-click menu is now built as a GMenu model, hung
 * off a GtkPopoverMenu with its action group's user/sess context
 * captured per-click in a small UserActionCtx that lives for the life
 * of the popover (released on the popover's "closed" signal). */

struct UserActionCtx {
    session *sess;
    struct hx_user *user;
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
    (void)action;
    (void)param;
    if (!ctx->user) {
        return;
    }
    hx_kick_user (&ctx->sess->htlc, ctx->user->uid, 0);
}

static void
on_user_ban (GSimpleAction *action, GVariant *param, gpointer user_data)
{
    struct UserActionCtx *ctx = user_data;
    (void)action;
    (void)param;
    if (!ctx->user) {
        return;
    }
    hx_kick_user (&ctx->sess->htlc, ctx->user->uid, 1);
}

static void
on_user_ignore (GSimpleAction *action, GVariant *param, gpointer user_data)
{
    struct UserActionCtx *ctx = user_data;
    (void)action;
    (void)param;
    if (!ctx->user) {
        return;
    }
    ctx->user->ignore = 1;
    hx_printf_prefix (&ctx->sess->htlc, 0, INFOPREFIX,
                      _ ("ignore: %s is now ignored\n"), ctx->user->name);
}

static void
on_user_unignore (GSimpleAction *action, GVariant *param, gpointer user_data)
{
    struct UserActionCtx *ctx = user_data;
    (void)action;
    (void)param;
    if (!ctx->user) {
        return;
    }
    ctx->user->ignore = 0;
    hx_printf_prefix (&ctx->sess->htlc, 0, INFOPREFIX,
                      _ ("ignore: %s is now unignored\n"), ctx->user->name);
}

static void
on_user_info (GSimpleAction *action, GVariant *param, gpointer user_data)
{
    struct UserActionCtx *ctx = user_data;
    (void)action;
    (void)param;
    if (!ctx->user) {
        return;
    }
    hx_get_user_info (&ctx->sess->htlc, ctx->user->uid);
}

static void
on_user_msg (GSimpleAction *action, GVariant *param, gpointer user_data)
{
    struct UserActionCtx *ctx = user_data;
    struct msgwin *msg;
    (void)action;
    (void)param;

    if (!ctx->user) {
        return;
    }

    if ((msg = msgwin_with_uid (ctx->user->uid))) {
        gtk_window_present (GTK_WINDOW (msg->window));
    } else {
        create_msgwin (ctx->user->uid, ctx->user->name);
    }
}

static void
on_user_pchat (GSimpleAction *action, GVariant *param, gpointer user_data)
{
    struct UserActionCtx *ctx = user_data;
    int with_cid = 0;
    (void)action;
    (void)param;

    if (!ctx->user) {
        return;
    }

    if (ctx->sess->gchats) {
        GHashTableIter iter;
        gpointer key;
        g_hash_table_iter_init (&iter, ctx->sess->gchats);
        while (g_hash_table_iter_next (&iter, &key, NULL)) {
            if (GPOINTER_TO_UINT (key) != 0) {
                with_cid = 1;
                break;
            }
        }
    }

    if (!with_cid) {
        hx_chat_user (&ctx->sess->htlc, ctx->user->uid);
    } else {
        prompt_chat (ctx->sess, ctx->user->uid);
    }
}

/* Phase 5: the GActionEntry table that drove the old GtkPopoverMenu
 * is gone — the bare-popover rewrite invokes the on_user_* handlers
 * directly via on_user_btn_clicked. The handler signatures still
 * take (GSimpleAction*, GVariant*, gpointer) so a future caller that
 * wants to reach them through GAction can do so without re-shaping
 * the body of each handler. */

static void
user_popover_closed (GtkPopover *popover, gpointer data)
{
    (void)data;
    /* Unparent on close so the popover, its ctx, and per-button
	 * closures all get released. The popover was parented to the
	 * user list anchor in user_popup. */
    gtk_widget_unparent (GTK_WIDGET (popover));
}

/* Per-button closure carries the user context plus the action
 * handler to invoke on click. The action handlers' signatures
 * still take (GSimpleAction*, GVariant*, gpointer) so the on_user_*
 * functions can stay shared with any future code that wants to
 * drive them through GAction; we just pass NULL/NULL here. */
struct user_btn_ctx {
    struct UserActionCtx *user_ctx;
    void (*activate) (GSimpleAction *, GVariant *, gpointer);
    GtkPopover *popover;
};

static void
on_user_btn_clicked (GtkButton *btn, gpointer data)
{
    struct user_btn_ctx *bctx = data;
    (void)btn;
    bctx->activate (NULL, NULL, bctx->user_ctx);
    gtk_popover_popdown (bctx->popover);
}

static void
user_btn_ctx_free (gpointer data, GClosure *closure)
{
    (void)closure;
    g_free (data);
}

/* Append a flat button row to the popover's vertical box. Caller
 * passes the action handler from on_user_*; we wrap it in a click
 * closure that activates it with the popover's ctx and dismisses. */
static void
user_popup_append_button (GtkBox *vbox, GtkPopover *popover,
                          struct UserActionCtx *user_ctx, const char *label,
                          void (*activate) (GSimpleAction *, GVariant *,
                                            gpointer))
{
    GtkWidget *btn = gtk_button_new_with_label (label);
    struct user_btn_ctx *bctx = g_new0 (struct user_btn_ctx, 1);

    gtk_widget_add_css_class (btn, "flat");
    gtk_widget_add_css_class (btn, "gtkhx-user-popup-item");
    gtk_button_set_has_frame (GTK_BUTTON (btn), FALSE);
    /* Phase 5: keyboard focus on the first button auto-paints a
	 * focus ring when the popover opens; the user reads that as
	 * 'this item is hovered' and gets confused when cursor motion
	 * doesn't update it. We don't expect the popup to be navigated
	 * by keyboard (right-click → click is the path), so just turn
	 * focus off for these buttons. The CSS provider below adds a
	 * visible :hover background so cursor tracking is obvious. */
    gtk_widget_set_focusable (btn, FALSE);
    gtk_widget_set_can_focus (btn, FALSE);
    /* Left-align label inside the flat button so the menu reads
	 * like a menu, not a row of centred captions. */
    {
        GtkWidget *lbl = gtk_button_get_child (GTK_BUTTON (btn));
        if (GTK_IS_LABEL (lbl)) {
            gtk_label_set_xalign (GTK_LABEL (lbl), 0.0);
            gtk_widget_set_hexpand (lbl, TRUE);
        }
    }

    bctx->user_ctx = user_ctx;
    bctx->activate = activate;
    bctx->popover = popover;
    g_signal_connect_data (btn, "clicked", G_CALLBACK (on_user_btn_clicked),
                           bctx, user_btn_ctx_free, 0);
    gtk_box_append (vbox, btn);
}

/* One-shot CSS provider giving our user-popup buttons a clearly
 * visible :hover background. Adwaita's default flat-button hover is
 * subtle enough that mouse tracking can be hard to read, especially
 * when the user is expecting menu-style highlighting. Install on the
 * default display once; namespaced selector keeps the rule scoped to
 * our popup buttons. */
static void
user_popup_install_css (void)
{
    static GtkCssProvider *provider = NULL;
    if (provider) {
        return;
    }
    provider = gtk_css_provider_new ();
    gtk_css_provider_load_from_string (
        provider, ".gtkhx-user-popup-item { "
                  "  padding: 4px 10px; "
                  "} "
                  ".gtkhx-user-popup-item:hover { "
                  "  background-color: alpha(currentColor, 0.10); "
                  "}");
    gtk_style_context_add_provider_for_display (
        gdk_display_get_default (), GTK_STYLE_PROVIDER (provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

static void
user_popup (GtkWidget *anchor, struct hx_user *user, session *sess, double x,
            double y)
{
    GtkWidget *popover, *vbox, *info_label, *sep, *parent;
    struct UserActionCtx *ctx;
    char *info_markup;
    GdkRectangle rect;
    graphene_point_t src_pt = { (float)x, (float)y };
    graphene_point_t dst_pt = { (float)x, (float)y };

    if (!user || !sess) {
        return;
    }

    user_popup_install_css ();

    ctx = g_new0 (struct UserActionCtx, 1);
    ctx->sess = sess;
    ctx->user = user;

    /* Phase 5: parent the popover to the toplevel root, NOT the
	 * user-list anchor. Same fix that gtkurl_show_popup needed:
	 * GtkPopover's autohide grab needs a 'top-most parent' in the
	 * widget tree, and the user list (a GtkTreeView wrapped in a
	 * GtkScrolledWindow) is not it. Anchoring there lets the
	 * popover render but breaks pointer-motion routing into its
	 * children — clicks still work because they're button-press
	 * events, but :hover never triggers, so the buttons looked
	 * frozen. Translating click coords from anchor-local to
	 * toplevel-local keeps pointing_to right. */
    parent = GTK_WIDGET (gtk_widget_get_root (anchor));
    if (!parent) {
        parent = anchor;
    }
    if (parent != anchor) {
        if (gtk_widget_compute_point (anchor, parent, &src_pt, &dst_pt)) {
            x = dst_pt.x;
            y = dst_pt.y;
        }
    }
    rect.x = (int)x;
    rect.y = (int)y;
    rect.width = 1;
    rect.height = 1;

    /* Phase 5: dropped GtkPopoverMenu in favour of a bare GtkPopover
	 * with a vertical box of flat buttons. GtkPopoverMenu had two
	 * regressions:
	 *
	 *   - It wraps its model items in an internal GtkScrolledWindow
	 *     with a non-overridable max-content-height; the menu's last
	 *     item kept getting clipped and the user had to scroll to
	 *     see it. The CSS workaround that used to disable max-height
	 *     on the inner scrolledwindow stopped applying after a GTK /
	 *     libadwaita refresh.
	 *
	 *   - Hover routing got stuck on the first item once the popover
	 *     opened; the cursor's motion events didn't re-target items
	 *     after the keyboard-focus default landed on "Ignore", so
	 *     the user couldn't pick anything except by arrow keys.
	 *
	 * Building the popover by hand avoids both: a GtkBox with native
	 * GtkButtons hover correctly, sizes to its natural content
	 * height, and gives us full control over separators / styling.
	 * The on_user_* action handlers stay as-is and are invoked from
	 * a thin per-button click closure (see on_user_btn_clicked). */
    popover = gtk_popover_new ();
    gtk_popover_set_has_arrow (GTK_POPOVER (popover), FALSE);
    gtk_popover_set_pointing_to (GTK_POPOVER (popover), &rect);
    gtk_widget_set_halign (popover, GTK_ALIGN_START);

    vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start (vbox, 4);
    gtk_widget_set_margin_end (vbox, 4);
    gtk_widget_set_margin_top (vbox, 4);
    gtk_widget_set_margin_bottom (vbox, 4);
    gtk_popover_set_child (GTK_POPOVER (popover), vbox);

    /* Header: bold name + dim details. */
    info_markup = g_markup_printf_escaped (
        "<b>%s</b>\n<small>UID %d · Icon %d · %s%s</small>", user->name,
        user->uid, user->icon, user->color >= 2 ? _ ("Admin") : _ ("Guest"),
        user->color % 2 ? _ (" (Away)") : "");
    info_label = gtk_label_new (NULL);
    gtk_label_set_markup (GTK_LABEL (info_label), info_markup);
    gtk_label_set_xalign (GTK_LABEL (info_label), 0.0);
    gtk_widget_set_margin_start (info_label, 8);
    gtk_widget_set_margin_end (info_label, 8);
    gtk_widget_set_margin_top (info_label, 4);
    gtk_widget_set_margin_bottom (info_label, 4);
    g_free (info_markup);
    gtk_box_append (GTK_BOX (vbox), info_label);

    sep = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_box_append (GTK_BOX (vbox), sep);

    /* Kick / Ban — only when the account has DISCONNECT_USERS.
	 * Same rule as before: hidden, not disabled, since the server
	 * would reject the wire op anyway. */
    if (hl_access_has ((const guint8 *)&sess->htlc.access,
                       HL_ACCESS_DISCONNECT_USERS)) {
        user_popup_append_button (GTK_BOX (vbox), GTK_POPOVER (popover), ctx,
                                  _ ("Kick"), on_user_kick);
        user_popup_append_button (GTK_BOX (vbox), GTK_POPOVER (popover), ctx,
                                  _ ("Ban"), on_user_ban);
        gtk_box_append (GTK_BOX (vbox),
                        gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
    }

    /* Ignore / UnIgnore */
    user_popup_append_button (GTK_BOX (vbox), GTK_POPOVER (popover), ctx,
                              _ ("Ignore"), on_user_ignore);
    user_popup_append_button (GTK_BOX (vbox), GTK_POPOVER (popover), ctx,
                              _ ("UnIgnore"), on_user_unignore);
    gtk_box_append (GTK_BOX (vbox),
                    gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));

    /* Interact */
    user_popup_append_button (GTK_BOX (vbox), GTK_POPOVER (popover), ctx,
                              _ ("Get User Info"), on_user_info);
    user_popup_append_button (GTK_BOX (vbox), GTK_POPOVER (popover), ctx,
                              _ ("Private Message"), on_user_msg);
    user_popup_append_button (GTK_BOX (vbox), GTK_POPOVER (popover), ctx,
                              _ ("Private Chat"), on_user_pchat);

    /* ctx outlives any one button-click — bound to the popover,
	 * freed when it's unparented. The on_user_btn_clicked closure
	 * borrows the pointer; safe since clicks fire synchronously
	 * while the popover (and so the ctx) is still alive. */
    g_object_set_data_full (G_OBJECT (popover), "user-action-ctx", ctx,
                            user_action_ctx_free);

    gtk_widget_set_parent (popover, parent);
    g_signal_connect (popover, "closed", G_CALLBACK (user_popover_closed),
                      NULL);
    gtk_popover_popup (GTK_POPOVER (popover));
}

static int
users_sort (GtkHList *hlist, gconstpointer ptr1, gconstpointer ptr2)
{
    const GtkHListRow *row1 = ptr1;
    const GtkHListRow *row2 = ptr2;
    struct hx_user *usr1 = row1->data;
    struct hx_user *usr2 = row2->data;

    if (user_click_col == COL_UID) {
        if (usr1->uid < usr2->uid) {
            return -1;
        } else {
            return 1;
        }
    } else {
        int len1 = strlen (usr1->name), len2 = strlen (usr2->name);
        int len = len1 < len2 ? len1 : len2;
        int i;

        for (i = 0; i < len; i++) {
            if (tolower (usr1->name[i]) < tolower (usr2->name[i])) {
                return -1;
            } else if (tolower (usr2->name[i]) < tolower (usr1->name[i])) {
                return 1;
            }
        }

        return 0;
    }

    return 0;
}

static void
usercol_clicked (GtkWidget *clist, gint col, gpointer data)
{
    user_click_col = col;
    gtk_hlist_sort (GTK_HLIST (clist));
}

/* Phase 4.5: GTK 4 widgets don't emit button-press-event. Clicks come
 * via a GtkGestureClick controller; the "pressed" signal carries
 * (gesture, n_press, x, y, user_data). The button is read off the
 * gesture itself with gtk_gesture_single_get_current_button. */
static void
user_pressed (GtkGestureClick *gesture, int n_press, double x, double y,
              gpointer data)
{
    GtkWidget *list
        = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
    session *sess = data;
    guint button
        = gtk_gesture_single_get_current_button (GTK_GESTURE_SINGLE (gesture));
    int row = -1, column = -1;

    gtk_hlist_get_selection_info (GTK_HLIST (list), (int)x, (int)y, &row,
                                  &column);

    if (button == GDK_BUTTON_PRIMARY) {
        if (n_press == 2) {
            struct hx_user *user
                = gtk_hlist_get_row_data (GTK_HLIST (list), row);
            if (user) {
                struct msgwin *msg = msgwin_with_uid (user->uid);
                if (msg) {
                    gtk_window_present (GTK_WINDOW (msg->window));
                } else {
                    create_msgwin (user->uid, user->name);
                }
            }
        } else if (row >= 0) {
            user_storow = row;
            user_stocolumn = column;
        }
    } else if (button == GDK_BUTTON_SECONDARY) {
        struct hx_user *user = gtk_hlist_get_row_data (GTK_HLIST (list), row);
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
    gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (click),
                                                GTK_PHASE_BUBBLE);
    g_signal_connect (click, "pressed", G_CALLBACK (user_pressed), sess);
    gtk_widget_add_controller (list, GTK_EVENT_CONTROLLER (click));
}

void
open_message_btn (GtkWidget *widget, gpointer data)
{
    struct hx_user *user;
    GtkWidget *users_list = data;

    if (!users_list) {
        return;
    }
    user = gtk_hlist_get_row_data (GTK_HLIST (users_list), user_storow);
    if (!user) {
        return;
    }
    create_msgwin (user->uid, user->name);
}

void
user_info_btn (GtkWidget *widget, gpointer data)
{
    struct hx_user *user;
    GtkWidget *users_list = data;
    session *sess = g_object_get_data (G_OBJECT (widget), "sess");

    if (!users_list) {
        return;
    }
    user = gtk_hlist_get_row_data (GTK_HLIST (users_list), user_storow);
    if (!user) {
        return;
    }

    hx_get_user_info (&sess->htlc, user->uid);
}

void
user_kick_btn (GtkWidget *widget, gpointer data)
{
    struct hx_user *user;
    GtkWidget *users_list = data;
    session *sess = g_object_get_data (G_OBJECT (widget), "sess");

    if (!users_list) {
        return;
    }
    user = gtk_hlist_get_row_data (GTK_HLIST (users_list), user_storow);
    if (!user) {
        return;
    }
    hx_kick_user (&sess->htlc, user->uid, 0);
}

void
user_igno_btn (GtkWidget *widget, gpointer data)
{
    struct hx_user *user;
    GtkWidget *users_list = data;
    session *sess = g_object_get_data (G_OBJECT (widget), "sess");

    if (!users_list) {
        return;
    }

    user = gtk_hlist_get_row_data (GTK_HLIST (users_list), user_storow);
    if (!user) {
        return;
    }

    user->ignore ^= 1;
    hx_printf_prefix (&sess->htlc, 0, INFOPREFIX,
                      user->ignore ? _ ("ignore: %s is now ignored\n")
                                   : _ ("ignore: %s is now unignored"),
                      user->name);
}

void
user_ban_btn (GtkWidget *widget, gpointer data)
{
    struct hx_user *user;
    GtkWidget *users_list = data;
    session *sess = g_object_get_data (G_OBJECT (widget), "sess");

    if (!users_list) {
        return;
    }
    user = gtk_hlist_get_row_data (GTK_HLIST (users_list), user_storow);
    if (!user) {
        return;
    }
    hx_kick_user (&sess->htlc, user->uid, 1);
}

/* Phase 4.5: GdkEventButton is gone; the gtk_hlist_compat shim emits
 * "select_row" with a NULL GdkEvent so the parameter is just typed
 * as the bare GdkEvent* now. The handler only reads `row'. */
static void
select_cid (GtkWidget *widget, gint row, gint col, GdkEvent *event,
            gpointer data)
{
    guint32 *cid = data;
    (void)widget;
    (void)col;
    (void)event;
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
    session *sess;
    guint16 uid;
    GtkWidget *list; /* the GtkHList of existing pchats */
    guint32 selected_row;
};

static void
prompt_chat_response (AdwAlertDialog *dialog, const char *response,
                      gpointer data)
{
    struct prompt_chat_ctx *ctx = data;
    (void)dialog;

    if (g_strcmp0 (response, "invite") == 0) {
        guint32 chat_cid = GPOINTER_TO_UINT (
            gtk_hlist_get_row_data (GTK_HLIST (ctx->list), ctx->selected_row));
        hx_invite_user (&ctx->sess->htlc, ctx->uid, chat_cid);
    } else if (g_strcmp0 (response, "new") == 0) {
        hx_chat_user (&ctx->sess->htlc, ctx->uid);
    }
}

static void
prompt_chat_closed (AdwDialog *dialog, gpointer data)
{
    (void)dialog;
    g_free (data);
}

static void
prompt_chat (session *sess, guint16 _uid)
{
    AdwDialog *dialog;
    GtkWidget *list, *scroll;
    struct gtkhx_chat *gchat;
    struct prompt_chat_ctx *ctx;
    char *titles[] = { "CID", "Subject" };
    char *entry[2];

    dialog = adw_alert_dialog_new (
        _ ("Private Chat Invitation"),
        _ ("Invite this user to an existing private chat, "
           "or create a new one."));
    adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "cancel",
                                   _ ("_Cancel"));
    adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "new",
                                   _ ("_New Chat"));
    adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "invite",
                                   _ ("_Invite"));
    adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog),
                                              "invite", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "invite");
    adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "cancel");

    gtkhx_dialog_add_close_shortcuts (GTK_WIDGET (dialog));

    ctx = g_new0 (struct prompt_chat_ctx, 1);
    ctx->sess = sess;
    ctx->uid = _uid;

    scroll = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);

    list = gtk_hlist_new_with_titles (2, titles);
    gtk_hlist_set_selection_mode (GTK_HLIST (list), GTK_SELECTION_SINGLE);
    gtk_hlist_set_column_width (GTK_HLIST (list), 0, 80);
    g_signal_connect (list, "select_row", G_CALLBACK (select_cid),
                      &ctx->selected_row);
    gtkhx_widget_set_child (scroll, list);
    gtk_widget_set_size_request (scroll, 350, 200);

    if (sess->gchats) {
        GHashTableIter iter;
        gpointer val;
        g_hash_table_iter_init (&iter, sess->gchats);
        while (g_hash_table_iter_next (&iter, NULL, &val)) {
            gint row;
            gchat = val;
            if (!gchat->cid) {
                continue;
            }
            entry[0] = g_strdup_printf ("0x%08x", gchat->chat->cid);
            entry[1] = gchat->chat->subject;
            row = gtk_hlist_append (GTK_HLIST (list), entry);
            /* Stash the cid as row_data so the response
			 * handler can recover it without parsing back
			 * the display string. */
            gtk_hlist_set_row_data (GTK_HLIST (list), row,
                                    GUINT_TO_POINTER (gchat->chat->cid));
        }
    }
    ctx->list = list;

    adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dialog), scroll);

    g_signal_connect (dialog, "response", G_CALLBACK (prompt_chat_response),
                      ctx);
    g_signal_connect (dialog, "closed", G_CALLBACK (prompt_chat_closed), ctx);

    adw_dialog_present (dialog, sess && sess->users_window
                                    ? GTK_WIDGET (sess->users_window)
                                    : NULL);
}

void
user_chat_btn (GtkWidget *widget, gpointer data)
{
    struct hx_user *user;
    int with_cid = 0;
    GtkWidget *users_list = data;
    session *sess = g_object_get_data (G_OBJECT (widget), "sess");

    if (!users_list) {
        return;
    }

    user = gtk_hlist_get_row_data (GTK_HLIST (users_list), user_storow);

    if (!user) {
        return;
    }
    if (sess->gchats) {
        GHashTableIter iter;
        gpointer key;
        g_hash_table_iter_init (&iter, sess->gchats);
        while (g_hash_table_iter_next (&iter, &key, NULL)) {
            if (GPOINTER_TO_UINT (key) != 0) {
                with_cid = 1;
                break;
            }
        }
    }
    if (!with_cid) {
        hx_chat_user (&sess->htlc, user->uid);
    } else {
        prompt_chat (sess, user->uid);
    }
}

/* Phase 4.5: GTK 4 fires "close-request" instead of "delete-event"
 * when a window's titlebar close button is clicked. The handler returns
 * FALSE to allow the default destroy to proceed. */
static gboolean
close_users_window (GtkWindow *window, gpointer data)
{
    session *sess = data;
    (void)window;

    sess->users_window = 0;
    sess->users_list = 0;
    gtkhx_prefs.geo.users.open = 0;
    gtkhx_prefs.geo.users.init = 0;
    return FALSE;
}

/* Phase 4.5: the configure-event size tracker is gone — GTK 4 widgets
 * don't fire configure-event. Window size is now captured at hx_quit()
 * time alongside the position; see gtkhx.c gtkhx_save_window_positions. */

void
user_list (session *sess)
{
    struct chat *pub;

    if (!sess->users_window) {
        return;
    }

    pub = chat_with_cid (sess, 0);
    gtk_hlist_freeze (GTK_HLIST (sess->users_list));
    gtk_hlist_clear (GTK_HLIST (sess->users_list));
    if (pub && pub->users) {
        GHashTableIter iter;
        gpointer val;
        g_hash_table_iter_init (&iter, pub->users);
        while (g_hash_table_iter_next (&iter, NULL, &val)) {
            struct hx_user *user = val;
            gtkhx_session_emit_user_create (gtkhx_session_get_default (),
                                            &sess->htlc, pub, user, user->name,
                                            user->icon, user->color);
        }
    }
    gtk_hlist_thaw (GTK_HLIST (sess->users_list));
}

void
create_users_window (GtkWidget *widget, gpointer data)
{
    GtkWidget *users_window_scroll;
    GtkWidget *users_list;
    GtkWidget *users_window;
    gchar *titles[2];
    session *sess = data;
    titles[0] = _ ("UID");
    titles[1] = _ ("Name");

    if (gtkhx_prefs.geo.users.open) {
        gtk_window_present (GTK_WINDOW (sess->users_window));
        return;
    }

    users_window = gtk_window_new ();
    /* Phase 3.x: dropped a GTK 1.2-era gtk_widget_realize + get_style
	 * pair that left `style' unread. Forcing realize on a toplevel
	 * before its children are packed is a footgun under GTK 3 Wayland —
	 * it creates the wl_surface in an under-committed state. */
    gtk_window_set_title (GTK_WINDOW (users_window), _ ("Users"));
    /* Phase 5: default-size, not size_request — set_size_request
	 * sets both min AND natural in GTK 4, baking in a floor that
	 * wasn't intended. The right-click popover does its own
	 * sizing/positioning math; the window default-size shouldn't
	 * be coupled to it. */
    gtk_window_set_default_size (GTK_WINDOW (users_window), 320, 480);
    gtk_window_set_resizable (GTK_WINDOW (users_window), TRUE);
    g_signal_connect (users_window, "close-request",
                      G_CALLBACK (close_users_window), sess);

    users_list = gtk_hlist_new_with_titles (2, titles);
    gtk_hlist_set_column_width (GTK_HLIST (users_list), 0, 35);
    gtk_hlist_set_column_width (GTK_HLIST (users_list), 1, 240);
    /* Row height bumped from 18 to 24 in 2026-05 alongside the
	 * 1.25x pixel-scale on the Name column (see below). Both
	 * combine to give the list ~25% larger entries — easier to
	 * read at typical desktop scales. */
    gtk_hlist_set_row_height (GTK_HLIST (users_list), 24);
    gtk_hlist_set_shadow_type (GTK_HLIST (users_list), GTK_SHADOW_NONE);

    /* Phase 5: column 1 (Name) gets the Mac-classic overlay layout.
	 * Wide banner-style icons (Badmoon & co.) render as a row
	 * background and the name draws on top at a fixed offset from
	 * the cell edge — so names stay column-aligned regardless of
	 * how wide the user's icon happens to be.
	 *
	 * The offset clears the icon width plus a few px of breathing
	 * room. We bumped this from 22 to 36 in 2026-05 — the original
	 * value just cleared 16-18 px stock-icon widths, but the
	 * medium-wide "non-banner" tiles (UNIX = 500, a handful of
	 * others) are 24-32 px and were overlapping the name. 36 gives
	 * those room and still keeps banner-format icons (60+ px wide)
	 * rendering as backgrounds with the name overlaid on top —
	 * the Mac-classic look. */
    gtk_hlist_column_set_overlay_pixtext (GTK_HLIST (users_list), 1, 36);
    /* Pixel scale 1.25x on the Name column — multiplies both the
	 * icon dimensions and the font size. Combined with the 24 px
	 * row height above, the user list reads ~25% larger without
	 * affecting other tables (Tracker, Tasks, Files browser).
	 * Text outline draws a 1 px contrasting halo behind each name
	 * so light user colours (the pink default in particular) stay
	 * readable on busy banner-icon backgrounds. */
    gtk_hlist_column_set_overlay_decoration (GTK_HLIST (users_list), 1,
                                             /*pixel_scale=*/1.25,
                                             /*text_outline=*/TRUE);
    gtk_hlist_set_column_justification (GTK_HLIST (users_list), 1,
                                        GTK_JUSTIFY_LEFT);
    gtk_hlist_set_compare_func (GTK_HLIST (users_list),
                                (GtkHListCompareFunc)users_sort);
    g_signal_connect (users_list, "click_column", G_CALLBACK (usercol_clicked),
                      sess);
    /* Phase 4.5: button-press-event is gone in GTK 4. The gesture
	 * controller installed here dispatches double-clicks to the message
	 * window and right-clicks to the user popup. */
    users_attach_click_gesture (users_list, sess);

    if (!users_font_desc) {
        users_font_desc = pango_font_description_from_string ("Sans 10");
    }
    gtkhx_refresh_userlist_css (users_font_desc);
    gtkhx_apply_userlist_style (users_list);

    users_window_scroll = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (users_window_scroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
    /* Phase 5: dropped explicit 240x400 size_request; vexpand on
	 * the scroll lets it grow with the window naturally, and the
	 * 240px min width was a holdover that prevented the user from
	 * shrinking the window narrower than that. */
    gtk_widget_set_vexpand (users_window_scroll, TRUE);
    gtkhx_widget_set_child (users_window_scroll, users_list);

    /* Phase 5: per-user action buttons via the shared
	 * gtkhx_pixmap_button helper (2x scale, matches the toolbar). */
    msgbtn
        = gtkhx_pixmap_button ("/com/nasledov/gtkhx/pixmaps/msg.png", _ ("Msg"),
                               2, G_CALLBACK (open_message_btn), users_list);
    g_object_set_data (G_OBJECT (msgbtn), "sess", sess);

    kickbtn = gtkhx_pixmap_button ("/com/nasledov/gtkhx/pixmaps/kick.png",
                                   _ ("Kick"), 2, G_CALLBACK (user_kick_btn),
                                   users_list);
    g_object_set_data (G_OBJECT (kickbtn), "sess", sess);

    infobtn = gtkhx_pixmap_button ("/com/nasledov/gtkhx/pixmaps/info.png",
                                   _ ("User Info"), 2,
                                   G_CALLBACK (user_info_btn), users_list);
    g_object_set_data (G_OBJECT (infobtn), "sess", sess);

    banbtn
        = gtkhx_pixmap_button ("/com/nasledov/gtkhx/pixmaps/ban.png", _ ("Ban"),
                               2, G_CALLBACK (user_ban_btn), users_list);
    g_object_set_data (G_OBJECT (banbtn), "sess", sess);

    chatbtn = gtkhx_pixmap_button ("/com/nasledov/gtkhx/pixmaps/chat.png",
                                   _ ("Private Chat"), 2,
                                   G_CALLBACK (user_chat_btn), users_list);
    g_object_set_data (G_OBJECT (chatbtn), "sess", sess);

    ignobtn = gtkhx_pixmap_button ("/com/nasledov/gtkhx/pixmaps/ignore.png",
                                   _ ("Ignore"), 2, G_CALLBACK (user_igno_btn),
                                   users_list);
    g_object_set_data (G_OBJECT (ignobtn), "sess", sess);

    gtk_widget_set_sensitive (msgbtn, FALSE);
    gtk_widget_set_sensitive (kickbtn, FALSE);
    gtk_widget_set_sensitive (infobtn, FALSE);
    gtk_widget_set_sensitive (banbtn, FALSE);
    gtk_widget_set_sensitive (chatbtn, FALSE);
    gtk_widget_set_sensitive (ignobtn, FALSE);

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

    init_keyaccel (users_window);

    /* Phase 3.x: only apply saved geometry when the prefs file actually
	 * has one. On a fresh install the geo struct is zeroed, and calling
	 * set_size_request(window, 0, 0) collapses the window to 0x0 — under
	 * GTK 3 the compositor then refuses to map it and the window never
	 * appears. The earlier set_size_request(264, 400) call serves as
	 * the default size. */
    if (gtkhx_prefs.geo.users.xsize > 0 && gtkhx_prefs.geo.users.ysize > 0) {
        gtk_window_set_default_size (GTK_WINDOW (users_window),
                                     gtkhx_prefs.geo.users.xsize,
                                     gtkhx_prefs.geo.users.ysize);
    }
    if (gtkhx_prefs.geo.users.xpos > 0 || gtkhx_prefs.geo.users.ypos > 0) {
        /* Phase 4.2: gtk_window_move removed (Wayland) */
        gtk_window_present (GTK_WINDOW (users_window));
    }

    gtkhx_prefs.geo.users.open = 1;
    gtkhx_prefs.geo.users.init = 1;

    sess->users_list = users_list;
    sess->users_window = users_window;

    if (connected == 1) {
        changetitlespecific (users_window, _ ("Users"));
        gtk_widget_set_sensitive (msgbtn, TRUE);
        gtk_widget_set_sensitive (banbtn, TRUE);
        gtk_widget_set_sensitive (infobtn, TRUE);
        gtk_widget_set_sensitive (kickbtn, TRUE);
        gtk_widget_set_sensitive (chatbtn, TRUE);
        gtk_widget_set_sensitive (ignobtn, TRUE);
        user_list (sess);
    }
}

void
users_clear (struct htlc_conn *htlc, struct chat *chat)
{
    session *sess = &the_session;

    if (!sess->users_list || !gtkhx_prefs.geo.users.open) {
        return;
    }

    gtk_hlist_clear (GTK_HLIST (sess->users_list));
}

/* Phase 5 dark-theme: the four-slot palette is { regular, idle, admin,
 * admin-idle }. The "regular" slot used to be hard-coded black, which
 * is invisible against a dark theme's row background. Returning NULL
 * for that slot lets the cell renderer fall back to the GTK theme's
 * default foreground so regular users read on both light and dark
 * themes. The other three slots stay distinctive enough to convey
 * status under either theme. */
GdkRGBA *
user_color_gdk (guint16 color)
{
    if ((color % 4) == 0) {
        return NULL;
    }
    return &gdk_user_colors[color % 4];
}

/* Colored-Nicknames extension. When the user has a
 * server-supplied RGB nick color, fill the caller's GdkRGBA from
 * the 0x00RRGGBB value and return a pointer to it; otherwise
 * fall through to user_color_gdk's legacy status palette
 * (Admin/Guest/Away). gtk_hlist_set_foreground reads the channels
 * synchronously and formats them into a hex string for the cell
 * renderer, so a stack-allocated GdkRGBA is fine for the caller's
 * lifetime.
 *
 * `status` is the 2-bit status field that user_color_gdk reads —
 * passed explicitly rather than read off user->color because at
 * the call sites (users.c::user_create / user_change) the new
 * status from the wire hasn't been stamped onto user->color yet.
 * Reading user->color here would have rendered the row from the
 * OLD status (stale idle-dim, stale palette slot) until a later
 * unrelated USER_CHANGE rebuilt the row.
 *
 * Lookup priority: explicit nick_color > status palette > theme
 * default. The status palette still applies to away/admin
 * decoration when the user hasn't set their own color, matching
 * what users would expect from a colored-nicknames-unaware client.
 *
 * Returns NULL when nick_color is unset AND the status palette
 * resolves to the regular-user slot — user_color_gdk explicitly
 * returns NULL there so the caller falls through to the GTK
 * theme's default foreground (hard-coding black would be
 * invisible on dark themes). gtk_hlist_set_foreground treats NULL
 * as "use theme default", so call sites can pass the result
 * through unconditionally. */
GdkRGBA *
user_nick_color_gdk (const struct hx_user *user, guint16 status, GdkRGBA *out)
{
    if (user && user->nick_color != HX_NICK_COLOR_NONE && out) {
        double r = ((user->nick_color >> 16) & 0xff) / 255.0;
        double g = ((user->nick_color >> 8) & 0xff) / 255.0;
        double b = (user->nick_color & 0xff) / 255.0;
        /* status bit 0 = idle/away, bit 1 = admin. When the user
		 * is idle, dim their custom color so the idle indication
		 * still reads visually — without this an away user with a
		 * vibrant nick_color would look just as "alive" as an
		 * active user. 0.55 is roughly what the legacy
		 * gdk_user_colors[1] (idle) and [3] (admin-idle) slots use
		 * relative to their non-idle siblings. */
        if (status & 1) {
            r *= 0.55;
            g *= 0.55;
            b *= 0.55;
        }
        out->red = r;
        out->green = g;
        out->blue = b;
        out->alpha = 1.0;
        return out;
    }
    return user_color_gdk (status);
}

void
user_create (struct htlc_conn *htlc, struct chat *chat, struct hx_user *user,
             const char *nam, guint16 icon, guint16 color)
{
    GdkPixmap *pixmap;
    GdkBitmap *mask;
    gint row;
    session *sess = &the_session;
    GtkWidget *list_widget = gtkhx_prefs.geo.users.open ? sess->users_list : 0;
    gchar *nulls[2];
    struct gtkhx_chat *gchat;

    if (chat->cid) {
        gchat = gchat_with_cid (sess, chat->cid);
        if (!gchat) {
            gchat = create_pchat_window (htlc, chat);
        }
        list_widget = gchat->userlist;
    }

    if (!list_widget) {
        return;
    }

    nulls[0] = g_strdup_printf ("%u", user->uid);
    nulls[1] = 0;
    row = gtk_hlist_append (GTK_HLIST (list_widget), nulls);
    gtk_hlist_set_row_data (GTK_HLIST (list_widget), row, user);
    g_free (nulls[0]);
    /* Colored-Nicknames: prefer the per-user RGB
	 * nick_color, fall back to the status palette via the helper.
	 * Pass the caller's `color` arg as the status bitmap rather
	 * than reading user->color — at this point we're rendering the
	 * NEW state, but rcv.c stamps user->color AFTER emitting the
	 * signal, so user->color would still hold the previous value.
	 * Stack-allocated GdkRGBA is fine — gtk_hlist_set_foreground
	 * reads the channels synchronously into a hex string. */
    GdkRGBA fg_buf;
    GdkRGBA *fg = user_nick_color_gdk (user, color, &fg_buf);
    gtk_hlist_set_foreground (GTK_HLIST (list_widget), row, fg);
    load_icon (list_widget, icon, &icon_files, 1, &pixmap, &mask);
    if (!pixmap) {
        gtk_hlist_set_text (GTK_HLIST (list_widget), row, 1, nam);
    } else {
        gtk_hlist_set_pixtext (GTK_HLIST (list_widget), row, 1, nam, 34, pixmap,
                               mask);
    }
}

void
user_delete (struct htlc_conn *htlc, struct chat *chat, struct hx_user *user)
{
    gint row;
    struct gtkhx_chat *gchat;
    session *sess = &the_session;
    GtkWidget *list_widget = gtkhx_prefs.geo.users.open ? sess->users_list : 0;

    if (chat->cid) {
        gchat = gchat_with_cid (sess, chat->cid);
        if (!gchat) {
            return;
        }
        list_widget = gchat->userlist;
    }

    if (!list_widget) {
        return;
    }

    row = gtk_hlist_find_row_from_data (GTK_HLIST (list_widget), user);
    gtk_hlist_remove (GTK_HLIST (list_widget), row);
}

void
user_change (struct htlc_conn *htlc, struct chat *chat, struct hx_user *user,
             const char *nam, guint16 icon, guint16 color)
{
    GdkPixmap *pixmap;
    GdkBitmap *mask;
    gint row;
    gchar *rowdat[2];
    struct gtkhx_chat *gchat;
    session *sess = &the_session;
    GtkWidget *list_widget = gtkhx_prefs.geo.users.open ? sess->users_list : 0;

    if (chat->cid) {
        gchat = gchat_with_cid (sess, chat->cid);
        if (!gchat) {
            gchat = create_pchat_window (&sess->htlc, chat);
        }
        list_widget = gchat->userlist;
    }

    if (!list_widget) {
        return;
    }

    if (!chat->cid && sess->gchats) {
        GHashTableIter iter;
        gpointer key, val;
        g_hash_table_iter_init (&iter, sess->gchats);
        while (g_hash_table_iter_next (&iter, &key, &val)) {
            if (GPOINTER_TO_UINT (key) == 0) {
                continue;
            }
            gchat = val;
            struct hx_user *u = hx_user_with_uid (gchat->chat, user->uid);
            if (u) {
                user_change (&sess->htlc, gchat->chat, u, nam, icon, color);
            }
        }
    }

    rowdat[0] = g_strdup_printf ("%u", user->uid);
    rowdat[1] = 0;
    row = gtk_hlist_find_row_from_data (GTK_HLIST (list_widget), user);
    gtk_hlist_remove (GTK_HLIST (list_widget), row);
    gtk_hlist_insert (GTK_HLIST (list_widget), row, rowdat);
    g_free (rowdat[0]);
    gtk_hlist_set_row_data (GTK_HLIST (list_widget), row, user);
    /* See user_create — same nick_color-prefer-status-fallback,
	 * same reason to pass `color` (the freshly-parsed status) rather
	 * than rely on user->color (which rcv.c stamps later). */
    GdkRGBA fg_buf;
    GdkRGBA *fg = user_nick_color_gdk (user, color, &fg_buf);
    gtk_hlist_set_foreground (GTK_HLIST (list_widget), row, fg);
    load_icon (sess->users_window, icon, &icon_files, 1, &pixmap, &mask);
    if (!pixmap) {
        gtk_hlist_set_text (GTK_HLIST (list_widget), row, 1, nam);
    } else {
        gtk_hlist_set_pixtext (GTK_HLIST (list_widget), row, 1, nam, 34, pixmap,
                               mask);
    }

    /* Phase 5: if this user has an open PM window, refresh its info
	 * pane (icon / name / status) so it tracks the user changing
	 * their nick or going idle. Only fires on the global-list pass
	 * (cid==0) to avoid duplicate refreshes from the per-pchat
	 * recursion above. We pass the new nam/icon/color through
	 * directly rather than re-reading user->* — rcv.c hasn't
	 * patched the new values onto the cached struct yet at this
	 * point in the dispatch (its rename-detection compares
	 * user->name vs nam after we return), so a cache-lookup
	 * refresh would paint the OLD identity. */
    if (!chat->cid) {
        struct msgwin *mw = msgwin_with_uid (user->uid);
        if (mw) {
            msgwin_apply_user_change (mw, nam, icon, color);
        }
    }
}
