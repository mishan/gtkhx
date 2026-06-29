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
#include "hx.h"
#include "network.h"
#include "news.h"
#include "news15.h"
#include "news_browser.h"
#include "files_browser.h"
#include "xfers.h"
#include "gtkutil.h"
#include "text_util.h"
#include "tracker.h"
#include "tray.h"
#include "gtkhx.h"
#include "users.h"
#include "chat.h"
#include "tasks.h"
#include "options.h"
#ifdef HAVE_VOICE
#include "voice_ptt.h"
#endif
#include "connect.h"
#include "files.h"
#include "hl_access.h"
#include "msg.h"
#include "usermod.h"
#include "about.h"
#include "banner.h"
#include "bookmarks.h"
#include "plugin.h"
#include "toolbar.h"
#include "hx_panel.h"
#include "hx_panel_frame.h"
#include "hx_split.h"
#include "dock_layout.h"
#include "panel_registry.h"

GtkWidget *toolbar_window, *files_btn, *connect_btn;
GtkWidget *disconnect_btn, *news15_btn, *news_btn;
GtkWidget *broadcast_btn;

/* handles to the dock + four default-leaf
 * PanelFrame globals that per-window panel factories use to
 * insert their HxPanels. See toolbar.h for the contract.
 *
 * toolbar_dock is a libpanel PanelDock acting as a thin wrapper
 * around the HxSplit tree — the dock has exactly one center
 * child, the HxSplit root, and no other children. The wrapper
 * exists to satisfy libpanel's PanelDropControls invariant
 * (which assert a PANEL_TYPE_DOCK ancestor at root time); from
 * the user's perspective the dock is a single recursive HxSplit
 * tree. See docs/docking-splits.md for the rationale. */
GtkWidget *toolbar_dock           = NULL;  /* thin PanelDock wrapper */
GtkWidget *toolbar_sidebar_frame  = NULL;
GtkWidget *toolbar_end_frame      = NULL;
GtkWidget *toolbar_bottom_frame   = NULL;
GtkWidget *toolbar_center_frame   = NULL;

#ifdef USE_PLUGIN
GtkWidget *plugin_btn;
#endif

/* status_bar is now a GtkLabel. The previous GtkStatusbar
 * was deprecated in GTK 4.10 and we never used its stack-of-messages
 * model — we always replaced the message wholesale. set_status_bar()
 * in gtkutil.c does a single gtk_label_set_text() now. The
 * status_msg / context_status globals are gone with it. */
GtkWidget *status_bar;

/* AdwToastOverlay anchors transient notifications over the
 * toolbar content. set_status_bar() pushes a toast for "Logged in"
 * to give positive feedback at connection time; the persistent
 * status label still shows the current state for ambient awareness.
 * Static — only the toolbar.c-internal wiring touches it directly,
 * external callers go through toolbar_show_toast(). */
static AdwToastOverlay *toolbar_toast;

/* Borrowed pointers to AdwToast instances we've handed to
 * toolbar_toast that haven't been dismissed yet. The overlay owns
 * the refs; we just need to know which toasts are still on screen
 * so toolbar_clear_toasts can dismiss them when the user moves to
 * a new server. Entries are removed from the list by each toast's
 * "dismissed" handler, which fires whether the toast timed out,
 * was swiped away, or was programmatically dismissed — so the list
 * stays accurate without explicit cleanup. */
static GList *live_toasts = NULL;

/* AdwBanner sits above the content row and surfaces
 * connection-issue state that needs user action — typically "Lost
 * connection — Reconnect" after an unexpected disconnect. Hidden by
 * default; toolbar_show_connection_lost / toolbar_hide_banner toggle
 * the "revealed" property. The banner button reconnects to the same
 * server we just lost — connect_reconnect_last replays the cached
 * params from connect.c without showing the dialog (one-click
 * reconnect after a network blip). The cache falls back to opening
 * the dialog if it's empty. */
static AdwBanner *toolbar_banner;

static void
on_banner_button_clicked (AdwBanner *banner, gpointer user_data)
{
    (void)banner;
    (void)user_data;
    if (toolbar_banner) {
        adw_banner_set_revealed (toolbar_banner, FALSE);
    }
    connect_reconnect_last ();
}

/* New User / Edit User used to be standalone toolbar
 * buttons. They're admin actions used rarely (only by sysops),
 * so they fold into an Admin submenu in the hamburger. The
 * GActions below carry the same dispatch the old click handlers
 * did; their enabled state is toggled in setbtns() to mirror
 * the historic per-button sensitivity. */
static void
on_action_user_new (GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action;
    (void)param;
    (void)user_data;
    create_useredit_window (0, 1);
}

static void
on_action_user_edit (GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action;
    (void)param;
    (void)user_data;
    useredit_open_dialog ();
}

/* AdwAlertDialog "response" handler for the Broadcast button.
 * Pulls the text out of the AdwEntryRow extra-child and sends it
 * via HTLC_HDR_MSG_BROADCAST. Cancel / close-via-X / empty text
 * is a silent no-op. */
static void
on_broadcast_response (AdwAlertDialog *dialog, const char *response,
                       gpointer data)
{
    GtkWidget *entry = data;
    const char *text;
    gsize len;

    (void)dialog;
    if (g_strcmp0 (response, "send") != 0) {
        return;
    }
    text = gtk_editable_get_text (GTK_EDITABLE (entry));
    if (!text || !*text) {
        return;
    }
    len = strlen (text);
    if (len > 0xfffe) {
        len = 0xfffe; /* HTLC_DATA_MSG length is u16; clamp. */
    }
    hx_send_broadcast (&the_session.htlc, text, (guint16)len);
}

/* AdwEntryRow swallows Enter for its own entry-activated signal,
 * which bypasses the dialog's default-response binding. Bridge it
 * by running the same logic as the Send button (see the open-user
 * dialog for the same trick). */
static void
on_broadcast_entry_activated (AdwEntryRow *entry, gpointer data)
{
    AdwAlertDialog *dialog = data;
    const char *text = gtk_editable_get_text (GTK_EDITABLE (entry));
    gsize len;
    if (text && *text) {
        len = strlen (text);
        if (len > 0xfffe) {
            len = 0xfffe;
        }
        hx_send_broadcast (&the_session.htlc, text, (guint16)len);
    }
    adw_dialog_close (ADW_DIALOG (dialog));
}

/* Open the Broadcast composer — an AdwAlertDialog with an
 * AdwEntryRow in an AdwPreferencesGroup, mirroring the Open User
 * dialog's shape. Send is the default + suggested-action response;
 * Cancel / Esc / window-close dismiss without sending. The button
 * is hidden when the account lacks HL_ACCESS_CAN_BROADCAST, so by
 * the time this fires we expect the server to accept; if it
 * doesn't, the task_error path handles the rejection. */
