#ifndef HX_GTKUTIL_H
#define HX_GTKUTIL_H

extern void init_keyaccel(GtkWidget *widget);
extern void set_disconnect_btn(session *sess, int stat);
extern void setbtns(session *sess, int stat);
extern void set_status_bar(int status);
extern void changetitlesconnected(session *sess);
extern void changetitlespecific();
extern void changetitlesdisconnected(session *sess);
extern void close_connected_windows(session *sess);
extern void error_dialog(char *title, char *msg);

/*
 * gtk_dialog_get_action_area was deprecated in GTK 3.12 with no
 * direct replacement (gtk_dialog_add_button covers the common case
 * but we have many sites that pack pre-existing buttons with custom
 * click handlers). The action area itself is still functional and
 * remains the right place to pack response widgets — the wholesale
 * rewrite is Phase 4 work. This wrapper hides the deprecation in one
 * place so the per-call-site noise goes away.
 */
extern GtkWidget *gtkhx_dialog_action_area (GtkDialog *dialog);

#endif
