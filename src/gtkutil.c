/*
 * Copyright (C) 2001 Misha Nasledov <misha@nasledov.com>
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
#include <fcntl.h>
#include <gtk/gtk.h>
#include <adwaita.h>
#include <gdk/gdkkeysyms.h>
#include <time.h>
#include "hx.h"
#include "gtkhx_icon.h"
#include "news.h"
#include "network.h"
#include "toolbar.h"
#include "tracker.h"
#include "tasks.h"
#include "users.h"
#include "chat.h"
#include "chat_tabs.h"
#include "connect.h"
#include "gtkhx.h"
#include "files.h"
#include "gtkutil.h"
#include "hl_access.h"
#include "hxconn.h"
#include "host_port.h"
#include "tray.h"
#include "banner.h"
#ifdef HAVE_VOICE
#include "voice_panel.h"
#endif
#include "inline_media_attach.h"
#include "panel_registry.h"

/* GtkAccelGroup / gtk_accel_group_new /
 * gtk_widget_add_accelerator / gtk_window_add_accel_group are gone
 * in GTK 4 — replaced by GtkShortcutController plus GtkShortcut
 * instances bound to GtkKeyvalTrigger triggers and GtkCallbackAction
 * actions.
 *
 * Behavior: every window the user opens gets Ctrl+K (connect dialog)
 * and Ctrl+Q (quit) wired up. Connect still goes through the
 * AdwHeaderBar's connect_btn (so visual feedback works). Quit fires
 * the app.quit GAction directly, since after the Phase 5 toolbar
 * refactor Quit lives in the hamburger menu and there is no quit_btn
 * to "click". The GAction approach is the modern path for the rest
 * of these too — see gtk_application_set_accels_for_action — but
 * Connect's button-driven flow is harmless for now. */

static gboolean
keyaccel_connect_cb (GtkWidget *w, GVariant *args, gpointer data)
{
    (void)w;
    (void)args;
    (void)data;
    if (connect_btn) {
        g_signal_emit_by_name (connect_btn, "clicked");
    }
    return TRUE;
}

static gboolean
keyaccel_quit_cb (GtkWidget *w, GVariant *args, gpointer data)
{
    GApplication *app = g_application_get_default ();
    (void)w;
    (void)args;
    (void)data;
    if (app) {
        g_action_group_activate_action (G_ACTION_GROUP (app), "quit", NULL);
    }
    return TRUE;
}

/* Ctrl+W — close the focused window. Skips the toolbar
 * (the toolbar is the application's anchor; closing it via the WM's
 * X button does drive hx_quit, but we don't want a casual Ctrl+W to
 * tear down the whole app). For every other window, route through
 * gtk_window_close so the existing close-request handler still
 * runs — keeps preference / position saving in lockstep with the
 * normal close path. The widget arg is the controller's widget
 * (the window we attached to), so casting to GtkWindow is safe. */
static gboolean
keyaccel_close_cb (GtkWidget *w, GVariant *args, gpointer data)
{
    (void)args;
    (void)data;
    if (GTK_IS_WINDOW (w)) {
        gtk_window_close (GTK_WINDOW (w));
    }
    return TRUE;
}

/* Ctrl+T — open (or focus) the Tracker window. Same
 * everywhere init_keyaccel runs, including the toolbar, so the
 * shortcut works even with no other window in focus. tracker.c's
 * create_tracker_window is idempotent: if the tracker is already
 * up, the early-return at the top of the function leaves the
 * existing window alone. */
static gboolean
keyaccel_tracker_cb (GtkWidget *w, GVariant *args, gpointer data)
{
    (void)w;
    (void)args;
    (void)data;
    create_tracker_window (NULL, hx_active_session ());
    return TRUE;
}

/* AdwDialog variant of keyaccel_close_cb — adw_dialog_close runs the
 * close-attempt machinery (fires close_response + close-attempt
 * signal) instead of destroying the widget. Used for Ctrl+W on
 * AdwDialog, which otherwise wouldn't have a usable close shortcut.
 * Esc is handled natively by AdwDialog via close_response, so no
 * separate Esc binding is needed. */
static gboolean
keyaccel_close_adw_dialog_cb (GtkWidget *w, GVariant *args, gpointer data)
{
    (void)args;
    (void)data;
    if (ADW_IS_DIALOG (w)) {
        adw_dialog_close (ADW_DIALOG (w));
    }
    return TRUE;
}

void
init_keyaccel (GtkWidget *widget)
{
    init_keyaccel_full (widget, FALSE);
}

void
init_keyaccel_dialog (GtkWidget *widget)
{
    init_keyaccel_full (widget, TRUE);
}