static void
on_broadcast_button_clicked (GtkButton *btn, gpointer user_data)
{
    AdwDialog *dialog;
    GtkWidget *prefs_grp;
    GtkWidget *entry;
    (void)btn;
    (void)user_data;

    if (!the_session.htlc.fd) {
        return;
    }

    dialog = adw_alert_dialog_new (
        _ ("Broadcast"),
        _ ("Send a broadcast message to every user on the server."));

    adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "cancel",
                                   _ ("_Cancel"));
    adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "send",
                                   _ ("_Send"));
    adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog), "send",
                                              ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "send");
    adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "cancel");

    prefs_grp = adw_preferences_group_new ();
    entry = adw_entry_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (entry),
                                   _ ("Message"));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (prefs_grp), entry);
    adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dialog), prefs_grp);

    gtkhx_dialog_add_close_shortcuts (GTK_WIDGET (dialog));

    g_signal_connect (dialog, "response", G_CALLBACK (on_broadcast_response),
                      entry);
    g_signal_connect (entry, "entry-activated",
                      G_CALLBACK (on_broadcast_entry_activated), dialog);

    adw_dialog_present (dialog,
                        toolbar_window ? GTK_WIDGET (toolbar_window) : NULL);
    gtk_widget_grab_focus (entry);
}

/* app.open_bookmark fires from the AdwSplitButton's
 * dropdown menu. The GVariant parameter carries the bookmark
 * name as a string; connect_open_bookmark_by_name reads the
 * file and dispatches to the existing connect path. */
static void
on_action_open_bookmark (GSimpleAction *action, GVariant *param,
                         gpointer user_data)
{
    const char *name;
    (void)action;
    (void)user_data;

    if (!param || !g_variant_is_of_type (param, G_VARIANT_TYPE_STRING)) {
        return;
    }
    name = g_variant_get_string (param, NULL);
    connect_open_bookmark_by_name (name);
}

/* app.connect_builtin fires from the SplitButton's
 * dropdown for one of the hardcoded "well-known" Hotline servers.
 * Index 1..4 — same numbering the connect dialog's built-in combo
 * has used since forever. */
static void
on_action_connect_builtin (GSimpleAction *action, GVariant *param,
                           gpointer user_data)
{
    (void)action;
    (void)user_data;

    if (!param || !g_variant_is_of_type (param, G_VARIANT_TYPE_INT32)) {
        return;
    }
    connect_open_builtin_bookmark (g_variant_get_int32 (param));
}

/* GTK 4 close-request signature is (GtkWindow *, gpointer)
 * returning gboolean. Returning TRUE inhibits the default destroy.
 *
 * when the tray icon is enabled AND a tray host is around
 * to render it, X-button closes hide all windows instead of
 * quitting — the conventional "minimize-to-tray" pattern. We require
 * an actual host (not just the pref) so users with the toggle on
 * but no AppIndicator extension installed don't end up unable to
 * exit the app. Without an SNI host, fall through to a quit-
 * confirmation dialog. */
static void
quit_confirm_response (AdwAlertDialog *dialog, const char *response,
                       gpointer data)
{
    (void)dialog;
    (void)data;
    if (g_strcmp0 (response, "quit") == 0) {
        hx_quit ();
    }
    /* "cancel" / Esc / window-close: do nothing — toolbar window
     * stays open. The close-request that brought us here already
     * returned TRUE to inhibit destroy. */
}

/* Debounced toolbar-resize save. notify::default-width / -height
 * fire on every pixel of a user drag — coalesce them on a 500 ms
 * idle so the prefs write runs once when the user lets go. The
 * actual save uses the existing save path: gtk_window_get_default_size
 * → gtkhx_prefs.geo.tool → prefs_write. */
static guint toolbar_size_save_idle = 0;

static gboolean
on_toolbar_size_save_idle (gpointer data)
{
    int w = 0, h = 0;

    (void)data;
    toolbar_size_save_idle = 0;
    if (toolbar_window == NULL || !gtk_widget_get_realized (toolbar_window))
        return G_SOURCE_REMOVE;

    gtk_window_get_default_size (GTK_WINDOW (toolbar_window), &w, &h);
    if (w > 0) gtkhx_prefs.geo.tool.xsize = w;
    if (h > 0) gtkhx_prefs.geo.tool.ysize = h;
    prefs_write ();
    return G_SOURCE_REMOVE;
}

static void
on_toolbar_size_notify (GObject *object, GParamSpec *pspec, gpointer data)
{
    (void)object; (void)pspec; (void)data;
    /* Debounce, not throttle: cancel + reschedule on every notify
     * so a drag-resize burst collapses to one prefs_write 500 ms
     * after the user lets go, not one every 500 ms across the
     * drag. Same fix as dock_layout's request_save. */
    if (toolbar_size_save_idle != 0)
        g_source_remove (toolbar_size_save_idle);
    toolbar_size_save_idle = g_timeout_add (500, on_toolbar_size_save_idle,
                                            NULL);
}

static gboolean
close_toolbar_window (GtkWindow *window, gpointer data)
{
    AdwDialog *dialog;
    (void)data;

    if (gtkhx_tray_is_enabled () && gtkhx_tray_host_available ()) {
        gtkhx_tray_hide_all_windows ();
        return TRUE; /* inhibit destroy */
    }

    /* No tray host — confirm before quitting so the user doesn't
     * lose an active connection (or just an open chat) by accident.
     * AdwAlertDialog so the dialog adapts to the OS theme; the
     * "quit" response gets DESTRUCTIVE appearance to mark it as
     * the irreversible action. */
    dialog = adw_alert_dialog_new (
        _ ("Quit GtkHx?"),
        _ ("Closing this window will disconnect from the current "
           "server and exit GtkHx."));
    adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "cancel",
                                   _ ("_Cancel"));
    adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "quit",
                                   _ ("_Quit"));
    adw_alert_dialog_set_response_appearance (
        ADW_ALERT_DIALOG (dialog), "quit", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog),
                                           "cancel");
    adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog),
                                         "cancel");
    g_signal_connect (dialog, "response",
                      G_CALLBACK (quit_confirm_response), NULL);
    adw_dialog_present (dialog, GTK_WIDGET (window));

    return TRUE; /* inhibit destroy until the user confirms */
}

