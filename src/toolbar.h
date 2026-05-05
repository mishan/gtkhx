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

#endif
