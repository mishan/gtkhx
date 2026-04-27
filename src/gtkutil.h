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

#endif
