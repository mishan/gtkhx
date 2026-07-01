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
#include <libpanel.h>
#include <netinet/in.h>
#include <ctype.h>
#include "hx.h"
#include "gtkhx_session.h"
#include "gtkhx_theme.h"
#include "hx_panel.h"
#include "panel_registry.h"
#include "toolbar.h"
#include "hl_access.h"
#include "cicn.h"
#include "hotline_proto.h"
#include "network.h"
#include "proto_helpers.h" /* struct hx_chunk (stack-allocated below) */
#include "chat.h"
#include "gtkhx.h"
#include "msg.h"
#include "chat_tabs.h"
#include "gtkutil.h"
#include "tasks.h"
#include "rcv.h"
#include "users.h"
#include "users_view.h"
#include "voice_panel.h" /* voice_panel_new — Join/Mute icon controls */
#include "gif_avatar.h" /* gtkhx_avatar_is_animated / _is_paused / _set_paused */

/* Every userlist call site is HxUserListView-backed; selection
 * comes from the view's GtkSingleSelection, right-click pops via
 * the view's internal gesture, and the shared view_*_btn handlers
 * below drive both the Users window and the pchat sidebars. */

PangoFontDescription *users_font_desc;

GdkRGBA user_colors[8];
GdkRGBA gdk_user_colors[4];

GtkWidget *msgbtn, *kickbtn, *infobtn, *banbtn, *chatbtn, *ignobtn;

void
hx_change_name_icon (struct htlc_conn *htlc)
{
    /* encode the nick to the negotiated wire encoding.
     * is_body = FALSE — nicks don't have line endings; we want the
     * Mac-Roman transcoding (or UTF-8 passthrough) without the
     * LF→CR substitution. */
    gboolean utf8 = (htlc->caps & HTLC_CAP_TEXT_ENCODING) != 0;
    gsize name_len = 0;
    char *name_wire
        = gtkhx_text_for_wire ((const char *)htlc->name, strlen (htlc->name),
                               utf8, /*is_body=*/FALSE, &name_len);

    /* chunk layout moved to gtkhx_proto_build_user_change
     * _chunks. Colored-Nicknames extension: include DATA_COLOR ONLY
     * when the local pref has set a real color — we deliberately
     * don't send HX_NICK_COLOR_NONE because the spec's auto-opt-in
     * marks the session as color-aware on first DATA_COLOR receipt
     * regardless of the value, and a "no color" client shouldn't opt
     * in. Servers that don't know the extension ignore the trailing
     * chunk; supporting servers mark us color-aware and start
     * decorating other users' USER_CHANGE pushes for us. */
    bool has_color = htlc->nick_color != HX_NICK_COLOR_NONE;
    struct hx_chunk chunks[3];
    guint8 scratch[6];
    int hc = (int)gtkhx_proto_build_user_change_chunks (
        htlc->icon, (const uint8_t *)name_wire, name_len, has_color ? 1 : 0,
        htlc->nick_color, chunks, G_N_ELEMENTS (chunks), scratch,
        sizeof (scratch));
    if (hc > 0) {
        hlwrite_chunks (htlc, HTLC_HDR_USER_CHANGE, 0, chunks, hc);
    }
    g_free (name_wire);
}

void
hx_kick_user (struct htlc_conn *htlc, guint16 uid, guint16 ban)
{
    /* chunk layout moved to gtkhx_proto_build_user_kick_chunks.
     * Build BEFORE task_new — see hx_send_msg for the rationale
     * (task_new snapshots htlc->trans into a pending entry; the
     * increment happens inside hlpack_chunks during packing). A
     * builder failure must not leave a phantom task behind. */
    struct hx_chunk chunks[2];
    guint8 scratch[4];
    int hc = (int)gtkhx_proto_build_user_kick_chunks (
        uid, ban, chunks, G_N_ELEMENTS (chunks), scratch, sizeof (scratch));
    if (hc > 0) {
        task_new (htlc, RCV_TASK_FN (rcv_task_kick), 0, 0, "kick");
        hlwrite_chunks (htlc, HTLC_HDR_USER_KICK, 0, chunks, hc);
    }
}

