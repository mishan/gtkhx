#ifndef HX_TOOL_H
#define HX_TOOL_H

extern GtkWidget *toolbar_window;
extern GtkWidget *files_btn;
extern GtkWidget *connect_btn;
extern GtkWidget *disconnect_btn;
extern GtkWidget *news15_btn;
extern GtkWidget *news_btn;
extern GtkWidget *broadcast_btn;

/* Phase 5: status_bar is a GtkLabel now (was GtkStatusbar — deprecated
 * in GTK 4.10 and we never used its message-stack model). The
 * status_msg / context_status globals are gone with it; set_status_bar()
 * just calls gtk_label_set_text() on this. */
extern GtkWidget *status_bar;

extern void create_toolbar_window (session *sess);
extern void disconnect_clicked (void);

/* Phase 5 / docking (Phase 1): handles to the toolbar window's
 * embedded PanelDock and its sidebar PanelFrame. Per-window panel
 * factories (users_panel.c, tasks_panel.c, …) use these to insert
 * themselves into the right area when the panel is first
 * registered. NULL before create_toolbar_window has run.
 *
 * toolbar_dock          — the dock; pass to panel_dock_set_reveal_*
 *                         when adding the first sidebar resident.
 * toolbar_sidebar_frame — the start-area PanelFrame; pass to
 *                         panel_frame_add for SIDEBAR-kind panels.
 * toolbar_center_grid   — the center PanelGrid; pass to
 *                         panel_grid_add for CENTER-kind panels.
 *
 * Phase 2 panel factories call:
 *   panel_frame_add (PANEL_FRAME (toolbar_sidebar_frame), panel);
 *   panel_dock_set_reveal_start (PANEL_DOCK (toolbar_dock), TRUE);
 * (revealer doesn't auto-open on the first add — see Phase 0
 * finding #5 in docs/docking-phase0-findings.md). */
extern GtkWidget *toolbar_dock;
extern GtkWidget *toolbar_sidebar_frame;  /* start-area frame  (initially empty) */
extern GtkWidget *toolbar_end_frame;      /* end-area frame    (Users default)   */
extern GtkWidget *toolbar_bottom_frame;   /* bottom-area frame (Tasks default)   */
extern GtkWidget *toolbar_center_grid;    /* center grid       (Chat/News/Files) */

/* Phase 5: register the hamburger-menu's GActions on the application.
 * Call from gtkhx_activate after the AdwApplication is constructed —
 * fe_init() runs create_toolbar_window earlier (before
 * g_application_run), so the actions can't be added at toolbar
 * construction time. Idempotent: GActionMap silently overwrites a
 * second registration with a g_critical (we want to see that, so
 * caller should only call this once). */
extern void toolbar_register_actions (GApplication *app, session *sess);

/* Phase 5: push a transient AdwToast onto the toolbar window's
 * AdwToastOverlay. Safe to call before the toolbar is built (no-op).
 * The toast auto-dismisses after libadwaita's default timeout. */
extern void toolbar_show_toast (const char *text);

/* Dismiss every toast we've pushed onto the toolbar's overlay that
 * hasn't auto-timed-out yet. Wired into the connection-state hook so
 * starting a new connect (Connect button, bookmark, Reconnect)
 * clears toasts from the previous server — task errors, broadcasts,
 * "Logged in" — instead of letting them hang over a new session.
 * Safe to call before the toolbar is built (no-op). */
extern void toolbar_clear_toasts (void);

/* Phase 5: reveal / hide the toolbar's AdwBanner. show_connection_lost
 * sets the banner text to "Lost connection to <server>" and reveals
 * the banner with a Reconnect button; hide_banner just sets revealed
 * to FALSE. Safe to call before the toolbar is built (no-op). */
extern void toolbar_show_connection_lost (const char *server);
extern void toolbar_hide_banner (void);

/* Phase 5: rescan the bookmarks directory and refresh the Connect
 * SplitButton's dropdown menu. Called from connect.c after a
 * successful Save Bookmark so newly-added entries show up without
 * restarting the app. */
extern void toolbar_refresh_bookmarks (void);

#endif
