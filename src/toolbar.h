#ifndef HX_TOOL_H
#define HX_TOOL_H

extern GtkWidget *toolbar_window;
extern GtkWidget *files_btn;
extern GtkWidget *connect_btn;
extern GtkWidget *post_btn;
extern GtkWidget *disconnect_btn;
extern GtkWidget *usermod_btn;
extern GtkWidget *usernew_btn;
extern GtkWidget *news15_btn;

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

#endif