void
hx_get_user_info (struct htlc_conn *htlc, guint16 uid)
{
    /* chunk layout moved to gtkhx_proto_build_user_getinfo
	 * _chunks. Same build-before-task ordering as hx_kick_user. */
    struct hx_chunk chunks[1];
    guint8 scratch[2];
    int hc = (int)gtkhx_proto_build_user_getinfo_chunks (
        uid, chunks, G_N_ELEMENTS (chunks), scratch, sizeof (scratch));
    if (hc > 0) {
        guint16 *_uid = g_malloc (sizeof (guint16));
        *_uid = uid;
        task_new (htlc, RCV_TASK_FN (rcv_task_user_info), (void *)_uid, 0,
                  "info");
        hlwrite_chunks (htlc, HTLC_HDR_USER_GETINFO, 0, chunks, hc);
    }
}

/* per-chat user list lives in chat->users, a
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
        /* Existing msgwin — just raise its tab inside the Chat
         * panel. The Chat panel itself gets attached / raised by
         * gtkhx_chat_tabs_raise_msg if it's hidden. */
        gtkhx_chat_tabs_raise_msg (ctx->user->uid);
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

/* Toggle this user's GIF-avatar animation (Phase 10.D). Only offered
 * when the user actually has an animated avatar (see user_popup_show). */
static void
on_user_toggle_anim (GSimpleAction *action, GVariant *param, gpointer user_data)
{
    struct UserActionCtx *ctx = user_data;
    (void)action;
    (void)param;
    if (!ctx->user) {
        return;
    }
    gtkhx_avatar_set_paused (ctx->user->uid,
                             !gtkhx_avatar_is_paused (ctx->user->uid));
}

/* Tthe GActionEntry table that drove the old GtkPopoverMenu
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
    /* keyboard focus on the first button auto-paints a
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

/* The user_popup_show entry point is what HxUserListView's
 * right-click gesture and any future in-file callers use; the
 * old static `user_popup' wrapper retired with the legacy
 * user_pressed handler (Phase C). */
void
user_popup_show (GtkWidget *anchor, struct hx_user *user, session *sess,
                 double x, double y)
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

    /* GIF-icons (Phase 10.D): pause / resume this user's animated
	 * avatar. Only shown when they actually have an animated one — the
	 * discoverable counterpart to clicking the avatar directly. */
    if (gtkhx_avatar_is_animated (user->uid)) {
        gtk_box_append (GTK_BOX (vbox),
                        gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
        user_popup_append_button (
            GTK_BOX (vbox), GTK_POPOVER (popover), ctx,
            gtkhx_avatar_is_paused (user->uid) ? _ ("Resume Animation")
                                               : _ ("Pause Animation"),
            on_user_toggle_anim);
    }

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

/* HxUserListView installs its own GtkGestureClick for right-click
 * (see users_view.c::on_view_secondary_press) and its
 * GtkSortListModel + GtkCustomSorter pair handles header-click
 * sorting. The shared view_*_btn handlers (above) drive every
 * userlist button site. */

/* Phase 5 / Phase D: AdwAlertDialog with three responses
 * (Cancel / New / Invite) replaces the hand-rolled GtkDialog. A
 * GtkListBox of existing pchat sessions goes in extra-child. The
 * response handler dispatches by id: "invite" reads the listbox's
 * selected row's stashed cid and calls hx_invite_user, "new" creates
 * a fresh pchat via hx_chat_user, "cancel" does nothing.
 *
 * State carried through user_data: sess + uid + listbox. Selection
 * tracking lives on the GtkListBox itself — no separate selected_row
 * counter. Each row is a GtkListBoxRow with a horizontal label pair
 * (cid in monospace, subject right-aligned) and the cid stashed as
 * "pchat-cid" qdata for O(1) read-back.
 *
 * Lifetime: ctx is owned by the dialog GObject via
 * g_object_set_data_full. The destroy-notify (prompt_chat_ctx_free)
 * runs when the dialog itself is finalised — strictly AFTER both
 * "response" and "closed" have fired and their handlers have
 * returned. Earlier versions wired ctx to a "closed" handler that
 * g_free'd it directly, which raced ahead of "response" under
 * re-entrant dialog conditions and crashed in hx_chat_user via
 * &ctx->sess->htlc reading freed memory. */
struct prompt_chat_ctx {
    session *sess;
    guint16 uid;
    GtkWidget *listbox; /* the GtkListBox of existing pchats */
};

static void
prompt_chat_ctx_free (gpointer data)
{
    g_free (data);
}

static void
prompt_chat_response (AdwAlertDialog *dialog, const char *response,
                      gpointer data)
{
    struct prompt_chat_ctx *ctx = data;
    (void)dialog;

    if (g_strcmp0 (response, "invite") == 0) {
        GtkListBoxRow *row
            = gtk_list_box_get_selected_row (GTK_LIST_BOX (ctx->listbox));
        if (!row) {
            return;
        }
        guint32 chat_cid
            = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (row), "pchat-cid"));
        hx_invite_user (&ctx->sess->htlc, ctx->uid, chat_cid);
    } else if (g_strcmp0 (response, "new") == 0) {
        hx_chat_user (&ctx->sess->htlc, ctx->uid);
    }
}

