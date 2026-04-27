#ifndef HX_TOOL_H
#define HX_TOOL_H

extern GtkWidget *toolbar_window;
extern GtkWidget *files_btn;
extern GtkWidget *connect_btn;
extern GtkWidget *post_btn;
extern GtkWidget *quit_btn;
extern GtkWidget *disconnect_btn;
extern GtkWidget *usermod_btn;
extern GtkWidget *usernew_btn;
extern GtkWidget *news15_btn;

extern GtkWidget *status_bar;
extern guint status_msg;
extern guint context_status;

extern void quit_btn_func(GtkWidget *widget, gpointer data);
extern void create_toolbar_window (session *sess);
extern void disconnect_clicked (void);

#endif
