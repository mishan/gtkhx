#ifndef HX_USERS_H
#define HX_USERS_H

#include "session.h"

extern GtkWidget *msgbtn;
extern GtkWidget *kickbtn;
extern GtkWidget *infobtn;
extern GtkWidget *banbtn;
extern GtkWidget *chatbtn;
extern GtkWidget *ignobtn;

extern GdkRGBA user_colors[8];
extern GdkRGBA gdk_user_colors[4];

extern PangoFontDescription *users_font_desc;


extern void create_users_window (GtkWidget *widget, gpointer data);
extern void user_list (session *sess);
extern void user_create (struct htlc_conn *htlc, struct chat *chat, struct hx_user *user, const char *nam, guint16 icon, guint16 color);
extern void user_delete (struct htlc_conn *htlc, struct chat *chat, struct hx_user *user);
extern void user_change (struct htlc_conn *htlc, struct chat *chat, struct hx_user *user, const char *nam, guint16 icon, guint16 color);
extern void users_clear (struct htlc_conn *htlc, struct chat *chat);
/* Phase 4.5/4.7: user_clicked was the GdkEventButton-shaped GtkCList
 * "button_press_event" handler. GTK 4 widgets don't emit
 * button_press_event — clicks come via a GtkGestureClick controller.
 * users_attach_click_gesture() installs a controller on `list' that
 * dispatches double-clicks to the message window and right-clicks to
 * the user popup, pulling the session from `sess'. */
extern void users_attach_click_gesture (GtkWidget *list, session *sess);
extern void open_message_btn (GtkWidget *widget, gpointer data);
extern void user_info_btn (GtkWidget *widget, gpointer data);
extern void user_kick_btn (GtkWidget *widget, gpointer data);
extern void user_igno_btn (GtkWidget *widget, gpointer data);
extern void user_ban_btn (GtkWidget *widget, gpointer data);
extern void user_chat_btn(GtkWidget *widget, gpointer data);



extern struct hx_user *hx_user_new (struct hx_user **utailp);
extern void hx_user_delete (struct hx_user **utailp, struct hx_user *user);
extern struct hx_user *hx_user_with_uid (struct hx_user *ulist, guint16 uid);
extern struct hx_user *hx_user_with_name (struct hx_user *ulist, char *name);
extern void hx_change_name_icon (struct htlc_conn *htlc);
extern void hx_get_user_info (struct htlc_conn *htlc, guint16 uid);
extern void hx_kick_user (struct htlc_conn *htlc, guint16 uid, guint16 ban);

#endif