void
disconnect_clicked (void)
{
    if (!connected) {
        kill_threads ();
        setbtns (&the_session, 0);
        set_status_bar (0);
        set_disconnect_btn (&the_session, 0);
        conn_task_update (&the_session, 2);
        /* hx_htlc_close (which set connected=0) already detached
         * the GPollable sources and released current_conn, so the
         * legacy hxd_fd_clr + close(fd) cleanup here would either
         * be a no-op or worse — close(fd) would target a fd
         * already owned (and possibly closed) by the released
         * GSocketConnection. Just clear the gdk_input bookkeeping
         * flag and emit the user-visible notice. */
        the_session.htlc.gdk_input = 0;
        hx_printf_prefix (&the_session.htlc, 0, INFOPREFIX, "%s: %s\n",
                          server_addr, _ ("connection closed"));
    }

    else if (the_session.htlc.fd) {
        hx_htlc_close (&the_session.htlc, 1);
    }
}

/* app-level GActions backing the AdwHeaderBar's hamburger
 * menu. The actions just dispatch to the existing window-creation
 * functions so the menu items stay one g_action_map_add_action_entries
 * call away from working alongside the historic toolbar buttons. The
 * GAction approach also means the menu items pick up keyboard
 * accelerators for free if we ever wire any (gtk_application_set_accels_for_action).
 *
 * The action callbacks defer their real work to g_idle_add — when the
 * action fires from a hamburger menu item, the popover is mid-dismiss
 * and the GdkSurface for it is still alive on the click stack. Building
 * a new top-level dialog (especially the AdwPreferencesWindow with
 * its 9 pages and the icon-picker FlowBox that walks main-loop
 * iterations) inside that callstack hit a Heisenbug — segfault on bare run, no
 * crash under gdb. Letting the click chain unwind first via the idle
 * source side-steps it cleanly. */
static gboolean
defer_open_settings (gpointer data)
{
    create_options_window (NULL, data);
    return G_SOURCE_REMOVE;
}

static gboolean
defer_open_about (gpointer data)
{
    (void)data;
    create_about_window (NULL, NULL);
    return G_SOURCE_REMOVE;
}

static gboolean
defer_open_bookmarks (gpointer data)
{
    (void)data;
    create_bookmarks_window ();
    return G_SOURCE_REMOVE;
}

static gboolean
defer_quit (gpointer data)
{
    (void)data;
    hx_quit ();
    return G_SOURCE_REMOVE;
}

static void
on_action_settings (GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action;
    (void)param;
    g_idle_add (defer_open_settings, user_data);
}

static void
on_action_about (GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action;
    (void)param;
    (void)user_data;
    g_idle_add (defer_open_about, NULL);
}

static void
on_action_bookmarks (GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action;
    (void)param;
    (void)user_data;
    g_idle_add (defer_open_bookmarks, NULL);
}

static void
on_action_quit (GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action;
    (void)param;
    (void)user_data;
    g_idle_add (defer_quit, NULL);
}

static void
on_action_reset_layout (GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action; (void)param; (void)user_data;
    /* Wipe the saved file. The current in-memory dock isn't
     * rebuilt to defaults — that would require tearing down and
     * re-creating every panel — but the NEXT launch comes up
     * with the default layout. A toast tells the user this so
     * they don't think the action no-op'd. */
    dock_layout_reset ();
    toolbar_show_toast (_ ("Layout will reset on next launch."));
}

/* on_files_button_clicked
 * retired. The Files button now uses the toolbar_show_panel
 * helper directly; for the first click on Files (when the panel
 * doesn't exist yet) the panel factory runs at toolbar build
 * time, so the registry lookup always succeeds. */

static const GActionEntry app_actions[] = {
    { .name = "settings", .activate = on_action_settings },
    { .name = "bookmarks", .activate = on_action_bookmarks },
    { .name = "about", .activate = on_action_about },
    { .name = "user_new", .activate = on_action_user_new },
    { .name = "user_edit", .activate = on_action_user_edit },
    { .name = "open_bookmark",
      .activate = on_action_open_bookmark,
      .parameter_type = "s" },
    { .name = "connect_builtin",
      .activate = on_action_connect_builtin,
      .parameter_type = "i" },
    { .name = "quit", .activate = on_action_quit },
    { .name = "reset_layout", .activate = on_action_reset_layout },
};

/* live_toasts bookkeeping — remove the dismissed toast from the
 * list so toolbar_clear_toasts doesn't try to dismiss something
 * the user (or the auto-timeout) already cleaned up. Fires
 * synchronously from the overlay before the overlay drops its
 * own ref, so the borrowed pointer in our list is still valid
 * here. */
static void
on_toast_dismissed (AdwToast *toast, gpointer user_data)
{
    (void)user_data;
    live_toasts = g_list_remove (live_toasts, toast);
}

/* push a transient AdwToast onto the toolbar window's
 * AdwToastOverlay. No-op until the toolbar is built (toolbar_toast
 * starts NULL), so callers can safely fire toasts during early
 * startup without guarding. The toast auto-dismisses after libadwaita's
 * default timeout (~5s); duplicate text replaces the previous toast
 * cleanly. */
void
toolbar_show_toast (const char *text)
{
    char *safe = NULL;
    const char *body;
    AdwToast *toast;

    if (!toolbar_toast || !text) {
        return;
    }

    /* AdwToast stores the title as a UTF-8 string and the
	 * accessibility layer behind it (libadwaita →
	 * gtk_accessible_announce → g_variant_new_string) abort()s
	 * the process on non-UTF-8 input. Most call sites feed
	 * server-supplied bytes (task error strings, broadcast
	 * messages) that can be MacRoman from old Mac servers, so
	 * defend the choke point: validate, fall back to MacRoman
	 * conversion, finally U+FFFD substitution. */
    if (!g_utf8_validate (text, -1, NULL)) {
        safe = gtkhx_text_to_utf8 (text, strlen (text), NULL);
        body = safe ? safe : "";
    } else {
        body = text;
    }

    toast = adw_toast_new (body);
    g_signal_connect (toast, "dismissed", G_CALLBACK (on_toast_dismissed),
                      NULL);
    /* Append before handing to the overlay so a synchronous
	 * "dismissed" emit (libadwaita doesn't currently do this, but
	 * the contract doesn't forbid it either) finds the entry to
	 * remove. */
    live_toasts = g_list_append (live_toasts, toast);
    adw_toast_overlay_add_toast (toolbar_toast, toast);

    g_free (safe);
}

/* Dismiss every toast still on screen so the next connection starts
 * with a clean overlay. Wired into the connection-state hook on the
 * CONNECTING transition (see gtkhx.c). Iterates a snapshot of the
 * live list because adw_toast_dismiss fires "dismissed"
 * synchronously, which mutates the original list under us. */