/* Build one GtkListBoxRow for a pchat. Layout: monospace cid label
 * on the start side (fixed-ish width via xalign + width-chars), the
 * subject as a single-line ellipsised label filling the rest. The cid
 * lives on the row as qdata for the response handler. */
static GtkWidget *
prompt_chat_make_row (guint32 cid, const char *subject)
{
    GtkWidget *row = gtk_list_box_row_new ();
    GtkWidget *hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *cid_lbl;
    GtkWidget *subj_lbl;
    char *cid_str = g_strdup_printf ("0x%08x", cid);

    gtk_widget_set_margin_start (hbox, 8);
    gtk_widget_set_margin_end (hbox, 8);
    gtk_widget_set_margin_top (hbox, 4);
    gtk_widget_set_margin_bottom (hbox, 4);

    cid_lbl = gtk_label_new (cid_str);
    gtk_label_set_xalign (GTK_LABEL (cid_lbl), 0.0f);
    gtk_widget_add_css_class (cid_lbl, "monospace");
    g_free (cid_str);

    subj_lbl = gtk_label_new (subject ? subject : "");
    gtk_label_set_xalign (GTK_LABEL (subj_lbl), 0.0f);
    gtk_label_set_ellipsize (GTK_LABEL (subj_lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand (subj_lbl, TRUE);

    gtk_box_append (GTK_BOX (hbox), cid_lbl);
    gtk_box_append (GTK_BOX (hbox), subj_lbl);
    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), hbox);

    g_object_set_data (G_OBJECT (row), "pchat-cid", GUINT_TO_POINTER (cid));
    return row;
}

static void
prompt_chat (session *sess, guint16 _uid)
{
    AdwDialog *dialog;
    GtkWidget *listbox, *scroll;
    struct gtkhx_chat *gchat;
    struct prompt_chat_ctx *ctx;

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

    /* GtkListBox in BROWSE selection mode: exactly one row is
     * selected at a time (single-select), the user can't unselect
     * by re-clicking the same row, and arrow-key navigation moves
     * the selection. .boxed-list gives us the Adwaita rounded-card
     * look that fits an alert-dialog extra-child. */
    listbox = gtk_list_box_new ();
    gtk_list_box_set_selection_mode (GTK_LIST_BOX (listbox),
                                     GTK_SELECTION_BROWSE);
    gtk_widget_add_css_class (listbox, "boxed-list");
    gtkhx_widget_set_child (scroll, listbox);
    gtk_widget_set_size_request (scroll, 350, 200);

    if (sess->gchats) {
        GHashTableIter iter;
        gpointer val;
        g_hash_table_iter_init (&iter, sess->gchats);
        while (g_hash_table_iter_next (&iter, NULL, &val)) {
            gchat = val;
            if (!gchat->cid) {
                continue;
            }
            GtkWidget *row
                = prompt_chat_make_row (gchat->chat->cid, gchat->chat->subject);
            gtk_list_box_append (GTK_LIST_BOX (listbox), row);
        }
    }
    /* Pre-select the first row so the default "Invite" response
     * always has a target, even if the user hits Enter without
     * touching the list. */
    {
        GtkListBoxRow *first
            = gtk_list_box_get_row_at_index (GTK_LIST_BOX (listbox), 0);
        if (first) {
            gtk_list_box_select_row (GTK_LIST_BOX (listbox), first);
        }
    }
    ctx->listbox = listbox;

    adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dialog), scroll);

    g_signal_connect (dialog, "response", G_CALLBACK (prompt_chat_response),
                      ctx);
    /* Anchor ctx to the dialog GObject's qdata — destroy-notify runs
     * at finalize, strictly after both "response" and "closed" have
     * already fired. */
    g_object_set_data_full (G_OBJECT (dialog), "prompt-chat-ctx", ctx,
                            prompt_chat_ctx_free);

    adw_dialog_present (dialog, sess && sess->users_window
                                    ? GTK_WIDGET (sess->users_window)
                                    : NULL);
}