void
init_keyaccel_full (GtkWidget *widget, gboolean esc_closes)
{
    GtkEventController *ctrl = gtk_shortcut_controller_new ();
    GtkShortcut *sc;

    gtk_event_controller_set_propagation_phase (ctrl, GTK_PHASE_CAPTURE);

    sc = gtk_shortcut_new (
        gtk_keyval_trigger_new ('k', GDK_CONTROL_MASK),
        gtk_callback_action_new (keyaccel_connect_cb, NULL, NULL));
    gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (ctrl), sc);

    sc = gtk_shortcut_new (
        gtk_keyval_trigger_new ('q', GDK_CONTROL_MASK),
        gtk_callback_action_new (keyaccel_quit_cb, NULL, NULL));
    gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (ctrl), sc);

    /* Ctrl+T → open the Tracker window (or focus it if already up). */
    sc = gtk_shortcut_new (
        gtk_keyval_trigger_new ('t', GDK_CONTROL_MASK),
        gtk_callback_action_new (keyaccel_tracker_cb, NULL, NULL));
    gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (ctrl), sc);

    /* Ctrl+W → close, except on the toolbar (see keyaccel_close_cb).
     * Compared by pointer equality: at this call site, toolbar.c
     * passes the same toolbar_window global that we read here. */
    if (widget != toolbar_window) {
        sc = gtk_shortcut_new (
            gtk_keyval_trigger_new ('w', GDK_CONTROL_MASK),
            gtk_callback_action_new (keyaccel_close_cb, NULL, NULL));
        gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (ctrl),
                                              sc);

        /* Esc → close for dialog-like windows (Settings, Bookmarks,
         * User Editor, About, Agreement, etc.). Off by default so that
         * the main user-facing windows (chat, news, files, tracker)
         * don't swallow Esc — those windows give Esc to widgets like
         * search entries and inline editors. AdwDialog wires Esc
         * itself, so this path is only for GtkWindows that we *want*
         * to behave dialog-y: gtk_window_close runs through any
         * close-request handler the window registered, just like the
         * Ctrl+W path. */
        if (esc_closes) {
            sc = gtk_shortcut_new (
                gtk_keyval_trigger_new (GDK_KEY_Escape, 0),
                gtk_callback_action_new (keyaccel_close_cb, NULL, NULL));
            gtk_shortcut_controller_add_shortcut (
                GTK_SHORTCUT_CONTROLLER (ctrl), sc);
        }
    }

    gtk_widget_add_controller (widget, ctrl);
}

/* Companion to init_keyaccel for AdwDialog instances. AdwDialog
 * isn't a GtkWindow, so init_keyaccel's gtk_window_close path can't
 * be reused; instead we attach Ctrl+W → adw_dialog_close and Ctrl+Q
 * → app.quit directly to the dialog widget. Esc is handled by
 * AdwDialog natively via close_response. Call this right after
 * adw_dialog_new() / adw_alert_dialog_new() / adw_preferences_dialog_new(),
 * before adw_dialog_present(). */
void
gtkhx_dialog_add_close_shortcuts (GtkWidget *dialog)
{
    GtkEventController *ctrl;
    GtkShortcut *sc;

    if (!dialog) {
        return;
    }
    ctrl = gtk_shortcut_controller_new ();
    gtk_event_controller_set_propagation_phase (ctrl, GTK_PHASE_CAPTURE);

    sc = gtk_shortcut_new (
        gtk_keyval_trigger_new ('w', GDK_CONTROL_MASK),
        gtk_callback_action_new (keyaccel_close_adw_dialog_cb, NULL, NULL));
    gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (ctrl), sc);

    sc = gtk_shortcut_new (
        gtk_keyval_trigger_new ('q', GDK_CONTROL_MASK),
        gtk_callback_action_new (keyaccel_quit_cb, NULL, NULL));
    gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (ctrl), sc);

    gtk_widget_add_controller (dialog, ctrl);
}

void
set_disconnect_btn (session *sess, int stat)
{
    gtk_widget_set_sensitive (disconnect_btn, stat);
}

/* helper to flip a hamburger-menu GAction's enabled state.
 * The Admin submenu's New User / Edit User entries used to be
 * standalone toolbar buttons whose sensitivity was driven by the
 * connection state; with the actions on the application instead,
 * we toggle GSimpleAction::enabled and the menu items grey out
 * automatically. */
static void
set_app_action_enabled (const char *name, gboolean enabled)
{
    GApplication *app = g_application_get_default ();
    GAction *action;

    if (!app) {
        return;
    }
    action = g_action_map_lookup_action (G_ACTION_MAP (app), name);
    if (G_IS_SIMPLE_ACTION (action)) {
        g_simple_action_set_enabled (G_SIMPLE_ACTION (action), enabled);
    }
}