void
toolbar_clear_toasts (void)
{
    GList *snapshot;

    if (!live_toasts) {
        return;
    }
    snapshot = g_list_copy (live_toasts);
    for (GList *l = snapshot; l; l = l->next) {
        adw_toast_dismiss (ADW_TOAST (l->data));
    }
    g_list_free (snapshot);
}

/* surface "lost connection" with a Reconnect button. The
 * banner is built once with the toolbar; here we just set the title
 * text to the live server name and reveal it. set_status_bar() drives
 * the show/hide on the 1/2 -> 0 (and 2 -> 0) transitions; a fresh
 * login (status == 2) clears the banner via toolbar_hide_banner. */
void
toolbar_show_connection_lost (const char *server)
{
    char *title;

    if (!toolbar_banner) {
        return;
    }
    title = g_strdup_printf (_ ("Lost connection to %s"), server ? server : "");
    adw_banner_set_title (toolbar_banner, title);
    adw_banner_set_revealed (toolbar_banner, TRUE);
    g_free (title);
}

void
toolbar_hide_banner (void)
{
    if (toolbar_banner) {
        adw_banner_set_revealed (toolbar_banner, FALSE);
    }
}

/* register the hamburger-menu actions on the application.
 * Called from gtkhx_activate AFTER the AdwApplication is constructed
 * — fe_init() runs the toolbar construction earlier (before
 * g_application_run), so gtkhx_app is still NULL at that point and
 * we couldn't have registered the actions then. Splitting this out
 * lets the toolbar build with the actions referenced by name; the
 * actions get wired up in time for the menu's first interaction. */
void
toolbar_register_actions (GApplication *app, session *sess)
{
    GAction *act;

    g_return_if_fail (app != NULL);

    g_action_map_add_action_entries (G_ACTION_MAP (app), app_actions,
                                     G_N_ELEMENTS (app_actions), sess);

    /* Admin actions start disabled until login (gtkutil.c setbtns
	 * flips them on with the rest of the connection-gated UI). */
    act = g_action_map_lookup_action (G_ACTION_MAP (app), "user_new");
    if (G_IS_SIMPLE_ACTION (act)) {
        g_simple_action_set_enabled (G_SIMPLE_ACTION (act), FALSE);
    }
    act = g_action_map_lookup_action (G_ACTION_MAP (app), "user_edit");
    if (G_IS_SIMPLE_ACTION (act)) {
        g_simple_action_set_enabled (G_SIMPLE_ACTION (act), FALSE);
    }
}

/* build the GtkMenuButton + GMenuModel that hangs off the
 * end of the AdwHeaderBar. The connection-specific feature buttons
 * stay in the content row where the user clicks them often; the
 * menu collects the global / less-frequent actions:
 *
 *   Settings
 *   About GtkHx
 *   Admin >
 *     New User...
 *     Edit User...
 *   Quit
 *
 * Admin is a submenu (separate GMenu wrapped via append_submenu)
 * because New / Edit User are sysop-only — keeping them visible
 * but tucked away from the everyday user-flow. The GActions stay
 * disabled at startup; setbtns() flips them on at login (and only
 * for accounts whose privileges actually grant admin). */
static GtkWidget *
build_hamburger (void)
{
    GMenu *menu, *admin_menu;
    GtkWidget *btn;

    admin_menu = g_menu_new ();
    g_menu_append (admin_menu, _ ("New User…"), "app.user_new");
    g_menu_append (admin_menu, _ ("Edit User…"), "app.user_edit");

    menu = g_menu_new ();
    g_menu_append (menu, _ ("Settings"), "app.settings");
    g_menu_append (menu, _ ("Bookmarks…"), "app.bookmarks");
    g_menu_append (menu, _ ("About GtkHx"), "app.about");
    g_menu_append (menu, _ ("Reset Layout"), "app.reset_layout");
    g_menu_append_submenu (menu, _ ("Admin"), G_MENU_MODEL (admin_menu));
    g_menu_append (menu, _ ("Quit"), "app.quit");
    g_object_unref (admin_menu);

    btn = gtk_menu_button_new ();
    gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (btn), "open-menu-symbolic");
    gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (btn), G_MENU_MODEL (menu));
    gtk_widget_set_tooltip_text (btn, _ ("Main menu"));
    g_object_unref (menu);
    return btn;
}

/* Toolbar pixmap buttons belong to the GTKHX_SCALE_TOOLBAR theme
 * area. The historic 16x16 XPMs read as tiny pixel-art runes at modern
 * desktop sizes; the default theme renders this area at 200% (the old
 * hard-coded 2x), upscaled nearest-neighbor for the crisp blocky look,
 * and the user can retune it in Settings → Appearance. */
static GtkWidget *
make_pixmap_button (const char *resource_name, const char *tooltip,
                    GCallback cb, gpointer user_data)
{
    return gtkhx_pixmap_button (resource_name, tooltip, GTKHX_SCALE_TOOLBAR, cb,
                                user_data);
}

/* rebuild the AdwSplitButton's bookmark menu. Called from
 * connect.c after a successful Save Bookmark so newly-saved
 * entries appear in the dropdown without restarting the app. The
 * earlier lazy-rebuild-on-popover-show approach (a "show" signal
 * hook on the popover) had a chicken-and-egg problem: AdwSplitButton
 * creates its popover lazily, so on the first dropdown click the
 * popover didn't yet exist when we connected the signal, and the
 * menu wouldn't update. An explicit refresh call from the save path
 * is simpler and behaves predictably. */
void
toolbar_refresh_bookmarks (void)
{
    GMenu *menu;
    if (!connect_btn || !ADW_IS_SPLIT_BUTTON (connect_btn)) {
        return;
    }
    menu = connect_build_bookmark_menu ();
    adw_split_button_set_menu_model (ADW_SPLIT_BUTTON (connect_btn),
                                     G_MENU_MODEL (menu));
    g_object_unref (menu);
}

/* configure-event is gone in GTK 4 and the toolbar window
 * has no resizable size to save anyway. Position is captured at
 * hx_quit() in gtkhx.c gtkhx_save_window_positions. */

/* install the per-frame plumbing every leaf
 * PanelFrame in the dock needs. Three concerns:
 *
 *   - close-dispatcher: routes PanelFrame::page-closed to the
 *     dynamic panel's teardown (pchat / msg).
 *   - drag-out hook: detects drag-cancel on the libpanel drag
 *     handle and undocks the dragged panel.
 *   - defang drop-controls: makes PanelDropControls transparent
 *     so the dock-level drop target sees the drop.
 *
 * Called once per area at dock build time, and again whenever a
 * user splits a leaf — the new sibling leaf needs the same hooks
 * so the user can interact with it the same way as the originals. */
