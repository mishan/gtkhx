#ifndef HX_TOOL_H
#define HX_TOOL_H

extern GtkWidget *toolbar_window;
extern GtkWidget *files_btn;
extern GtkWidget *connect_btn;
extern GtkWidget *post_btn;
extern GtkWidget *disconnect_btn;
extern GtkWidget *news15_btn;
extern GtkWidget *news_btn;

/* Phase 5: status_bar is a GtkLabel now (was GtkStatusbar — deprecated
 * in GTK 4.10 and we never used its message-stack model). The
 * status_msg / context_status globals are gone with it; set_status_bar()
 * just calls gtk_label_set_text() on this. */
extern GtkWidget *status_bar;

extern void create_toolbar_window (session *sess);
extern void disconnect_clicked (void);

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