void
setbtns (session *sess, int stat)
{
    /* This session's own buttons, and the test is whether *it* has a Users
     * page — not the process-wide "was a Users panel ever built?" latch that
     * used to gate this. The latch answered yes for every connection as soon
     * as any one of them had a page, which was the same thing until there
     * were two of them. NULL means this connection has no Users content yet;
     * its buttons get their state when it does. */
    if (sess->user_btns.msg) {
        gtk_widget_set_sensitive (sess->user_btns.msg, stat);
        gtk_widget_set_sensitive (sess->user_btns.info, stat);
        gtk_widget_set_sensitive (sess->user_btns.chat, stat);
        gtk_widget_set_sensitive (sess->user_btns.ignore, stat);
        /* kick / ban get visibility gating in the Users
         * window — hide them entirely when the account doesn't
         * have HL_ACCESS_DISCONNECT_USERS. (One bit gates both per
         * mhxd's struct.) On disconnect, hide them too: we don't
         * know what the next server will allow, and an unauthorised
         * kick button next to a friendly Msg button is worse UX
         * than just dropping the icon.
         *
         * Same access-bit gate as the right-click popup's Kick/Ban
         * section, so the toolbar and the popup agree on what's
         * available. */
        if (stat
            && hx_conn_access_has (sess->htlc, HL_ACCESS_DISCONNECT_USERS)) {
            gtk_widget_set_visible (sess->user_btns.kick, TRUE);
            gtk_widget_set_sensitive (sess->user_btns.kick, TRUE);
            gtk_widget_set_visible (sess->user_btns.ban, TRUE);
            gtk_widget_set_sensitive (sess->user_btns.ban, TRUE);
        } else {
            gtk_widget_set_visible (sess->user_btns.kick, FALSE);
            gtk_widget_set_visible (sess->user_btns.ban, FALSE);
        }
    }
    /* Same shape as the Users buttons above: this session's own widgets, and
     * the test is whether it has News content rather than whether anyone
     * does. */
    if (sess->postButton) {
        gtk_widget_set_sensitive (sess->postButton, stat);
        gtk_widget_set_sensitive (sess->reloadButton, stat);
    }

    /* files_btn intentionally not gated on connection state — the
     * Files panel is always resident in the dock and the button
     * just brings it forward; even offline it can show whatever
     * directory listing was last fetched. See toolbar.c initial-
     * sensitivity comment. */

    /* Broadcast button: always present in the toolbar, greyed out
     * when unavailable. Unavailable means either the connection is
     * down (stat==0) or the account lacks HL_ACCESS_CAN_BROADCAST.
     * We used to hide-when-not-permitted to match Kick/Ban; Misha
     * preferred always-visible with sensitivity reflecting the
     * actual permission, which is also more discoverable for users
     * who don't realise the feature exists. */
    if (broadcast_btn) {
        gboolean can_broadcast
            = stat && hx_conn_access_has (sess->htlc, HL_ACCESS_CAN_BROADCAST);
        gtk_widget_set_sensitive (broadcast_btn, can_broadcast);
    }

    /* New User / Edit User moved from toolbar buttons to
     * the hamburger menu's Admin submenu. Flip the corresponding
     * GActions instead of the old GtkWidget pointers. Each item is
     * gated independently on its access bit so a sysop with view-
     * only privileges can still open the editor on an existing
     * account but can't fire off the New User dialog (or vice
     * versa for create-only).
     *
     *   user_new  → HL_ACCESS_CREATE_USERS
     *   user_edit → HL_ACCESS_READ_USERS (lets you open the dialog;
     *                MODIFY_USERS still gates Save server-side, but
     *                even view-only privilege should let you inspect)
     *
     * On disconnect (`stat == 0`) both are always disabled —
     * there's no session to talk to. */
    if (stat) {
        set_app_action_enabled (
            "user_new",
            hx_conn_access_has (sess->htlc, HL_ACCESS_CREATE_USERS));
        set_app_action_enabled (
            "user_edit", hx_conn_access_has (sess->htlc, HL_ACCESS_READ_USERS));
    } else {
        set_app_action_enabled ("user_new", FALSE);
        set_app_action_enabled ("user_edit", FALSE);
    }

#ifdef HAVE_VOICE
    /* Phase 8.D: refresh the per-chat voice toolbars. The chat
     * windows open before LOGIN finishes, so their
     * construction-time refresh runs against caps=0 / access=0
     * and the toolbars stay hidden. setbtns() runs after
     * SELFINFO populates the access bitmap (see
     * hx_rcv_user_selfinfo) — by then HTLC_CAP_VOICE has also
     * landed (HTLS_DATA_CAPABILITIES handler in rcv.c), so this
     * is the right place to flip the toolbars on. On disconnect
     * (stat==0), the refresh hides them again — htlc->caps will
     * have been cleared by network.c. */
    voice_panel_refresh_all_chats (sess);
#endif /* HAVE_VOICE */

    /* Same gating discipline for the Phase 9.C inline-media
     * attach buttons: visible only when the server echoed
     * HTLC_CAP_INLINE_MEDIA. Most Hotline servers don't ship
     * the extension; hiding the button on those sessions is
     * less confusing than a button that always shows an
     * inert toast. Cleared on disconnect via the same caps
     * reset path. */
    inline_media_attach_refresh_all_chats (sess);

    /* News-related toolbar buttons get sensitivity-only
     * gating — they always remain visible so the toolbar shape
     * doesn't reshape between connections.
     *
     *   news_btn   (legacy News): enabled when the account has
     *               HL_ACCESS_READ_NEWS. The legacy news file
     *               protocol exists on every Hotline server
     *               version including 1.5+ — Badmoon (1.9) and
     *               other modern servers serve both legacy and
     *               threaded news side by side, so don't gate this
     *               on server version. The legacy "Post" button
     *               that used to live on the toolbar was removed
     *               in 2026-05; the News window's own headerbar
     *               already exposes a Post action gated on
     *               HL_ACCESS_POST_NEWS, so the toolbar copy was
     *               redundant.
     *
     *   news15_btn (threaded News): NOT gated here. Like Files,
     *               News 1.5 is a permanent panel in the dock now;
     *               clicking the button just brings it forward.
     *               The panel itself can show its cached state
     *               when disconnected, or a 'this server is older
     *               than 1.5' message when connected to an old
     *               server. */
    if (!stat) {
        gtk_widget_set_sensitive (news_btn, FALSE);
    } else {
        /* access_permits (not access_has): a legacy / minimal server
         * that sends no access bitmap (all zeros — e.g. the 1.0/1.2-class
         * ones) still serves legacy news, and news.c fires NEWS_GETFILE in
         * that case. Gate the button by the same rule so it isn't greyed
         * out on a server where News actually works. */
        gtk_widget_set_sensitive (
            news_btn, hx_conn_access_permits (sess->htlc, HL_ACCESS_READ_NEWS));
    }
}

/* status_bar is now a GtkLabel (was GtkStatusbar — deprecated
 * in GTK 4.10). The toolbar always replaced the message wholesale, so
 * the message-stack model the GtkStatusbar provided was overhead that
 * earned us nothing. A single gtk_label_set_text per state change
 * does what we want.
 *
 * The label shows the persistent state ("Logged in to ...") for
 * ambient awareness. Important state transitions also fire an
 * AdwToast over the toolbar so the change is visible without the
 * user having to glance at the corner of the window: login success
 * is the canonical positive transition, disconnect-from-connected is
 * the canonical "you've lost connectivity" negative transition. The
 * intermediate "Connecting..." / TCP-connected states are
 * label-only because they're either expected (you just clicked
 * Connect) or short-lived (TCP-connected almost always becomes
 * Logged-in within milliseconds). */
char *
hx_session_label (const session *sess)
{
    const char *host;

    if (sess == NULL) {
        return g_strdup ("");
    }
    /* What the server calls itself, if it has said — that is the name the user
     * recognises. Otherwise the endpoint they typed. */
    if (sess->server_name && *sess->server_name) {
        return g_strdup (sess->server_name);
    }
    host = hx_conn_serverhost (sess->htlc);
    if (host == NULL || *host == 0) {
        return g_strdup ("");
    }
    return gtkhx_join_host_port (host, hx_conn_serverport (sess->htlc));
}