/* close_users_window dropped — the
 * standalone Users GtkWindow is gone, and the Users panel is a
 * permanent resident of the toolbar's sidebar PanelFrame. An
 * undocked Users panel's "X" affordance flows through libpanel's
 * own tab-close + Redock-on-close machinery in Phase 4. */

void
user_list (session *sess)
{
    struct chat *pub;

    /* users_view, not users_window,
     * is the canonical "have we got a view?" signal now. The
     * standalone window is gone — the panel is always either
     * registered or not. */
    if (!sess->users_view) {
        return;
    }

    pub = chat_with_cid (sess, 0);
    hx_user_list_view_clear (sess->users_view);
    if (pub && pub->users) {
        GHashTableIter iter;
        gpointer val;
        g_hash_table_iter_init (&iter, pub->users);
        while (g_hash_table_iter_next (&iter, NULL, &val)) {
            struct hx_user *user = val;
            /* Emit via the signal so any listener (incl. the
			 * per-pchat sidebars when the cid==0 broadcast lands
			 * on them downstream) sees a single canonical add. */
            gtkhx_session_emit_user_create (gtkhx_session_get_default (),
                                            &sess->htlc, pub, user, user->name,
                                            user->icon, user->color);
        }
    }
}

/* ----------------------------------------------------------------
 * Headerbar / sidebar button handlers, shared between the standalone
 * Users window and the pchat sidebars in chat.c.
 *
 * The `data' parameter is the HxUserListView* the button is attached
 * to. view_selected_user reads the current single-selection; the
 * session pointer is borrowed from the view (set at construction). */
static struct hx_user *
view_selected_user (gpointer data)
{
    HxUserListView *v = data;
    if (!v) {
        return NULL;
    }
    return hx_user_list_view_get_selected_user (v);
}

static session *
view_session (gpointer data)
{
    HxUserListView *v = data;
    return v ? hx_user_list_view_get_session (v) : NULL;
}

void
view_msg_btn (GtkWidget *w, gpointer data)
{
    struct hx_user *user = view_selected_user (data);
    struct msgwin *mw;
    (void)w;
    if (!user) {
        return;
    }
    mw = msgwin_with_uid (user->uid);
    if (mw) {
        gtkhx_chat_tabs_raise_msg (user->uid);
    } else {
        create_msgwin (user->uid, user->name);
    }
}

void
view_info_btn (GtkWidget *w, gpointer data)
{
    struct hx_user *user = view_selected_user (data);
    session *sess = view_session (data);
    (void)w;
    if (!user || !sess) {
        return;
    }
    hx_get_user_info (&sess->htlc, user->uid);
}

void
view_kick_btn (GtkWidget *w, gpointer data)
{
    struct hx_user *user = view_selected_user (data);
    session *sess = view_session (data);
    (void)w;
    if (!user || !sess) {
        return;
    }
    hx_kick_user (&sess->htlc, user->uid, 0);
}

void
view_ban_btn (GtkWidget *w, gpointer data)
{
    struct hx_user *user = view_selected_user (data);
    session *sess = view_session (data);
    (void)w;
    if (!user || !sess) {
        return;
    }
    hx_kick_user (&sess->htlc, user->uid, 1);
}