void
toolbar_install_panel_hooks_on_frame (GtkWidget *frame)
{
    g_return_if_fail (PANEL_IS_FRAME (frame));
    hx_panel_install_close_dispatcher (frame);
    hx_panel_install_drag_out_on_frame (frame);
    hx_panel_defang_drop_controls_on_frame (frame);
    hx_split_install_frame_ui (frame);
}

/* hx_split_foreach_leaf callback. Bridges to
 * toolbar_install_panel_hooks_on_frame for each leaf in the dock
 * tree. Used by create_toolbar_window so both the default-built
 * tree and a saved-layout-restored tree get the same per-frame
 * setup in one pass. */
static void
install_leaf_hooks_cb (HxSplit *leaf, gpointer user_data)
{
    PanelFrame *frame = hx_split_get_frame (leaf);
    (void)user_data;
    if (frame != NULL)
        toolbar_install_panel_hooks_on_frame (GTK_WIDGET (frame));
}

/* toolbar buttons "show
 * panel X". The button's `data' is the panel's registry id (a
 * static string). Behaviour:
 *
 *   - Look up the panel from the registry.
 *   - panel_widget_raise() — selects the panel's tab in its frame,
 *     so even if its area was already visible with a different tab
 *     active, the click brings THIS panel forward.
 *   - Reveal the area the panel lives in. Walking the panel up to
 *     its PanelFrame and that frame up to a PanelDockChild gives
 *     us the right area; flip its reveal-area on.
 *
 * Net: each toolbar button reliably brings up its specific panel
 * regardless of dock state, instead of just toggling visibility of
 * whatever happens to be in a given area. */
static void
toolbar_show_panel (GtkButton *button, gpointer data)
{
    const char *panel_id = data;
    HxPanel    *panel;

    (void)button;

    panel = hx_panel_registry_lookup (panel_id);
    if (panel == NULL || toolbar_dock == NULL)
        return;

    /* If the panel was closed (frame chevron "Close all pages",
     * per-tab close), it has no parent; the registry still owns a
     * strong ref so the widget is alive. Splice it back into its
     * home area before raising — without this the raise no-ops and
     * the user's click on the toolbar button silently does nothing. */
    hx_panel_ensure_attached (panel);

    panel_widget_raise (PANEL_WIDGET (panel));
}

/* DEFAULT_LEAF_MIN_WIDTH lives in toolbar.h so the saved-layout
 * loader uses the same value as MAKE_LEAF_FRAME below. */

/* notify::max-position handler — see the comment block where this
 * is connected in create_toolbar_window for the rationale.
 *
 * Halves the right child's share on first allocation, with a
 * floor at DEFAULT_LEAF_MIN_WIDTH (matches the size-request
 * MAKE_LEAF_FRAME installs on every leaf). The size-request is
 * what bounds user-dragging too — this handler only sets the
 * initial divider position. */
static void
on_right_paned_first_alloc (GObject    *object,
                            GParamSpec *pspec,
                            gpointer    user_data)
{
    GtkPaned *paned = GTK_PANED (object);
    int       max_position = 0;
    int       paned_width;
    int       pos;
    int       right_current;
    int       target;

    (void)pspec;
    (void)user_data;

    /* notify::max-position fires once with 0 before allocation
     * happens (the initial property value), and again with the
     * real width on first allocation. Skip the 0 notification. */
    g_object_get (object, "max-position", &max_position, NULL);
    if (max_position <= 0)
        return;

    paned_width = gtk_widget_get_width (GTK_WIDGET (paned));
    pos = gtk_paned_get_position (paned);

    /* With position-set=FALSE (the default before this handler
     * runs), GtkPaned reports the natural divider position once
     * allocated. right_current = paned_width - divider_position
     * (the handle width is negligible for the halving math). */
    right_current = paned_width - pos;
    if (right_current <= 0)
        goto out;

    target = right_current / 2;
    if (target < DEFAULT_LEAF_MIN_WIDTH)
        target = DEFAULT_LEAF_MIN_WIDTH;
    gtk_paned_set_position (paned, paned_width - target);

out:
    g_signal_handlers_disconnect_by_func (object,
                                          on_right_paned_first_alloc,
                                          user_data);
}