/* Shared body. `announce` is what separates a state *change* from a repaint:
 * a change is worth a toast and a banner, and re-showing the same state
 * because the user switched tabs is not. Without the split, switching back to
 * a logged-in connection re-toasted "Logged in to …" every time. */
static void status_bar_set (session *sess, int status, gboolean announce);

static void
status_bar_set (session *sess, int status, gboolean announce)
{
    const char *fixed = NULL;
    char *fmt = NULL;
    char *toast = NULL;
    char *addr;

    if (!status_bar) {
        return;
    }

    /* One status bar, one title, one tray icon — so this chrome *follows the
     * focus* rather than becoming per-connection: it shows the state of the
     * connection the user is looking at, and a background connection changing
     * state must not repaint it.
     *
     * It used to read a `server_addr` global that the most recent connect
     * overwrote, which is the same thing at one connection and a race between
     * them at two. The address comes off the connection now. */
    if (sess == NULL || sess != hx_active_session ()) {
        return;
    }
    addr = hx_session_label (sess);

    switch (status) {
    case -1:
        fmt = g_strdup_printf ("%s %s", _ ("Connecting to"), addr);
        /* Hide any leftover "lost connection" banner — the user
         * is actively trying to reconnect. */
        toolbar_hide_banner ();
        break;
    case 0:
        fixed = _ ("Not Connected");
        /* Toast + banner only on a real disconnect — first-launch
         * state change of 0 -> 0 shouldn't surface a notification,
         * and neither should a Connect-canceled (last_status == -1). */
        if (announce && (sess->last_status == 1 || sess->last_status == 2)) {
            toast = g_strdup_printf ("%s %s", _ ("Disconnected from"), addr);
            toolbar_show_connection_lost (addr);
        }
        break;
    case 1:
        fmt = g_strdup_printf ("%s %s", _ ("Connected to"), addr);
        toolbar_hide_banner ();
        break;
    case 2:
        fmt = g_strdup_printf ("%s %s", _ ("Logged in to"), addr);
        if (announce) {
            toast = g_strdup (fmt);
        }
        toolbar_hide_banner ();
        break;
    default:
        g_free (addr);
        return;
    }

    gtk_label_set_text (GTK_LABEL (status_bar), fmt ? fmt : fixed);
    if (toast) {
        toolbar_show_toast (toast);
    }
    g_free (addr);
    g_free (fmt);
    g_free (toast);
    if (announce) {
        sess->last_status = status;
    }
}

void
set_status_bar (session *sess, int status)
{
    status_bar_set (sess, status, TRUE);
}

void
hx_chrome_refresh (void)
{
    session *sess = hx_active_session ();

    if (sess == NULL) {
        return;
    }
    /* Derived from the connection rather than remembered, so this is correct
     * however the user got here — including a tab switch onto a connection
     * whose state last changed long ago.
     *
     * Repaint, never announce: nothing has *happened*, the user has just
     * looked somewhere else. Announcing would re-toast "Logged in to …" on
     * every switch back to a connected tab. */
    if (hx_conn_logged_in (sess->htlc)) {
        status_bar_set (sess, 2, FALSE);
        changetitlesconnected (sess);
    } else if (hx_conn_fd (sess->htlc)) {
        status_bar_set (sess, 1, FALSE);
    } else {
        status_bar_set (sess, 0, FALSE);
        changetitlesdisconnected (sess);
    }
    banner_show_active ();
    /* Whatever the signal handler last decided for *this* connection, which
     * is what the tray is showing for the one in front. Reading a connection
     * flag instead gets it wrong in both directions: logged-in leaves a tab
     * that is still handshaking looking disconnected, and having-a-socket
     * lights the tray from the moment a connect is spawned, well before the
     * handler would. The recorded status is the only thing that matches. */
    gtkhx_tray_set_connected (sess->last_status >= 1);
    set_disconnect_btn (sess, hx_conn_fd (sess->htlc) ? 1 : 0);
    setbtns (sess, hx_conn_logged_in (sess->htlc) ? 1 : 0);
}

void
changetitlesconnected (session *sess)
{
    char *tooltitle;
    char *addr;

    /* One window, so its title follows the focus like the rest of the chrome:
     * a background connection logging in must not retitle the window to a
     * server the user isn't looking at. */
    if (sess != hx_active_session () || !GTK_IS_WINDOW (sess->toolbar_window)) {
        return;
    }
    addr = hx_session_label (sess);
    tooltitle = g_strdup_printf ("%s (%s)", _ ("GtkHx"), addr);
    gtk_window_set_title (GTK_WINDOW (sess->toolbar_window), tooltitle);
    g_free (addr);
    g_free (tooltitle);

    /* the per-window title-setting
     * loop for News / Chat / Users / Tasks is gone — those panels
     * live inside the toolbar window now, so their "title" is the
     * tab label set by the panel factory. Per-server attribution
     * is carried by the toolbar window's title above. */
}

void
changetitlespecific (GtkWidget *widget, char *name)
{
    char *futuretitle;
    char *addr = hx_session_label (hx_active_session ());

    /* Pre-connect (or during a reconnect, where the focused connection has no
     * label yet), skip the " (server)" suffix rather than printing "()". The
     * window gets re-titled by changetitlesconnected once the login lands. */
    if (*addr) {
        futuretitle = g_strdup_printf ("%s (%s)", name, addr);
    } else {
        futuretitle = g_strdup (name);
    }
    gtk_window_set_title (GTK_WINDOW (widget), futuretitle);
    g_free (addr);
    g_free (futuretitle);
}

