#ifndef HX_TOOL_H
#define HX_TOOL_H

extern GtkWidget *toolbar_window;
extern GtkWidget *files_btn;
extern GtkWidget *connect_btn;
extern GtkWidget *disconnect_btn;
extern GtkWidget *news15_btn;
extern GtkWidget *news_btn;
extern GtkWidget *broadcast_btn;

/* Set the connection-status line — the header bar's subtitle, under the
 * window title. set_status_bar() in gtkutil.c is the caller. Safe to call
 * before the toolbar is built (no-op). */
extern void toolbar_set_status (const char *text);

extern void create_toolbar_window (session *sess);
extern void disconnect_clicked (void);

/* gtkhx-ui: the main window's notice banners. Transfer none. */
extern GtkWidget *gtkhx_main_banners (void);

/* register the hamburger-menu's GActions on the application.
 * Call from gtkhx_activate after the AdwApplication is constructed —
 * fe_init() runs create_toolbar_window earlier (before
 * g_application_run), so the actions can't be added at toolbar
 * construction time. Idempotent: GActionMap silently overwrites a
 * second registration with a g_critical (we want to see that, so
 * caller should only call this once). */
extern void toolbar_register_actions (GApplication *app, session *sess);

/* Build a static dock panel by its registry id. Returns TRUE when
 * the panel exists afterwards.
 *
 * `respect_saved_state` is the whole difference between the callers.
 * TRUE is the startup path: a panel the saved dock layout recorded
 * as closed is deliberately left unbuilt, which is what "closed
 * panels stay closed across a restart" means. FALSE is the user path
 * — a toolbar-button click is an explicit request and outranks
 * whatever was on disk.
 *
 * The saved state only governs whether a panel is built in the first
 * place. An existing panel's factory runs regardless, because the
 * factories are keyed by (panel, session): a second connection needs
 * its own content page inside a panel the first one already built.
 *
 * `sess` is the session whose content the panel renders. NULL is
 * tolerated and builds nothing. */
extern gboolean toolbar_build_panel (const char *id, session *sess,
                                     gboolean respect_saved_state);

/* toolbar_build_panel, then bring the panel forward -- out of the
 * drawer if it was closed, only when the user asked. What a toolbar
 * button does, and what the startup auto-open does. */
extern void toolbar_present_panel (const char *id, session *sess,
                                   gboolean respect_saved_state);

/* The narrowest a dock panel may be made, in pixels. 300 px covers the
 * widest of the default panels' button rows (Users: 6 icon-buttons +
 * spacing + margins ≈ 280 px) with a small margin. */
#define DEFAULT_LEAF_MIN_WIDTH 300

/* push a transient AdwToast onto the toolbar window's
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

/* reveal / hide the toolbar's AdwBanner. show_connection_lost
 * sets the banner text to "Lost connection to <server>" and reveals
 * the banner with a Reconnect button; hide_banner just sets revealed
 * to FALSE. Safe to call before the toolbar is built (no-op). */
extern void toolbar_show_connection_lost (const char *server);
extern void toolbar_hide_banner (void);

/* rescan the bookmarks directory and refresh the Connect
 * SplitButton's dropdown menu. Called from connect.c after a
 * successful Save Bookmark so newly-added entries show up without
 * restarting the app. */
extern void toolbar_refresh_bookmarks (void);

#endif