void
create_toolbar_window (session *sess)
{
    GtkWidget *header;
    GtkWidget *hbox;
    GtkWidget *toolbar_view;

    /* stay on plain GtkWindow rather than AdwApplicationWindow.
	 * fe_init() runs the toolbar construction BEFORE g_application_run
	 * (so gtkhx_app is still NULL here — confirmed by an earlier
	 * AdwApplicationWindow attempt that hit a NULL-app assertion at
	 * this point). The Phase 3.6 toplevel sweep in gtkhx_activate
	 * registers this window with GtkApplication later, and the
	 * AdwHeaderBar slotted in via gtk_window_set_titlebar gives us
	 * the same "no double title bar" appearance AdwApplicationWindow
	 * would have. The hamburger actions live on the application and
	 * get registered from gtkhx_activate via toolbar_register_actions.
	 *
	 * the toolbar window is now the
	 * dock host. The button row and banners stay where they always
	 * were (top of the content), and a PanelDock fills the rest of
	 * the window. The dock starts empty — Phase 2 migrates one
	 * window at a time into PanelToggleButton-driven HxPanels.
	 * Until then the legacy create_*_window paths still spawn the
	 * standalone top-levels they always have. */
    toolbar_window = gtk_window_new ();
    gtk_window_set_title (GTK_WINDOW (toolbar_window), "GtkHx");
    /* Restore the saved window size if there is one. The existing
     * gtkhx_save_window_positions writes width/height into
     * gtkhx_prefs.geo.tool on quit; this is the matching read-back
     * path that the original code never had — for years the
     * toolbar always came up at the hardcoded default. Fall back
     * to 1100x700 when no saved size exists yet (first launch, or
     * the user wiped prefs). */
    {
        int saved_w = gtkhx_prefs.geo.tool.xsize;
        int saved_h = gtkhx_prefs.geo.tool.ysize;
        /* Sanity floor only rejects zero / negative (first launch
         * before any save, or a corrupted prefs file). We can't
         * floor at a 'sensible' value here because the user has a
         * legitimate reason to want a small toolbar when they've
         * undocked everything — Misha demoed exactly this with
         * a 442x177 saved size that the previous >200 floor was
         * silently rejecting on restore. GTK clamps anything
         * below the window's actual minimum at allocate time. */
        if (saved_w > 0 && saved_h > 0)
            gtk_window_set_default_size (GTK_WINDOW (toolbar_window),
                                         saved_w, saved_h);
        else
            gtk_window_set_default_size (GTK_WINDOW (toolbar_window),
                                         1100, 700);
    }

    /* ------------- header bar (top) ------------- */
    header = adw_header_bar_new ();

    /* AdwSplitButton — primary click opens the connect
	 * dialog; the dropdown chevron exposes a menu of saved
	 * bookmarks targeting app.open_bookmark with the bookmark name
	 * as parameter. Refresh of the menu happens from the bookmark
	 * save path via toolbar_refresh_bookmarks(). */
    connect_btn = adw_split_button_new ();
    adw_split_button_set_icon_name (ADW_SPLIT_BUTTON (connect_btn),
                                    "network-transmit-receive-symbolic");
    gtk_widget_add_css_class (connect_btn, "suggested-action");
    gtk_widget_set_tooltip_text (connect_btn, _ ("Connect"));
    g_signal_connect (connect_btn, "clicked",
                      G_CALLBACK (create_connect_window), sess);
    {
        GMenu *menu = connect_build_bookmark_menu ();
        adw_split_button_set_menu_model (ADW_SPLIT_BUTTON (connect_btn),
                                         G_MENU_MODEL (menu));
        g_object_unref (menu);
    }
    adw_header_bar_pack_start (ADW_HEADER_BAR (header), connect_btn);

    disconnect_btn = gtk_button_new_from_icon_name ("network-offline-symbolic");
    gtk_widget_set_tooltip_text (disconnect_btn, _ ("Disconnect"));
    g_signal_connect (disconnect_btn, "clicked",
                      G_CALLBACK (disconnect_clicked), sess);
    adw_header_bar_pack_start (ADW_HEADER_BAR (header), disconnect_btn);

    adw_header_bar_pack_end (ADW_HEADER_BAR (header), build_hamburger ());

    /* ------------- content (feature button row) ------------- */
    hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_set_margin_start (hbox, 6);
    gtk_widget_set_margin_end (hbox, 6);
    gtk_widget_set_margin_top (hbox, 6);
    gtk_widget_set_margin_bottom (hbox, 6);
    /* AdwToolbarView's content slot fills vertically, which
	 * stretches a single row of icon buttons into uncomfortably tall
	 * rectangles. Pin the row to its natural height and center it
	 * vertically so the toolbar reads as a strip of buttons rather
	 * than a wall of them. */
    gtk_widget_set_valign (hbox, GTK_ALIGN_CENTER);
    gtk_widget_set_vexpand (hbox, FALSE);

    gtk_box_append (
        GTK_BOX (hbox),
        make_pixmap_button ("/com/nasledov/gtkhx/pixmaps/tracker.png",
                            _ ("Tracker"), G_CALLBACK (create_tracker_window),
                            sess));
    /* Files / Users / Tasks
     * buttons use the toolbar_show_panel helper so each one raises
     * its specific panel + reveals the area, rather than just
     * toggling area visibility. Chat and the two news entry
     * points keep their own factories which build-or-raise AND
     * kick off a server fetch when connected — that's the bit
     * the bare show-panel helper can't do. */
    news_btn = make_pixmap_button ("/com/nasledov/gtkhx/pixmaps/news.png",
                                   _ ("News"),
                                   G_CALLBACK (open_news), sess);
    gtk_box_append (GTK_BOX (hbox), news_btn);
    /* News (1.5+): same shape — the entry point raises the
     * existing panel via the registry AND triggers a NEWSDIRLIST
     * when connected, so the user sees fresh content on each open
     * instead of having to hit Refresh after the first build. */
    news15_btn = make_pixmap_button ("/com/nasledov/gtkhx/pixmaps/news_folder.png",
                                     _ ("News (1.5+)"),
                                     G_CALLBACK (open_news_browser), sess);
    gtk_box_append (GTK_BOX (hbox), news15_btn);
    files_btn = make_pixmap_button ("/com/nasledov/gtkhx/pixmaps/files.png",
                                    _ ("Files"),
                                    G_CALLBACK (toolbar_show_panel),
                                    (gpointer) HX_PANEL_ID_FILES);
    gtk_box_append (GTK_BOX (hbox), files_btn);
    /* Users defaults to
     * the END (right) area. Button raises + reveals. */
    gtk_box_append (GTK_BOX (hbox),
                    make_pixmap_button (
                        "/com/nasledov/gtkhx/pixmaps/users.png", _ ("Users"),
                        G_CALLBACK (toolbar_show_panel),
                        (gpointer) HX_PANEL_ID_USERS));
    /* Chat is a center-area panel
     * resident; raise + reveal via the shared helper. */
    gtk_box_append (GTK_BOX (hbox),
                    make_pixmap_button ("/com/nasledov/gtkhx/pixmaps/chat.png",
                                        _ ("Chat"),
                                        G_CALLBACK (toolbar_show_panel),
                                        (gpointer) HX_PANEL_ID_CHAT));
    /* Tasks defaults to
     * the BOTTOM area. Button raises + reveals. */
    gtk_box_append (GTK_BOX (hbox),
                    make_pixmap_button (
                        "/com/nasledov/gtkhx/pixmaps/tasks.png", _ ("Tasks"),
                        G_CALLBACK (toolbar_show_panel),
                        (gpointer) HX_PANEL_ID_TASKS));

    /* Broadcast — sends an admin-wide message via HTLC_HDR_MSG_BROADCAST.
	 * Icon comes from icons.rsrc cicn 220 (tools/cicndump). Always
	 * present in the toolbar; setbtns flips sensitivity based on
	 * connection state + HL_ACCESS_CAN_BROADCAST. Greyed-out beats
	 * hidden for feature discoverability — users notice the button
	 * exists, hover for the tooltip, and learn what it does even
	 * before they have permission to use it. */
    broadcast_btn = make_pixmap_button (
        "/com/nasledov/gtkhx/pixmaps/broadcast.png", _ ("Broadcast"),
        G_CALLBACK (on_broadcast_button_clicked), sess);
    gtk_widget_set_sensitive (broadcast_btn, FALSE);
    gtk_box_append (GTK_BOX (hbox), broadcast_btn);

#ifdef USE_PLUGIN
    plugin_btn = gtk_button_new_with_label ("[ P ]");
    gtk_widget_set_tooltip_text (plugin_btn, _ ("Plugin Manager"));
    g_signal_connect (plugin_btn, "clicked", G_CALLBACK (create_plugin_manager),
                      0);
    gtk_box_append (GTK_BOX (hbox), plugin_btn);
#endif

    /* ------------- bottom bar (status label) ------------- */
    status_bar = gtk_label_new (_ ("Not Connected"));
    gtk_widget_add_css_class (status_bar, "dim-label");
    gtk_widget_set_halign (status_bar, GTK_ALIGN_START);
    gtk_widget_set_margin_start (status_bar, 8);
    gtk_widget_set_margin_end (status_bar, 8);
    gtk_widget_set_margin_top (status_bar, 4);
    gtk_widget_set_margin_bottom (status_bar, 4);

    /* ------------- compose ------------- */
    /* gtk_window_set_titlebar installs the AdwHeaderBar AS
	 * the window's title bar (no GTK default chrome on top of it),
	 * which is what AdwApplicationWindow does implicitly. */
    gtk_window_set_titlebar (GTK_WINDOW (toolbar_window), header);

    /* AdwBanner sits above the content row for "lost
	 * connection" state with an actionable Reconnect button.
	 * Hidden by default; toolbar_show_connection_lost() reveals
	 * it from set_status_bar() on the 1/2 -> 0 transition. */
    toolbar_banner = ADW_BANNER (adw_banner_new (""));
    adw_banner_set_button_label (toolbar_banner, _ ("Reconnect"));
    adw_banner_set_revealed (toolbar_banner, FALSE);
    g_signal_connect (toolbar_banner, "button-clicked",
                      G_CALLBACK (on_banner_button_clicked), sess);

    /* the dock is ONE recursive HxSplit tree.
     * The previous PanelDock-with-four-areas structure is gone;
     * splits / moves / closes operate over a single uniform tree.
     * The default layout below mimics the visual placement of the
     * Phase 5a four-area arrangement so existing users see the
     * same dock on first launch.
     *
     * Default layout:
     *
     *   root  (horizontal split):
     *   ├── left leaf  — News                  (toolbar_sidebar_frame)
     *   └── rest       (horizontal split):
     *       ├── middle (vertical split):
     *       │   ├── center leaf — Chat, Files,
     *       │   │                 News 1.5      (toolbar_center_frame)
     *       │   └── bottom leaf — Tasks         (toolbar_bottom_frame)
     *       └── right leaf — Users              (toolbar_end_frame)
     *
     * toolbar_*_frame pointers reference the four initial leaves'
     * PanelFrames so static-panel factories' panel_frame_add
     * target keeps working unchanged. The pointers stay STABLE
     * across user splits — when the user splits the Chat/Files
     * leaf, that leaf's frame keeps Chat+Files and a NEW empty
     * frame appears alongside; toolbar_center_frame still points
     * at the original.
     *
     * No more area revealers, no PanelDock-level reveal toggling.
     * The user closes a frame to remove it; the empty-frame
     * stays-visible behaviour that PanelDock used to suppress via
     * notify::empty is the new normal. */
    {
        HxSplit *root = NULL;
        gboolean from_saved = dock_layout_load (&root,
                                                &toolbar_sidebar_frame,
                                                &toolbar_center_frame,
                                                &toolbar_bottom_frame,
                                                &toolbar_end_frame);

        if (!from_saved) {
            PanelFrame *f_left, *f_center, *f_bottom, *f_right;
            HxSplit    *leaf_left, *leaf_center, *leaf_bottom, *leaf_right;
            HxSplit    *middle, *cb_plus_right;

            #define MAKE_LEAF_FRAME(out, var)                                \
                do {                                                         \
                    (var) = hx_panel_frame_new ();                           \
                    panel_frame_set_header (                                 \
                        (var), PANEL_FRAME_HEADER (                          \
                                   panel_frame_header_bar_new ()));          \
                    (out) = GTK_WIDGET (var);                                \
                    gtk_widget_set_size_request ((out),                      \
                                                 DEFAULT_LEAF_MIN_WIDTH,     \
                                                 -1);                        \
                } while (0)

            MAKE_LEAF_FRAME (toolbar_sidebar_frame, f_left);
            MAKE_LEAF_FRAME (toolbar_center_frame,  f_center);
            MAKE_LEAF_FRAME (toolbar_bottom_frame,  f_bottom);
            MAKE_LEAF_FRAME (toolbar_end_frame,     f_right);
            #undef MAKE_LEAF_FRAME

            leaf_left   = hx_split_new_with_frame (f_left);
            leaf_center = hx_split_new_with_frame (f_center);
            leaf_bottom = hx_split_new_with_frame (f_bottom);
            leaf_right  = hx_split_new_with_frame (f_right);

            middle = hx_split_new_internal (leaf_center, leaf_bottom,
                                            GTK_ORIENTATION_VERTICAL);
            cb_plus_right = hx_split_new_internal (middle, leaf_right,
                                                   GTK_ORIENTATION_HORIZONTAL);
            root = hx_split_new_internal (leaf_left, cb_plus_right,
                                          GTK_ORIENTATION_HORIZONTAL);

            /* The Users panel's natural width makes the right leaf
             * start out wider than it really needs to be — halve
             * its share on first allocation via a notify::max-
             * position one-shot. Only attached for the default
             * layout; saved layouts come back with paned positions
             * from dock_layout_apply_geometry instead.
             *
             * shrink_end_child stays FALSE (hx_split default). GTK
             * paned source: max_position = shrink ? allocation :
             * allocation - end_child_req, where end_child_req is
             * the end child's MIN (respects size-request), not its
             * natural. So shrink=FALSE still lets the user halve
             * below natural — it caps at the 300 px floor. */
            g_signal_connect (hx_split_get_paned (cb_plus_right),
                              "notify::max-position",
                              G_CALLBACK (on_right_paned_first_alloc),
                              NULL);
        }

        /* Both paths converge here: every leaf needs the per-frame
         * hooks (close-dispatcher, drag-out, drop-controls defang,
         * Split/Close menu button). One foreach pass over the tree
         * covers both default-construction and saved-layout-restore. */
        hx_split_foreach_leaf (root, install_leaf_hooks_cb, NULL);

        gtk_widget_set_hexpand (GTK_WIDGET (root), TRUE);
        gtk_widget_set_vexpand (GTK_WIDGET (root), TRUE);

        dock_layout_set_dock_root (root);

        /* PanelDock as a thin wrapper. The HxSplit root is the
         * dock's single center child; no start/end/top/bottom
         * children, no revealers active. The wrapper exists for
         * one reason: libpanel's PanelFrame template includes
         * PanelDropControls overlays that assert a PANEL_TYPE_DOCK
         * ancestor at root time (panel-drop-controls.c
         * panel_drop_controls_root). Without it, every frame
         * emits a 'PanelDropControls added without a dock'
         * warning even though we don't use libpanel's DnD. From
         * the user's perspective the dock is still one
         * recursive HxSplit tree — the wrapper just satisfies
         * libpanel's invariants. */
        toolbar_dock = panel_dock_new ();
        gtk_widget_set_hexpand (toolbar_dock, TRUE);
        gtk_widget_set_vexpand (toolbar_dock, TRUE);
        {
            GtkBuilder        *b     = gtk_builder_new ();
            GtkBuildable      *bdock = GTK_BUILDABLE (toolbar_dock);
            GtkBuildableIface *iface = GTK_BUILDABLE_GET_IFACE (bdock);
            iface->add_child (bdock, b, G_OBJECT (root), NULL); /* center */
            g_object_unref (b);
        }

        /* Dock-level drop target. The hit-test for the deepest
         * descendant PanelFrame walks through HxSplit nodes
         * transparently. */
        hx_panel_install_drop_target_on_dock (toolbar_dock);
    }

    /* If we restored the tree from disk, push the saved paned
     * positions onto the now-mounted paneds. Has to happen after
     * the dock is added to the dock widget — pre-mount, the
     * paneds' max-position is 0 and set_position would clamp to
     * 0. No-op for the default-built tree. */
    dock_layout_apply_geometry (GTK_WINDOW (toolbar_window));

    /* AdwToolbarView: the canonical libadwaita way to stack
	 * top/bottom chrome around a content widget. Top bars get the
	 * AdwBanner (reconnect), and a horizontal row holding the
	 * button strip + the server banner. Bottom bar gets the
	 * status label. The dock fills the rest.
	 *
	 * server banner row
	 * (banner.c, hidden until an HTLS_HDR_BANNER message arrives)
	 * sits to the RIGHT of the button row rather than below it.
	 * That kept the buttons compactly clustered on the left and
	 * gives the banner the rest of the horizontal real estate. */
    toolbar_view = adw_toolbar_view_new ();
    adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view),
                                  GTK_WIDGET (toolbar_banner));
    {
        GtkWidget *toprow = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *banner_row = banner_widget_new ();
        gtk_widget_set_hexpand (banner_row, TRUE);
        gtk_widget_set_valign  (banner_row, GTK_ALIGN_CENTER);
        gtk_box_append (GTK_BOX (toprow), hbox);
        gtk_box_append (GTK_BOX (toprow), banner_row);
        adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view),
                                      toprow);
    }
    adw_toolbar_view_set_content  (ADW_TOOLBAR_VIEW (toolbar_view),
                                   toolbar_dock);
    adw_toolbar_view_add_bottom_bar (ADW_TOOLBAR_VIEW (toolbar_view),
                                     status_bar);

    /* AdwToastOverlay wraps the content so toolbar_show_toast()
	 * can push transient notifications over the button row. Toasts
	 * surface as a sliding banner at the bottom of the overlay; the
	 * persistent status label stays visible underneath for ambient
	 * connection state. */
    toolbar_toast = ADW_TOAST_OVERLAY (adw_toast_overlay_new ());
    adw_toast_overlay_set_child (toolbar_toast, toolbar_view);
    gtk_window_set_child (GTK_WINDOW (toolbar_window),
                          GTK_WIDGET (toolbar_toast));

    /* Initial sensitivity: pre-connection, only Connect + the global
	 * menu items are usable. setbtns() flips the rest on at login,
	 * including the Admin submenu's app.user_new / app.user_edit
	 * GActions.
	 *
	 * files_btn and news15_btn stay enabled regardless of
	 * connection state — their click just brings the (always
	 * resident) Files / News 1.5 panel forward in the dock; even
	 * disconnected the panel shows whatever it has cached. */
    gtk_widget_set_sensitive (disconnect_btn, FALSE);

    /* Close-request → close_toolbar_window, which calls hx_quit() so
	 * the prefs_write + position-save pass runs before the
	 * GtkApplication unwinds the last window. */
    g_signal_connect (toolbar_window, "close-request",
                      G_CALLBACK (close_toolbar_window), 0);

    /* Persist toolbar size on every user resize. The existing
     * gtkhx_save_window_positions path captures the size at quit
     * time, but if the user undocks everything and resizes the
     * toolbar but doesn't quit through the confirmation dialog
     * (or quits via a path that skips hx_quit), no capture
     * happens and the size reverts on next launch. Mirroring the
     * undocked-window approach: connect notify::default-width
     * and -height; the handler debounces a save_geo + prefs_write
     * on a 500 ms idle. */
    g_signal_connect (toolbar_window, "notify::default-width",
                      G_CALLBACK (on_toolbar_size_notify), NULL);
    g_signal_connect (toolbar_window, "notify::default-height",
                      G_CALLBACK (on_toolbar_size_notify), NULL);

    /* gtk_window_move removed (Wayland) */

    gtk_window_present (GTK_WINDOW (toolbar_window));
    init_keyaccel (toolbar_window);

    if (connected) {
        gtk_widget_set_sensitive (disconnect_btn, TRUE);
        changetitlespecific (toolbar_window, "GtkHx");
    }
    sess->toolbar_window = toolbar_window;