void
changetitlesdisconnected (session *sess)
{
    /* One window, so this follows the focus too — see changetitlesconnected.
     * The per-panel titles it used to also set are gone: panels live inside
     * the toolbar window now, and their title is the tab label. */
    if (sess != hx_active_session () || !GTK_IS_WINDOW (sess->toolbar_window)) {
        return;
    }
    gtk_window_set_title (GTK_WINDOW (sess->toolbar_window), _ ("GtkHx"));
}

void
close_connected_windows (session *sess)
{
    if (sess->agreementwin) {
        gtkhx_widget_destroy (sess->agreementwin);
        sess->agreementwin = NULL;
    }
    /* legacy gfile_list cleanup is gone with the legacy files browser. The
     * orthodox-FM browser keeps one browser per session and tears each down
     * when its content page is destroyed (files_browser.c::browser_teardown),
     * so there is nothing to do from here. */

    /* walk sess->chats, closing the view of every non-public
     * pchat tab via the chat_tabs API. The public chat (cid=0) UI
     * persists across reconnects, like its model-side counterpart
     * in sess->chats.
     *
     * gtkhx_chat_tabs_close_pchat fires AdwTabView::close-page,
     * which runs the registered teardown (pchat_close in chat.c).
     * That teardown calls gchat_delete, which detaches the view.
     * Closing tabs mutates the chat registry as we go, so collect
     * cids in a first pass and close in a second.
     *
     * this was destroying gchat->window
     * directly, but gchat->window now points at the tab content
     * widget (hpane). Destroying that unparented the child but
     * left the AdwTabPage and the pchat_tabs index entry stale —
     * a real leak on every disconnect. Routing through the
     * close-page dispatcher keeps the AdwTabView, the registry
     * index, and gchat lifecycle consistent. */
    if (sess->chats) {
        GArray *cids = g_array_new (FALSE, FALSE, sizeof (guint32));
        guint n = hx_chats_count (sess->chats);
        for (guint i = 0; i < n; i++) {
            guint32 cid = hx_chats_cid_at (sess->chats, i);
            struct chat *c = hx_chats_get_at (sess->chats, i);
            /* Only non-public conversations that actually have an
             * open window need closing (models without a view have no
             * tab to tear down). */
            if (cid != 0 && hx_chat_view (c)) {
                g_array_append_val (cids, cid);
            }
        }
        for (guint i = 0; i < cids->len; i++) {
            guint32 cid = g_array_index (cids, guint32, i);
            gtkhx_chat_tabs_close_pchat (sess->htlc, cid);
        }
        g_array_free (cids, TRUE);
    }
}

/* gtkhx_text_to_utf8 lives in text_util.c now so the unit
 * tests can compile it without dragging in gtkutil's GTK / Adwaita
 * dependency tree. The prototype is forwarded via gtkutil.h →
 * text_util.h so existing #include "gtkutil.h" callers don't need to
 * change. */

/* error_dialog is an AdwAlertDialog now. The old GtkDialog
 * + manual GtkLabel + manual OK button + manual line-wrapping path
 * was the canonical example of "things AdwAlertDialog gives you for
 * free". libadwaita handles line wrapping inside the body text, the
 * dialog is auto-modal to its parent, ESC dismisses, and the visual
 * styling matches every other modern GNOME app's error popup. The
 * old add_break() helper that hand-inserted '\n' every 50 chars is
 * gone with it. */
void
error_dialog (char *title, char *msg)
{
    AdwDialog *dlg;

    dlg = adw_alert_dialog_new (title, msg);
    adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dlg), "ok", _ ("_OK"));
    adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dlg), "ok");
    adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dlg), "ok");

    /* Ctrl+W / Ctrl+Q for keyboard parity. Esc dismisses via the
     * close_response set above. */
    gtkhx_dialog_add_close_shortcuts (GTK_WIDGET (dlg));

    adw_dialog_present (dlg, GTK_WIDGET (gtkhx_active_window ()));
}

GtkWidget *
gtkhx_grid_new_table (int rows, int cols, gboolean homogeneous)
{
    GtkWidget *grid = gtk_grid_new ();
    (void)rows;
    (void)cols; /* Grid grows automatically. */
    if (homogeneous) {
        gtk_grid_set_row_homogeneous (GTK_GRID (grid), TRUE);
        gtk_grid_set_column_homogeneous (GTK_GRID (grid), TRUE);
    }
    return grid;
}

void
gtkhx_grid_attach_table (GtkGrid *grid, GtkWidget *child, int left, int right,
                         int top, int bottom, int xoptions, int yoptions,
                         int xpad, int ypad)
{
    if (xoptions & GTK_EXPAND) {
        gtk_widget_set_hexpand (child, TRUE);
    }
    if (yoptions & GTK_EXPAND) {
        gtk_widget_set_vexpand (child, TRUE);
    }
    gtk_widget_set_halign (child, (xoptions & GTK_FILL) ? GTK_ALIGN_FILL
                                                        : GTK_ALIGN_CENTER);
    gtk_widget_set_valign (child, (yoptions & GTK_FILL) ? GTK_ALIGN_FILL
                                                        : GTK_ALIGN_CENTER);
    if (xpad) {
        gtk_widget_set_margin_start (child, xpad);
        gtk_widget_set_margin_end (child, xpad);
    }
    if (ypad) {
        gtk_widget_set_margin_top (child, ypad);
        gtk_widget_set_margin_bottom (child, ypad);
    }
    gtk_grid_attach (grid, child, left, top, right - left, bottom - top);
}

void
gtkhx_grid_attach_table_defaults (GtkGrid *grid, GtkWidget *child, int left,
                                  int right, int top, int bottom)
{
    /* Mirror gtk_table_attach_defaults: GTK_EXPAND|GTK_FILL on both
     * axes, no padding. */
    gtk_widget_set_hexpand (child, TRUE);
    gtk_widget_set_vexpand (child, TRUE);
    gtk_widget_set_halign (child, GTK_ALIGN_FILL);
    gtk_widget_set_valign (child, GTK_ALIGN_FILL);
    gtk_grid_attach (grid, child, left, top, right - left, bottom - top);
}