void
view_igno_btn (GtkWidget *w, gpointer data)
{
    struct hx_user *user = view_selected_user (data);
    session *sess = view_session (data);
    (void)w;
    if (!user || !sess) {
        return;
    }
    user->ignore ^= 1;
    hx_printf_prefix (&sess->htlc, 0, INFOPREFIX,
                      user->ignore ? _ ("ignore: %s is now ignored\n")
                                   : _ ("ignore: %s is now unignored"),
                      user->name);
}

void
view_chat_btn (GtkWidget *w, gpointer data)
{
    struct hx_user *user = view_selected_user (data);
    session *sess = view_session (data);
    int with_cid = 0;
    (void)w;

    if (!user || !sess) {
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

void
create_users_window (GtkWidget *parent_window, gpointer data)
{
    GtkWidget *scroll;
    GtkWidget *cv_widget;
    GtkWidget *content_vbox;
    GtkWidget *button_bar;
    HxUserListView *view;
    HxPanel *panel;
    session *sess = data;

    /* the parent_window argument is
     * vestigial — we don't reparent the panel to it (it's already
     * a resident of the toolbar's sidebar frame). Kept on the
     * signature so the existing gtkhx.c auto-open and toolbar
     * button call sites compile unchanged. */
    (void)parent_window;

    /* the standalone Users GtkWindow
     * is gone. The panel is a permanent resident of the toolbar
     * window's start-area PanelFrame — first call constructs it
     * and slots it in; later calls just raise it to focus. */
    panel = hx_panel_registry_lookup (HX_PANEL_ID_USERS);
    if (panel != NULL) {
        hx_panel_ensure_attached (panel);
        panel_widget_raise (PANEL_WIDGET (panel));
        return;
    }

    /* HxUserListView wraps a GtkColumnView with a custom
     * snapshot()-rendered Name cell that gives us the Mac-classic
     * icon-as-background + name-on-top look. STYLE_USERS picks the
     * standalone window's chrome: 24-px row height, 1.25× pixel
     * scale, text outline, 36-px text offset. The view also installs
     * its own right-click gesture that pops user_popup. */
    view = hx_user_list_view_new (sess, HX_USER_LIST_STYLE_USERS);
    cv_widget = hx_user_list_view_get_widget (view);

    if (!users_font_desc) {
        users_font_desc = pango_font_description_from_string ("Sans 10");
    }
    /* Refresh the CSS provider that paints the .gtkhx-userlist
	 * class so the global font / fg / bg tracking the rest of the
	 * app does also covers our column-view cells. The view applied
	 * the class to its column_view widget at construction. */
    gtkhx_refresh_userlist_css (users_font_desc);

    scroll = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
    gtk_widget_set_vexpand (scroll, TRUE);
    gtkhx_widget_set_child (scroll, cv_widget);

    /* Per-user action buttons. `data' is the HxUserListView itself —
	 * the view_*_btn handlers read its current single-selection and
	 * its borrowed session pointer. Same handlers drive the chat.c
	 * pchat sidebars; see view_msg_btn above for the shape. */
    msgbtn = gtkhx_pixmap_button ("/com/nasledov/gtkhx/pixmaps/message.png",
                                  _ ("Msg"), GTKHX_SCALE_WINDOW_BUTTONS,
                                  G_CALLBACK (view_msg_btn), view);
    kickbtn = gtkhx_pixmap_button ("/com/nasledov/gtkhx/pixmaps/kick.png",
                                   _ ("Kick"), GTKHX_SCALE_WINDOW_BUTTONS,
                                   G_CALLBACK (view_kick_btn), view);
    infobtn = gtkhx_pixmap_button ("/com/nasledov/gtkhx/pixmaps/info.png",
                                   _ ("User Info"), GTKHX_SCALE_WINDOW_BUTTONS,
                                   G_CALLBACK (view_info_btn), view);
    banbtn = gtkhx_pixmap_button ("/com/nasledov/gtkhx/pixmaps/ban.png",
                                  _ ("Ban"), GTKHX_SCALE_WINDOW_BUTTONS,
                                  G_CALLBACK (view_ban_btn), view);
    chatbtn = gtkhx_pixmap_button ("/com/nasledov/gtkhx/pixmaps/chat.png",
                                   _ ("Private Chat"), GTKHX_SCALE_WINDOW_BUTTONS,
                                   G_CALLBACK (view_chat_btn), view);
    ignobtn = gtkhx_pixmap_button ("/com/nasledov/gtkhx/pixmaps/ignore.png",
                                   _ ("Ignore"), GTKHX_SCALE_WINDOW_BUTTONS,
                                   G_CALLBACK (view_igno_btn), view);

    gtk_widget_set_sensitive (msgbtn, FALSE);
    gtk_widget_set_sensitive (kickbtn, FALSE);
    gtk_widget_set_sensitive (infobtn, FALSE);
    gtk_widget_set_sensitive (banbtn, FALSE);
    gtk_widget_set_sensitive (chatbtn, FALSE);
    gtk_widget_set_sensitive (ignobtn, FALSE);

    /* the headerbar in the old
     * standalone Users window held Msg / Chat on the start and Ignore /
     * Ban / Kick / Info on the end. A PanelWidget's tab strip is too
     * narrow to host action buttons, so they relocate to a slim
     * GtkBox at the top of the panel content. Spacing + halign keep
     * the start / end grouping that the headerbar layout implied. */
    button_bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start (button_bar,  6);
    gtk_widget_set_margin_end   (button_bar,  6);
    gtk_widget_set_margin_top   (button_bar,  6);
    gtk_widget_set_margin_bottom (button_bar, 4);
    gtk_box_append (GTK_BOX (button_bar), msgbtn);
    gtk_box_append (GTK_BOX (button_bar), chatbtn);
#ifdef HAVE_VOICE
    /* Public-room (cid 0) voice Join/Leave + Mute icon controls.
     * The controls live with the user list rather than the chat
     * window; hidden entirely unless the server echoed
     * HTLC_CAP_VOICE. Grouped with Msg/Chat on the start side. */
    gtk_box_append (GTK_BOX (button_bar), voice_panel_new (sess, 0));
#endif
    {
        GtkWidget *spacer = gtk_label_new (NULL);
        gtk_widget_set_hexpand (spacer, TRUE);
        gtk_box_append (GTK_BOX (button_bar), spacer);
    }
    gtk_box_append (GTK_BOX (button_bar), infobtn);
    gtk_box_append (GTK_BOX (button_bar), kickbtn);
    gtk_box_append (GTK_BOX (button_bar), banbtn);
    gtk_box_append (GTK_BOX (button_bar), ignobtn);

    content_vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append (GTK_BOX (content_vbox), button_bar);
    gtk_box_append (GTK_BOX (content_vbox), scroll);

    /* The panel itself. SIDEBAR + PANEL_AREA_END routes it to the
     * toolbar window's right-side PanelFrame on the (Phase 4)
     * Redock path and tells the registry how to home it after an
     * Undock. */
    panel = hx_panel_new (HX_PANEL_ID_USERS,
                          HX_PANEL_KIND_SIDEBAR,
                          PANEL_AREA_END);
    panel_widget_set_title     (PANEL_WIDGET (panel), _ ("Users"));
    panel_widget_set_icon_name (PANEL_WIDGET (panel),
                                "system-users-symbolic");
    panel_widget_set_child     (PANEL_WIDGET (panel), content_vbox);

    /* Side areas auto-collapse when empty (Phase 0 finding #5).
     * Toggling the side area reveal is the toolbar button's job. */
    if (toolbar_end_frame != NULL) {
        panel_frame_add (PANEL_FRAME (toolbar_end_frame),
                         PANEL_WIDGET (panel));
        hx_panel_set_home_frame (panel, toolbar_end_frame);
    } else {
        g_critical ("create_users_window: toolbar dock not built yet");
    }

    /* hx_panel_registry_register strong-refs the panel so it
     * survives a Close-all-pages on its frame: libpanel's close path
     * destroys the AdwTabPage (which drops both the page->child ref
     * and the bin's parent-child ref), so the registry's ref is the
     * only thing keeping the widget alive between close and the
     * next toolbar-button-driven re-attach.
     *
     * Do NOT g_object_unref(panel) after register. hx_panel_new's
     * initial ref is the GTK4 floating ref, which panel_frame_add
     * already claimed via gtk_widget_set_parent's g_object_ref_sink
     * (clears floating, no new ref). Unrefing here drops the
     * registry's owning ref instead — the table holds the pointer
     * but no ownership, and the next close destroys the panel out
     * from under it. */
    hx_panel_registry_register (panel);

    sess->users_view = view;

    /* Auto-open + size persistence are Phase 4 work (layout
     * restore). For Phase 2 we just present the panel and trust
     * libpanel's defaults for sidebar width. */
    gtkhx_prefs.geo.users.open = 1;
    gtkhx_prefs.geo.users.init = 1;

    if (connected == 1) {
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
    session *sess = sess_from_htlc (htlc);
    (void)htlc;
    (void)chat;

    if (!sess->users_view || !gtkhx_prefs.geo.users.open) {
        return;
    }

    hx_user_list_view_clear (sess->users_view);
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
    /* Theme-supplied overrides win when set. The 4-slot status
	 * palette (active/idle/admin/admin-idle) maps 1:1 onto
	 * GtkhxUserColor by `color % 4`. Solarized (and any theme that
	 * sets [users.light]/[users.dark]) keeps names readable against
	 * its themed listview background — without an override, names
	 * inherit the GTK foreground via gtk_widget_get_color in the
	 * snapshot path, which on a themed row reads as gray/cream and
	 * clashes with the rest of the chrome.
	 *
	 * The buffer is static — every caller copies the GdkRGBA out
	 * of the returned pointer before doing anything else with it
	 * (cell renderer paths in users_view.c and msg.c both
	 * dereference once for the snapshot or Pango span). User-list
	 * rendering is main-thread-only, so no concurrent reads. */
    static GdkRGBA themed;
    AdwStyleManager *sm = adw_style_manager_get_default ();
    gboolean dark = sm ? adw_style_manager_get_dark (sm) : FALSE;
    GtkhxUserColor slot = (GtkhxUserColor)(color % 4);

    if (gtkhx_theme_get_user_color (slot, dark, &themed)) {
        return &themed;
    }

    /* No theme override → preserve historical behaviour: regular
	 * users use the GTK foreground (NULL → caller falls back to
	 * gtk_widget_get_color), admin/idle/admin-idle use the hardcoded
	 * gdk_user_colors[] palette. */
    if (slot == GTKHX_USER_COLOR_ACTIVE) {
        return NULL;
    }
    return &gdk_user_colors[color % 4];
}

/* Colored-Nicknames extension. When the user has a
 * server-supplied RGB nick color, fill the caller's GdkRGBA from
 * the 0x00RRGGBB value and return a pointer to it; otherwise
 * fall through to user_color_gdk's status palette
 * (Admin/Guest/Away). The cell renderer copies the colour into the
 * row immediately so a stack-allocated GdkRGBA buffer at the call
 * site is fine.
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
 * invisible on dark themes). The renderer treats NULL as
 * "use theme default", so call sites can pass the result
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
    session *sess = sess_from_htlc (htlc);
    struct gtkhx_chat *gchat;

    if (chat->cid) {
        /* Per-pchat sidebar — HxUserListView, STYLE_CHAT. The
		 * pchat window's userlist GObject is created lazily by
		 * create_pchat_window (Phase C) the first time a user
		 * shows up in this chat. */
        gchat = gchat_with_cid (sess, chat->cid);
        if (!gchat) {
            gchat = create_pchat_window (htlc, chat);
        }
        if (!gchat || !gchat->userlist) {
            return;
        }
        hx_user_list_view_add (gchat->userlist, user, nam, icon, color);
        return;
    }

    /* chat->cid == 0 — standalone Users window. The view's own
	 * bookkeeping (GListStore + GtkSortListModel) does the insert +
	 * position; the row computes its own foreground via
	 * user_nick_color_gdk from the `color` arg. */
    (void)htlc;
    if (!sess->users_view || !gtkhx_prefs.geo.users.open) {
        return;
    }
    hx_user_list_view_add (sess->users_view, user, nam, icon, color);
}

void
user_delete (struct htlc_conn *htlc, struct chat *chat, struct hx_user *user)
{
    struct gtkhx_chat *gchat;
    session *sess = sess_from_htlc (htlc);

    (void)htlc;
    if (chat->cid) {
        gchat = gchat_with_cid (sess, chat->cid);
        if (!gchat || !gchat->userlist) {
            return;
        }
        hx_user_list_view_remove (gchat->userlist, user);
        return;
    }

    /* chat->cid == 0 — standalone Users window. */
    if (!sess->users_view || !gtkhx_prefs.geo.users.open) {
        return;
    }
    hx_user_list_view_remove (sess->users_view, user);
}

void
user_change (struct htlc_conn *htlc, struct chat *chat, struct hx_user *user,
             const char *nam, guint16 icon, guint16 color)
{
    struct gtkhx_chat *gchat;
    session *sess = sess_from_htlc (htlc);

    (void)htlc;

    if (chat->cid) {
        gchat = gchat_with_cid (sess, chat->cid);
        if (!gchat) {
            gchat = create_pchat_window (&sess->htlc, chat);
        }
        if (!gchat || !gchat->userlist) {
            return;
        }
        /* In-place state mutation: HxUserRow::set_state fires its
		 * "changed" signal, the sort model re-orders, the cell
		 * re-snapshots. The row keeps its GObject identity so the
		 * sidebar selection stays on the same user across rename or
		 * icon change. */
        hx_user_list_view_update (gchat->userlist, user, nam, icon, color);
        return;
    }

    /* chat->cid == 0 — fan out the change into per-pchat sidebars
	 * AND into the standalone Users window. The fan-out happens
	 * BEFORE the Users-window update so the recursion shape is the
	 * same as the legacy code; both arms compute the foreground
	 * from the freshly-parsed `color`. */
    if (sess->gchats) {
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

    if (sess->users_view && gtkhx_prefs.geo.users.open) {
        /* HxUserListView's update is an in-place state mutation
		 * (HxUserRow::set_state fires "changed", the sort model
		 * re-orders, the column-view cell re-snapshots). The row
		 * keeps its GObject identity, so the live selection stays
		 * on the same user across rename or icon change. */
        hx_user_list_view_update (sess->users_view, user, nam, icon, color);
    }

    /* if this user has an open PM window, refresh its info
	 * pane (icon / name / status) so it tracks the user changing
	 * their nick or going idle. We pass the new nam/icon/color
	 * through directly rather than re-reading user->* — rcv.c
	 * hasn't patched the new values onto the cached struct yet at
	 * this point in the dispatch (its rename-detection compares
	 * user->name vs nam after we return), so a cache-lookup
	 * refresh would paint the OLD identity. */
    {
        struct msgwin *mw = msgwin_with_uid (user->uid);
        if (mw) {
            msgwin_apply_user_change (mw, nam, icon, color);
        }
    }
}

void
users_refresh_avatar (guint16 uid)
{
    session *sess = hx_active_session ();

    /* GIF avatar for `uid` changed in the gif_avatar cache — nudge
	 * every list that shows this user so the cell re-reads it. Mirrors
	 * user_change's fan-out: each pchat sidebar, then the standalone
	 * Users window (public chat). We look the hx_user up per chat
	 * because the row<->user mapping is keyed on the struct pointer,
	 * which differs per chat. */
    if (sess->gchats) {
        GHashTableIter iter;
        gpointer key, val;
        g_hash_table_iter_init (&iter, sess->gchats);
        while (g_hash_table_iter_next (&iter, &key, &val)) {
            struct gtkhx_chat *gchat = val;
            if (!gchat || !gchat->userlist || !gchat->chat) {
                continue;
            }
            struct hx_user *u = hx_user_with_uid (gchat->chat, uid);
            if (u) {
                hx_user_list_view_refresh_avatar (gchat->userlist, u);
            }
        }
    }

    if (sess->users_view && gtkhx_prefs.geo.users.open) {
        struct chat *pub = chat_with_cid (sess, 0);
        struct hx_user *u = pub ? hx_user_with_uid (pub, uid) : NULL;
        if (u) {
            hx_user_list_view_refresh_avatar (sess->users_view, u);
        }
    }
}