#ifdef HAVE_VOICE
    /* Phase 8.E follow-up: install the push-to-talk key controller.
     * Window-scoped (not chat-input-scoped) so PTT works while
     * focus is anywhere in the app — users list, news, files
     * browser. CAPTURE phase means the bound key is consumed
     * before reaching the chat input; the keyspec vocabulary
     * (function keys, modifier+ combos, Pause/Insert/etc.)
     * guarantees no plain typing key can land in this binding.
     * Idempotent — safe to call again on reconnect-after-disconnect
     * if that ever wires through here. */
    hx_voice_ptt_attach (toolbar_window, sess);
#endif

    /* eager-construct the sidebar
     * panels (Users + Tasks) so the sidebar has real residents
     * from the start. Revealing an empty side area would
     * auto-collapse immediately (panel_dock_notify_empty_cb).
     * News goes in the center PanelGrid; the grid lazily
     * creates a PanelFrame for it (and subsequent center
     * panels share that frame's tab strip). Subsequent fe_init
     * auto-open calls hit the registry. */
    create_users_window (toolbar_window, sess);
    create_tasks_window (toolbar_window, sess);
    create_news_window  (toolbar_window, sess);
    create_chat_window  (toolbar_window, sess);
    /* open_files_browser doesn't take a (parent, sess) signature;
     * it reads the_session directly. Eager-construct so the Files
     * toolbar button's toolbar_show_panel lookup always hits. */
    open_files_browser  ();
    /* Same shape as open_files_browser: no (parent, sess) — the
     * news (1.5+) entry point ignores its (widget, sess) args
     * since the browser is a singleton, but eager-construct so
     * the toolbar button's toolbar_show_panel lookup hits the
     * registry on first click. */
    open_news_browser   (NULL, sess);
}