/* GtkContainer is gone — dispatch on parent type to the
 * right child setter. Box gets append (call sites that want
 * gtk_box_pack_start semantics should use gtkhx_box_pack instead;
 * this helper covers the simple "put one child in a parent" case
 * gtk_container_add was usually doing). */
void
gtkhx_widget_set_child (GtkWidget *parent, GtkWidget *child)
{
    if (!parent || !child) {
        return;
    }

    if (GTK_IS_WINDOW (parent)) {
        gtk_window_set_child (GTK_WINDOW (parent), child);
    } else if (GTK_IS_SCROLLED_WINDOW (parent)) {
        gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (parent), child);
    } else if (GTK_IS_FRAME (parent)) {
        gtk_frame_set_child (GTK_FRAME (parent), child);
    } else if (GTK_IS_BUTTON (parent)) {
        gtk_button_set_child (GTK_BUTTON (parent), child);
    } else if (GTK_IS_BOX (parent)) {
        gtk_box_append (GTK_BOX (parent), child);
    } else if (GTK_IS_VIEWPORT (parent)) {
        gtk_viewport_set_child (GTK_VIEWPORT (parent), child);
    } else if (GTK_IS_POPOVER (parent)) {
        gtk_popover_set_child (GTK_POPOVER (parent), child);
    } else if (GTK_IS_LIST_BOX_ROW (parent)) {
        gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (parent), child);
    } else if (GTK_IS_LIST_BOX (parent)) {
        gtk_list_box_append (GTK_LIST_BOX (parent), child);
    } else {
        g_warning ("gtkhx_widget_set_child: unhandled parent type %s",
                   G_OBJECT_TYPE_NAME (parent));
    }
}

void
gtkhx_widget_remove_child (GtkWidget *parent, GtkWidget *child)
{
    if (!parent || !child) {
        return;
    }

    if (GTK_IS_BOX (parent)) {
        gtk_box_remove (GTK_BOX (parent), child);
    } else if (GTK_IS_LIST_BOX (parent)) {
        gtk_list_box_remove (GTK_LIST_BOX (parent), child);
    } else {
        gtk_widget_unparent (child);
    }
}

static void
gtkhx_box_pack_apply (GtkWidget *child, GtkOrientation orient, gboolean expand,
                      gboolean fill, guint padding)
{
    gboolean horiz = (orient == GTK_ORIENTATION_HORIZONTAL);

    if (expand) {
        if (horiz) {
            gtk_widget_set_hexpand (child, TRUE);
        } else {
            gtk_widget_set_vexpand (child, TRUE);
        }
    }
    if (fill) {
        if (horiz) {
            gtk_widget_set_halign (child, GTK_ALIGN_FILL);
        } else {
            gtk_widget_set_valign (child, GTK_ALIGN_FILL);
        }
    }
    if (padding) {
        if (horiz) {
            gtk_widget_set_margin_start (child, padding);
            gtk_widget_set_margin_end (child, padding);
        } else {
            gtk_widget_set_margin_top (child, padding);
            gtk_widget_set_margin_bottom (child, padding);
        }
    }
}

void
gtkhx_box_pack (GtkWidget *box, GtkWidget *child, gboolean expand,
                gboolean fill, guint padding)
{
    if (!box || !child) {
        return;
    }
    g_return_if_fail (GTK_IS_BOX (box));
    gtkhx_box_pack_apply (child,
                          gtk_orientable_get_orientation (GTK_ORIENTABLE (box)),
                          expand, fill, padding);
    gtk_box_append (GTK_BOX (box), child);
}

void
gtkhx_box_pack_end (GtkWidget *box, GtkWidget *child, gboolean expand,
                    gboolean fill, guint padding)
{
    GtkOrientation orient;

    if (!box || !child) {
        return;
    }
    g_return_if_fail (GTK_IS_BOX (box));
    orient = gtk_orientable_get_orientation (GTK_ORIENTABLE (box));
    gtkhx_box_pack_apply (child, orient, expand, fill, padding);
    /* Push toward the trailing edge to mimic gtk_box_pack_end. */
    if (orient == GTK_ORIENTATION_HORIZONTAL) {
        gtk_widget_set_halign (child, GTK_ALIGN_END);
    } else {
        gtk_widget_set_valign (child, GTK_ALIGN_END);
    }
    gtk_box_append (GTK_BOX (box), child);
}

/* gdk_memory_texture_new's GBytes-backed free path: when the
 * texture (and thus the GBytes) drops, this fires and unrefs
 * the pixbuf we kept alive. Bridging through GDestroyNotify
 * keeps the function pointer types lined up — g_object_unref
 * has a slightly different signature shape that the strict
 * gcc -Wincompatible-pointer-types catches. */
static void
gtkhx_texture_pixbuf_pixels_unref (gpointer data)
{
    g_object_unref ((GObject *)data);
}

/* gdk_texture_new_for_pixbuf is deprecated in GTK
 * 4.16 in favour of the GBytes / gdk_memory_texture_new path.
 * Every GtkHx icon comes through a GdkPixbuf source today
 * (GResource lookups, the Mac CICN decoder, gdk-pixbuf loader
 * paths, etc.), so this helper centralises the conversion to
 * a non-deprecated GdkTexture and the per-site migration is
 * just a name swap from the legacy call.
 *
 * GdkPixbuf storage is always 8-bit per channel, RGB or RGBA.
 * Alpha is straight (not premultiplied). Map to the matching
 * GdkMemoryFormat; stride + width + height come straight off
 * the pixbuf accessors.
 *
 * Pixel buffer ownership: gdk_pixbuf_read_pixels returns a
 * `const guint8 *` borrowed pointer into the pixbuf's
 * internal buffer (the const-correct successor to the older
 * gdk_pixbuf_get_pixels — same backing storage, just typed as
 * read-only). The pixbuf must outlive the GBytes (which the
 * texture refs). We hold a strong ref on the pixbuf in the
 * bytes' free_func and drop it when the bytes go away. */
