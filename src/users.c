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
#include "hxconn.h"
#include "gtkhx_session.h"
#include "gtkhx_theme.h"
#include "hl_access.h"
#include "cicn.h"
#include "hotline_proto.h"
#include "network.h"
#include "proto_helpers.h" /* struct hx_chunk (stack-allocated below) */
#include "chat.h"
#include "chat_members.h"
#include "gtkhx.h"
#include "msg.h"
#include "chat_tabs.h"
#include "gtkutil.h"
#include "tasks.h"
#include "rcv.h"
#include "users.h"
#include "users_view.h"
#include "voice_panel.h" /* voice_panel_new — Join/Mute icon controls */
#ifdef HAVE_VOICE
#include "voice_model.h"   /* hx_voice_model_get_indicator — in-voice check */
#include "voice_runtime.h" /* gtkhx_voice_runtime_set_user_volume — slider */
#endif
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
    gboolean utf8 = (hx_conn_has_cap (htlc, HTLC_CAP_TEXT_ENCODING)) != 0;
    gsize name_len = 0;
    char *name_wire
        = gtkhx_text_for_wire ((const char *)hx_conn_name (htlc), strlen (hx_conn_name (htlc)),
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
    bool has_color = hx_conn_nick_color (htlc) != HX_NICK_COLOR_NONE;
    struct hx_chunk chunks[3];
    guint8 scratch[6];
    int hc = (int)gtkhx_proto_build_user_change_chunks (
        hx_conn_icon (htlc), (const uint8_t *)name_wire, name_len, has_color ? 1 : 0,
        hx_conn_nick_color (htlc), chunks, G_N_ELEMENTS (chunks), scratch,
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


struct UserActionCtx {
    session *sess;
    guint32 cid;
    guint16 uid;
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
    if (!ctx || !ctx->sess) {
        return;
    }
    hx_kick_user (ctx->sess->htlc, ctx->uid, 0);
}

static void
on_user_ban (GSimpleAction *action, GVariant *param, gpointer user_data)
{
    struct UserActionCtx *ctx = user_data;
    (void)action;
    (void)param;
    if (!ctx || !ctx->sess) {
        return;
    }
    hx_kick_user (ctx->sess->htlc, ctx->uid, 1);
}

static void
on_user_ignore (GSimpleAction *action, GVariant *param, gpointer user_data)
{
    struct UserActionCtx *ctx = user_data;
    (void)action;
    (void)param;
    struct hx_member_info mi;
    struct chat *c
        = (ctx && ctx->sess) ? chat_with_cid (ctx->sess, ctx->cid) : NULL;
    if (!c || !hx_member_model_get_info (hx_chat_member_model (c), ctx->uid, &mi)) {
        return;
    }
    hx_member_model_set_ignore (hx_chat_member_model (c), ctx->uid, TRUE);
    hx_printf_prefix (ctx->sess->htlc, 0, INFOPREFIX,
                      _ ("ignore: %s is now ignored\n"), mi.name);
}

static void
on_user_unignore (GSimpleAction *action, GVariant *param, gpointer user_data)
{
    struct UserActionCtx *ctx = user_data;
    (void)action;
    (void)param;
    struct hx_member_info mi;
    struct chat *c
        = (ctx && ctx->sess) ? chat_with_cid (ctx->sess, ctx->cid) : NULL;
    if (!c || !hx_member_model_get_info (hx_chat_member_model (c), ctx->uid, &mi)) {
        return;
    }
    hx_member_model_set_ignore (hx_chat_member_model (c), ctx->uid, FALSE);
    hx_printf_prefix (ctx->sess->htlc, 0, INFOPREFIX,
                      _ ("ignore: %s is now unignored\n"), mi.name);
}

static void
on_user_info (GSimpleAction *action, GVariant *param, gpointer user_data)
{
    struct UserActionCtx *ctx = user_data;
    (void)action;
    (void)param;
    if (!ctx || !ctx->sess) {
        return;
    }
    hx_get_user_info (ctx->sess->htlc, ctx->uid);
}

static void
on_user_msg (GSimpleAction *action, GVariant *param, gpointer user_data)
{
    struct UserActionCtx *ctx = user_data;
    struct msgwin *msg;
    (void)action;
    (void)param;

    struct hx_member_info mi;
    struct chat *c
        = (ctx && ctx->sess) ? chat_with_cid (ctx->sess, ctx->cid) : NULL;
    if (!c || !hx_member_model_get_info (hx_chat_member_model (c), ctx->uid, &mi)) {
        return;
    }

    if ((msg = msgwin_with_uid (mi.uid))) {
        /* Existing msgwin — just raise its tab inside the Chat
         * panel. The Chat panel itself gets attached / raised by
         * gtkhx_chat_tabs_raise_msg if it's hidden. */
        gtkhx_chat_tabs_raise_msg (mi.uid);
    } else {
        create_msgwin (mi.uid, mi.name);
    }
}

static void
on_user_pchat (GSimpleAction *action, GVariant *param, gpointer user_data)
{
    struct UserActionCtx *ctx = user_data;
    int with_cid = 0;
    (void)action;
    (void)param;

    if (!ctx || !ctx->sess) {
        return;
    }

    if (ctx->sess->chats) {
        guint n = hx_chats_count (ctx->sess->chats);
        for (guint i = 0; i < n; i++) {
            struct chat *c = hx_chats_get_at (ctx->sess->chats, i);
            if (hx_chats_cid_at (ctx->sess->chats, i) != 0 && hx_chat_view (c)) {
                with_cid = 1;
                break;
            }
        }
    }

    if (!with_cid) {
        hx_chat_user (ctx->sess->htlc, ctx->uid);
    } else {
        prompt_chat (ctx->sess, ctx->uid);
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
    if (!ctx) {
        return;
    }
    gtkhx_avatar_set_paused (ctx->uid, !gtkhx_avatar_is_paused (ctx->uid));
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

#ifdef HAVE_VOICE
/* GtkRange::value-changed on the per-user volume slider. The scale
 * reads 0..150 (percent); the runtime wants a linear gain, so divide
 * by 100 (100% == unity 1.0, 150% == 1.5× boost). ctx outlives the
 * scale (both are owned by the popover), so borrowing sess + uid
 * through it is safe for the slider's lifetime. */
static void
on_user_volume_changed (GtkRange *range, gpointer user_data)
{
    struct UserActionCtx *ctx = user_data;
    double gain;

    if (!ctx || !ctx->sess || !ctx->sess->voice_runtime) {
        return;
    }
    gain = gtk_range_get_value (range) / 100.0;
    gtkhx_voice_runtime_set_user_volume (ctx->sess->voice_runtime,
                                         ctx->uid, gain);
}

/* Append a "Volume" label + horizontal GtkScale (0..150 %) to the
 * popover's vbox, initialised to the user's stored gain. Only called
 * when the user is actually in the voice room (indicator != NONE), so
 * the control never shows up for someone who can't be heard. */
static void
user_popup_append_volume (GtkBox *vbox, struct UserActionCtx *ctx)
{
    GtkWidget *label, *scale;
    double stored;

    label = gtk_label_new (_ ("Volume"));
    gtk_label_set_xalign (GTK_LABEL (label), 0.0);
    gtk_widget_set_margin_start (label, 8);
    gtk_widget_set_margin_end (label, 8);
    gtk_box_append (vbox, label);

    /* 0..150 %, 5% steps; 100% is unity. Value shown as the drag
     * happens so the user gets numeric feedback without a dialog. */
    scale = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL,
                                      0.0, 150.0, 5.0);
    gtk_scale_set_draw_value (GTK_SCALE (scale), TRUE);
    gtk_scale_set_value_pos (GTK_SCALE (scale), GTK_POS_RIGHT);
    gtk_widget_set_hexpand (scale, TRUE);
    gtk_widget_set_size_request (scale, 160, -1);
    gtk_widget_set_margin_start (scale, 8);
    gtk_widget_set_margin_end (scale, 8);
    /* Mark unity so 100% is easy to find by feel. */
    gtk_scale_add_mark (GTK_SCALE (scale), 100.0, GTK_POS_BOTTOM, NULL);

    /* The runtime clamps stored gain up to 10.0 (1000%), but the scale
     * only represents 0..150%. Clamp the initial position to the
     * scale's range so the widget reflects a value it can actually
     * show — don't lean on GTK's internal clamping, which would leave
     * the thumb at 150% while playback stayed higher. In practice the
     * slider can only ever store 0..150% itself; this guards a value
     * set by some future caller / bad FFI. */
    stored = gtkhx_voice_runtime_user_volume (ctx->sess->voice_runtime,
                                              ctx->uid) * 100.0;
    stored = CLAMP (stored, 0.0, 150.0);
    gtk_range_set_value (GTK_RANGE (scale), stored);

    g_signal_connect (scale, "value-changed",
                      G_CALLBACK (on_user_volume_changed), ctx);
    gtk_box_append (vbox, scale);
}
#endif /* HAVE_VOICE */

/* The user_popup_show entry point is what HxUserListView's
 * right-click gesture and any future in-file callers use; the
 * old static `user_popup' wrapper retired with the legacy
 * user_pressed handler (Phase C). */
void
user_popup_show (GtkWidget *anchor, session *sess, guint32 cid, guint16 uid,
                 double x, double y)
{
    GtkWidget *popover, *vbox, *info_label, *sep, *parent;
    struct UserActionCtx *ctx;
    struct hx_member_info mi;
    char *info_markup;
    GdkRectangle rect;
    graphene_point_t src_pt = { (float)x, (float)y };
    graphene_point_t dst_pt = { (float)x, (float)y };

    if (!sess) {
        return;
    }
    /* Resolve the member for the header + capability checks. The handlers
     * re-resolve from (cid, uid) at click time, so a member that leaves
     * while the menu is open just makes them no-op. */
    {
        struct chat *c = chat_with_cid (sess, cid);
        if (!c || !hx_member_model_get_info (hx_chat_member_model (c), uid, &mi)) {
            return;
        }
    }

    user_popup_install_css ();

    ctx = g_new0 (struct UserActionCtx, 1);
    ctx->sess = sess;
    ctx->cid = cid;
    ctx->uid = uid;

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
        "<b>%s</b>\n<small>UID %d · Icon %d · %s%s</small>", mi.name,
        mi.uid, mi.icon, mi.status >= 2 ? _ ("Admin") : _ ("Guest"),
        mi.status % 2 ? _ (" (Away)") : "");
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
    if (hl_access_has ((const guint8 *)&sess->htlc->access,
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
    if (gtkhx_avatar_is_animated (uid)) {
        gtk_box_append (GTK_BOX (vbox),
                        gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
        user_popup_append_button (
            GTK_BOX (vbox), GTK_POPOVER (popover), ctx,
            gtkhx_avatar_is_paused (uid) ? _ ("Resume Animation")
                                               : _ ("Pause Animation"),
            on_user_toggle_anim);
    }

#ifdef HAVE_VOICE
    /* Voice: per-listener volume slider, only when this user is
     * actually in the voice room (indicator != NONE means present).
     * A dialog-free inline GtkScale — drag to set how loud you hear
     * this person; session-scoped, applied live. */
    if (sess->voice_model
        && hx_voice_model_get_indicator (sess->voice_model, uid)
               != HX_VOICE_INDICATOR_NONE
        && sess->voice_runtime) {
        gtk_box_append (GTK_BOX (vbox),
                        gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
        user_popup_append_volume (GTK_BOX (vbox), ctx);
    }
#endif /* HAVE_VOICE */

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
 * ctx->sess->htlc reading freed memory. */
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
        hx_invite_user (ctx->sess->htlc, ctx->uid, chat_cid);
    } else if (g_strcmp0 (response, "new") == 0) {
        hx_chat_user (ctx->sess->htlc, ctx->uid);
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

    if (sess->chats) {
        guint n = hx_chats_count (sess->chats);
        for (guint i = 0; i < n; i++) {
            struct chat *c = hx_chats_get_at (sess->chats, i);
            if (!hx_chat_cid (c) || !hx_chat_view (c)) {
                continue;   /* skip the public chat + window-less models */
            }
            GtkWidget *row = prompt_chat_make_row (hx_chat_cid (c), hx_chat_subject (c));
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
    if (!pub) {
        return;
    }
    /* Repopulate the standalone Users view straight from the public chat's
     * membership model — the model is the store now, and
     * user_create(cid=0) only ever added to this same view anyway. */
    guint n = hx_member_model_count (hx_chat_member_model (pub));
    for (guint i = 0; i < n; i++) {
        struct hx_member_info mi;
        if (!hx_member_model_get_at (hx_chat_member_model (pub), i, &mi)) {
            continue;
        }
        hx_user_list_view_add (sess->users_view, mi.uid, mi.name, mi.icon,
                               mi.status, mi.nick_color);
    }
}

/* ----------------------------------------------------------------
 * Headerbar / sidebar button handlers, shared between the standalone
 * Users window and the pchat sidebars in chat.c.
 *
 * The `data' parameter is the HxUserListView* the button is attached
 * to. view_selected_member reads the current single-selection; the
 * session pointer is borrowed from the view (set at construction). */
/* Fill *out with the acted-on member from the view's live selection,
 * resolved fresh from (cid, uid) against the view's member model.
 * Returns FALSE when nothing is selected or the member has since left. The
 * fresh resolution also avoids the old use-after-free (a borrowed hx_user*
 * that dangled if the user left between selection and the button click). */
static gboolean
view_selected_member (gpointer data, struct hx_member_info *out)
{
    HxUserListView *v = data;
    session *sess;
    guint16 uid;
    struct chat *c;
    if (!v) {
        return FALSE;
    }
    sess = hx_user_list_view_get_session (v);
    uid = hx_user_list_view_get_selected_uid (v);
    if (!sess || !uid) {
        return FALSE;
    }
    c = chat_with_cid (sess, hx_user_list_view_get_cid (v));
    return c && hx_member_model_get_info (hx_chat_member_model (c), uid, out);
}

static session *
view_session (gpointer data)
{
    HxUserListView *v = data;
    return v ? hx_user_list_view_get_session (v) : NULL;
}

/* The chat id the view lists — pairs with a selected uid to reach the
 * per-chat member_model (e.g. the ignore toggle). */
static guint32
view_cid (gpointer data)
{
    HxUserListView *v = data;
    return v ? hx_user_list_view_get_cid (v) : 0;
}

void
view_msg_btn (GtkWidget *w, gpointer data)
{
    struct hx_member_info mi;
    struct msgwin *mw;
    (void)w;
    if (!view_selected_member (data, &mi)) {
        return;
    }
    mw = msgwin_with_uid (mi.uid);
    if (mw) {
        gtkhx_chat_tabs_raise_msg (mi.uid);
    } else {
        create_msgwin (mi.uid, mi.name);
    }
}

void
view_info_btn (GtkWidget *w, gpointer data)
{
    struct hx_member_info mi;
    session *sess = view_session (data);
    (void)w;
    if (!sess || !view_selected_member (data, &mi)) {
        return;
    }
    hx_get_user_info (sess->htlc, mi.uid);
}

void
view_kick_btn (GtkWidget *w, gpointer data)
{
    struct hx_member_info mi;
    session *sess = view_session (data);
    (void)w;
    if (!sess || !view_selected_member (data, &mi)) {
        return;
    }
    hx_kick_user (sess->htlc, mi.uid, 0);
}

void
view_ban_btn (GtkWidget *w, gpointer data)
{
    struct hx_member_info mi;
    session *sess = view_session (data);
    (void)w;
    if (!sess || !view_selected_member (data, &mi)) {
        return;
    }
    hx_kick_user (sess->htlc, mi.uid, 1);
}

void
view_igno_btn (GtkWidget *w, gpointer data)
{
    struct hx_member_info mi;
    session *sess = view_session (data);
    (void)w;
    if (!sess || !view_selected_member (data, &mi)) {
        return;
    }
    struct chat *c = chat_with_cid (sess, view_cid (data));
    if (!c) {
        return;
    }
    gboolean ig = hx_member_model_toggle_ignore (hx_chat_member_model (c), mi.uid);
    hx_printf_prefix (sess->htlc, 0, INFOPREFIX,
                      ig ? _ ("ignore: %s is now ignored\n")
                         : _ ("ignore: %s is now unignored"),
                      mi.name);
}

void
view_chat_btn (GtkWidget *w, gpointer data)
{
    struct hx_member_info mi;
    session *sess = view_session (data);
    int with_cid = 0;
    (void)w;

    if (!sess || !view_selected_member (data, &mi)) {
        return;
    }
    if (sess->chats) {
        guint n = hx_chats_count (sess->chats);
        for (guint i = 0; i < n; i++) {
            struct chat *c = hx_chats_get_at (sess->chats, i);
            if (hx_chats_cid_at (sess->chats, i) != 0 && hx_chat_view (c)) {
                with_cid = 1;
                break;
            }
        }
    }
    if (!with_cid) {
        hx_chat_user (sess->htlc, mi.uid);
    } else {
        prompt_chat (sess, mi.uid);
    }
}

void
users_clear (struct htlc_conn *htlc, struct chat *chat)
{
    session *sess = sess_from_htlc (htlc);
    (void)htlc;

    /* Drop this chat's authoritative membership and subject: a
     * disconnect / reconnect resets the user list, and the public chat
     * (which persists across reconnect) must not carry a stale topic.
     * Before the view gate so it runs even when the window is closed. */
    if (chat && hx_chat_member_model (chat)) {
        hx_member_model_clear (hx_chat_member_model (chat));
        hx_chat_set_subject (chat, NULL, 0);
    }

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


/* Pointer-free core: compute a foreground from a raw nick_color +
 * status, so the view row can cache its nick_color and recompute fg without
 * a borrowed per-user pointer. HX_NICK_COLOR_NONE falls through to the status
 * palette. */
GdkRGBA *
user_nick_color_rgb (guint32 nick_color, guint16 status, GdkRGBA *out)
{
    if (nick_color != HX_NICK_COLOR_NONE && out) {
        double r = ((nick_color >> 16) & 0xff) / 255.0;
        double g = ((nick_color >> 8) & 0xff) / 255.0;
        double b = (nick_color & 0xff) / 255.0;
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
user_create (struct htlc_conn *htlc, struct chat *chat, guint16 uid,
             guint32 nick_color, const char *nam, guint16 icon, guint16 color)
{
    session *sess = sess_from_htlc (htlc);
    struct gtkhx_chat *gchat;

    /* Feed this chat's authoritative membership model before
     * any view gate, so it stays populated even when the chat has no user-list
     * view open. Read by tab_nick_comp for input in this chat. */
    if (hx_chat_member_model (chat)) {
        hx_member_model_upsert (hx_chat_member_model (chat), uid, nam, icon, color,
                                nick_color);
    }

    if (hx_chat_cid (chat)) {
        /* Per-pchat sidebar — HxUserListView, STYLE_CHAT. The
		 * pchat window's userlist GObject is created lazily by
		 * create_pchat_window (Phase C) the first time a user
		 * shows up in this chat. */
        gchat = gchat_with_cid (sess, hx_chat_cid (chat));
        if (!gchat) {
            gchat = create_pchat_window (htlc, chat);
        }
        if (!gchat || !hx_gchat_userlist (gchat)) {
            return;
        }
        hx_user_list_view_add (hx_gchat_userlist (gchat), uid, nam, icon,
                               color, nick_color);
        return;
    }

    /* hx_chat_cid (chat) == 0 — standalone Users window. The view's own
	 * bookkeeping (GListStore + GtkSortListModel) does the insert +
	 * position; the row computes its own foreground via
	 * user_nick_color_gdk from the `color` arg. */
    (void)htlc;
    if (!sess->users_view || !gtkhx_prefs.geo.users.open) {
        return;
    }
    hx_user_list_view_add (sess->users_view, uid, nam, icon, color,
                           nick_color);
}

void
user_delete (struct htlc_conn *htlc, struct chat *chat, guint16 uid)
{
    struct gtkhx_chat *gchat;
    session *sess = sess_from_htlc (htlc);

    (void)htlc;
    if (hx_chat_member_model (chat)) {
        hx_member_model_remove (hx_chat_member_model (chat), uid);
    }
    if (hx_chat_cid (chat)) {
        gchat = gchat_with_cid (sess, hx_chat_cid (chat));
        if (!gchat || !hx_gchat_userlist (gchat)) {
            return;
        }
        hx_user_list_view_remove (hx_gchat_userlist (gchat), uid);
        return;
    }

    /* hx_chat_cid (chat) == 0 — standalone Users window. */
    if (!sess->users_view || !gtkhx_prefs.geo.users.open) {
        return;
    }
    hx_user_list_view_remove (sess->users_view, uid);
}

void
user_change (struct htlc_conn *htlc, struct chat *chat, guint16 uid,
             guint32 nick_color, const char *nam, guint16 icon, guint16 color)
{
    struct gtkhx_chat *gchat;
    session *sess = sess_from_htlc (htlc);

    (void)htlc;

    if (hx_chat_member_model (chat)) {
        hx_member_model_upsert (hx_chat_member_model (chat), uid, nam, icon, color,
                                nick_color);
    }

    if (hx_chat_cid (chat)) {
        gchat = gchat_with_cid (sess, hx_chat_cid (chat));
        if (!gchat) {
            gchat = create_pchat_window (sess->htlc, chat);
        }
        if (!gchat || !hx_gchat_userlist (gchat)) {
            return;
        }
        /* In-place state mutation: HxUserRow::set_state fires its
		 * "changed" signal, the sort model re-orders, the cell
		 * re-snapshots. The row keeps its GObject identity so the
		 * sidebar selection stays on the same user across rename or
		 * icon change. */
        hx_user_list_view_update (hx_gchat_userlist (gchat), uid, nam, icon,
                                  color, nick_color);
        return;
    }

    /* hx_chat_cid (chat) == 0 — fan out the change into per-pchat sidebars
	 * AND into the standalone Users window. The fan-out happens
	 * BEFORE the Users-window update so the recursion shape is the
	 * same as the legacy code; both arms compute the foreground
	 * from the freshly-parsed `color`. */
    if (sess->chats) {
        guint n = hx_chats_count (sess->chats);
        for (guint i = 0; i < n; i++) {
            if (hx_chats_cid_at (sess->chats, i) == 0) {
                continue;   /* public chat handled via the standalone view */
            }
            struct chat *c = hx_chats_get_at (sess->chats, i);
            if (!hx_chat_view (c)) {
                continue;   /* only open private-chat windows */
            }
            /* Only fan the change into pchats this user is actually in
			 * (the membership check is the model now). uid +
			 * nick_color are the same across chats. */
            if (hx_member_model_contains (hx_chat_member_model (c), uid)) {
                user_change (sess->htlc, c, uid, nick_color, nam, icon, color);
            }
        }
    }

    if (sess->users_view && gtkhx_prefs.geo.users.open) {
        /* HxUserListView's update is an in-place state mutation
		 * (HxUserRow::set_state fires "changed", the sort model
		 * re-orders, the column-view cell re-snapshots). The row
		 * keeps its GObject identity, so the live selection stays
		 * on the same user across rename or icon change. */
        hx_user_list_view_update (sess->users_view, uid, nam, icon, color,
                              nick_color);
    }

    /* if this user has an open PM window, refresh its info
	 * pane (icon / name / status) so it tracks the user changing
	 * their nick or going idle. We pass the new nam/icon/color
	 * through directly rather than re-reading cached state — rcv.c
	 * hasn't patched the new values onto the model yet at
	 * this point in the dispatch (its rename-detection compares
	 * the old name vs nam after we return), so a cache-lookup
	 * refresh would paint the OLD identity. */
    {
        struct msgwin *mw = msgwin_with_uid (uid);
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
	 * Users window (public chat). Each view's row map is keyed on the
	 * uid, so we refresh per chat the user appears in. */
    if (sess->chats) {
        guint n = hx_chats_count (sess->chats);
        for (guint i = 0; i < n; i++) {
            struct chat *c = hx_chats_get_at (sess->chats, i);
            struct gtkhx_chat *gchat = hx_chat_view (c);
            if (!gchat || !hx_gchat_userlist (gchat)) {
                continue;
            }
            hx_user_list_view_refresh_avatar (hx_gchat_userlist (gchat), uid);
        }
    }

    if (sess->users_view && gtkhx_prefs.geo.users.open) {
        hx_user_list_view_refresh_avatar (sess->users_view, uid);
    }
}