GdkTexture *
gtkhx_texture_from_pixbuf (GdkPixbuf *pixbuf)
{
    int width;
    int height;
    int channels;
    int stride;
    gsize buffer_size;
    GdkMemoryFormat format;
    GBytes *bytes;
    GdkTexture *texture;

    if (!pixbuf) {
        return NULL;
    }

    channels = gdk_pixbuf_get_n_channels (pixbuf);
    if (channels == 4) {
        format = GDK_MEMORY_R8G8B8A8;
    } else if (channels == 3) {
        format = GDK_MEMORY_R8G8B8;
    } else {
        /* GdkPixbuf API contract: 8-bit-per-sample RGB or
         * RGBA, so n_channels is always 3 or 4. A buggy or
         * pre-loaded pixbuf reporting something else would
         * silently render garbage — fail closed instead. */
        return NULL;
    }

    width = gdk_pixbuf_get_width (pixbuf);
    height = gdk_pixbuf_get_height (pixbuf);
    stride = gdk_pixbuf_get_rowstride (pixbuf);
    if (width <= 0 || height <= 0 || stride <= 0) {
        return NULL;
    }

    buffer_size = (gsize)stride * (gsize)height;

    g_object_ref (pixbuf);
    bytes = g_bytes_new_with_free_func (
        gdk_pixbuf_read_pixels (pixbuf), buffer_size,
        gtkhx_texture_pixbuf_pixels_unref, pixbuf);
    texture
        = gdk_memory_texture_new (width, height, format, bytes, (gsize)stride);
    g_bytes_unref (bytes);
    return texture;
}

/* gtk_image_new_from_pixbuf is deprecated in GTK 4.12.
 * Builds a GtkImage from a paintable backed by the texture helper
 * above. Returns a fresh-floating GtkImage; the caller takes
 * ownership the same way as the legacy gtk_image_new_from_pixbuf. */
GtkWidget *
gtkhx_image_new_from_pixbuf (GdkPixbuf *pixbuf)
{
    GtkWidget *image;
    GdkTexture *tex;

    if (!pixbuf) {
        return gtk_image_new ();
    }

    tex = gtkhx_texture_from_pixbuf (pixbuf);
    if (!tex) {
        return gtk_image_new ();
    }

    image = gtk_image_new_from_paintable (GDK_PAINTABLE (tex));
    g_object_unref (tex);
    return image;
}

/* ---- Theme-scaled icon buttons --------------------------------
 *
 * The default toolbar / window-button pixmaps are 16x16 pixel art
 * that looks tiny at modern desktop sizes, so they're scaled up with
 * GDK_INTERP_NEAREST (preserves the crisp blocky pixels — bilinear
 * scaling would blur them into mush). The scale factor comes entirely
 * from the theme: gtkhx_theme_scale(area) times the source dimension,
 * where the historical 2x upscale now lives honestly in the default
 * theme rather than as a hidden constant here (see gtkhx_theme.{c,h}).
 *
 * Each button remembers its source (a GResource name or a held pixbuf
 * ref) and its area as object data, and subscribes to the theme
 * "changed" signal so a Settings tweak rescales it live with no
 * restart. The subscription auto-drops on button finalize via
 * g_signal_connect_object.
 *
 * Rendering goes through GtkPicture rather than GtkImage: GtkImage has
 * a default min-width / min-height of ~16px from its Adwaita CSS that
 * clamps the visible size regardless of the source paintable's natural
 * dimensions, so a 32x32 pixbuf in a GtkImage would still render at
 * 16x16. GtkPicture doesn't carry those constraints — with
 * set_can_shrink(FALSE) it renders at the paintable's natural size. */

#define BTN_KEY_RESOURCE "gtkhx-pixmap-resource"
#define BTN_KEY_PIXBUF "gtkhx-pixmap-pixbuf"
#define BTN_KEY_AREA "gtkhx-scale-area"

/* Compute effective pixel dimensions for a source pixbuf at the
 * button's area scale, clamped to >= 1. */
static void
button_scaled_size (GdkPixbuf *src, GtkhxScaleArea area, int *out_w, int *out_h)
{
    double mul = gtkhx_theme_scale (area);
    int w = (int)(gdk_pixbuf_get_width (src) * mul + 0.5);
    int h = (int)(gdk_pixbuf_get_height (src) * mul + 0.5);
    *out_w = w < 1 ? 1 : w;
    *out_h = h < 1 ? 1 : h;
}

/* Load the source pixbuf for a tracked button. Returns a new ref the
 * caller owns, or NULL if neither source is available.
 *
 * The caller-supplied pixbuf path (BTN_KEY_PIXBUF set by
 * gtkhx_pixbuf_button) is the stable source — return a ref to it
 * directly. The resource-path branch (BTN_KEY_RESOURCE set by
 * gtkhx_pixmap_button) goes through gtkhx_icon_load so the active
 * theme's bundled icons ($CONFIG/themes/<theme>/icons/<basename>.png,
 * or the theme's GResource dir) can shadow the stock pixmap per
 * icon — the resolver's own cache absorbs the repeated-decode cost
 * across theme "changed" fan-outs. We deliberately do NOT stash the
 * resolver's result in BTN_KEY_PIXBUF — that would survive a theme
 * switch and keep the old theme's icon on screen even after
 * gtkhx_icon_invalidate_cache drops the resolver-side entry. */
static GdkPixbuf *
button_load_source (GtkWidget *btn)
{
    GdkPixbuf *source_pb = g_object_get_data (G_OBJECT (btn), BTN_KEY_PIXBUF);
    if (source_pb) {
        return g_object_ref (source_pb);
    }

    const char *resource_name
        = g_object_get_data (G_OBJECT (btn), BTN_KEY_RESOURCE);
    if (resource_name) {
        return gtkhx_icon_load (resource_name);
    }
    return NULL;
}

/* Rebuild a button's GtkPicture child from its tracked source at the
 * current theme scale. Called once at construction and on every theme
 * "changed" emission (connected swapped via g_signal_connect_object,
 * so the user_data btn arrives first and the GtkhxTheme instance
 * arrives second). Signature has to match that swapped shape even
 * when called directly — passing NULL for the unused theme arg from
 * the direct call site keeps both call paths going through one
 * prototype. */
static void
button_refresh_picture (GtkWidget *btn, gpointer unused_theme)
{
    GtkhxScaleArea area = (GtkhxScaleArea)GPOINTER_TO_INT (
        g_object_get_data (G_OBJECT (btn), BTN_KEY_AREA));
    (void)unused_theme;
    GdkPixbuf *src, *use_pb;
    GdkTexture *tex;
    GtkWidget *picture;
    int w, h;

    src = button_load_source (btn);
    if (!src) {
        gtkhx_widget_set_child (btn, gtk_picture_new ());
        return;
    }

    button_scaled_size (src, area, &w, &h);
    /* Fast path: a scale that lands on the source size skips the
     * scale_simple allocation. */
    if (w == gdk_pixbuf_get_width (src) && h == gdk_pixbuf_get_height (src)) {
        use_pb = src; /* transfer ownership */
    } else {
        use_pb = gdk_pixbuf_scale_simple (src, w, h, GDK_INTERP_NEAREST);
        g_object_unref (src);
        if (!use_pb) {
            /* scale_simple can fail under OOM — blank rather than
             * dereference NULL. */
            gtkhx_widget_set_child (btn, gtk_picture_new ());
            return;
        }
    }

    tex = gtkhx_texture_from_pixbuf (use_pb);
    if (tex) {
        picture = gtk_picture_new_for_paintable (GDK_PAINTABLE (tex));
        g_object_unref (tex);
        gtk_picture_set_can_shrink (GTK_PICTURE (picture), FALSE);
    } else {
        picture = gtk_picture_new ();
    }
    gtkhx_widget_set_child (btn, picture);
    g_object_unref (use_pb);
}

/* Common tail: stash the area, render the first picture, subscribe to
 * the theme, and wire tooltip + clicked. */
static void
button_finish_setup (GtkWidget *btn, GtkhxScaleArea area, const char *tooltip,
                     GCallback cb, gpointer user_data)
{
    g_object_set_data (G_OBJECT (btn), BTN_KEY_AREA, GINT_TO_POINTER (area));
    button_refresh_picture (btn, NULL);
    g_signal_connect_object (gtkhx_theme_get_default (), "changed",
                             G_CALLBACK (button_refresh_picture), btn,
                             G_CONNECT_SWAPPED);
    if (tooltip) {
        gtk_widget_set_tooltip_text (btn, tooltip);
    }
    if (cb) {
        g_signal_connect (btn, "clicked", cb, user_data);
    }
}

GtkWidget *
gtkhx_pixmap_button (const char *resource_name, const char *tooltip,
                     GtkhxScaleArea area, GCallback cb, gpointer user_data)
{
    GtkWidget *btn = gtk_button_new ();

    /* Own a copy of the resource name: button_load_source dereferences
     * it on every theme "changed" emission, which can outlive a
     * caller's stack/heap buffer. g_strdup + g_free destroy-notify
     * keeps it alive for the button's lifetime regardless of what the
     * caller passed. (g_strdup(NULL) is NULL — button_load_source
     * null-checks.) */
    g_object_set_data_full (G_OBJECT (btn), BTN_KEY_RESOURCE,
                            g_strdup (resource_name), g_free);
    button_finish_setup (btn, area, tooltip, cb, user_data);
    return btn;
}

/* Pixbuf-source companion. The caller already has a GdkPixbuf (e.g.
 * from cicn_to_pixbuf via load_icon); the button takes its own ref so
 * it can rescale live, and drops it on finalize. Returns NULL only if
 * pixbuf is NULL — call sites that allow a missing icon should
 * null-check the return. */
GtkWidget *
gtkhx_pixbuf_button (GdkPixbuf *pixbuf, const char *tooltip,
                     GtkhxScaleArea area, GCallback cb, gpointer user_data)
{
    GtkWidget *btn;

    if (!pixbuf) {
        return NULL;
    }
    btn = gtk_button_new ();
    g_object_set_data_full (G_OBJECT (btn), BTN_KEY_PIXBUF,
                            g_object_ref (pixbuf), g_object_unref);
    button_finish_setup (btn, area, tooltip, cb, user_data);
    return btn;
}

/* gtkhx_widget_destroy is gone. Toplevels (GtkWindow) use
 * gtk_window_destroy which tears down the surface and drops refs.
 * Non-toplevels: if the widget has a parent, unparent it (the
 * parent drops its ref); if floating, sink + unref. */
void
gtkhx_widget_destroy (GtkWidget *widget)
{
    GtkWidget *parent;

    if (!widget) {
        return;
    }
    if (GTK_IS_WINDOW (widget)) {
        gtk_window_destroy (GTK_WINDOW (widget));
        return;
    }
    parent = gtk_widget_get_parent (widget);
    if (parent) {
        gtk_widget_unparent (widget);
    } else if (g_object_is_floating (widget)) {
        g_object_ref_sink (widget);
        g_object_unref (widget);
    } else {
        g_object_unref (widget);
    }
}
