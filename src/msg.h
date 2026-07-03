#ifndef HX_MSG_H
#define HX_MSG_H

/* lazy-allocate the session's PM-window GHashTable. Safe to
 * call multiple times — only the first call constructs the table.
 * gtkhx.c calls it before the first create_msgwin at startup. */
extern void msg_windows_init (session *sess);

/* create_msgwin is the gtkhx-ui `msg` Rust shell: it builds the PM tab layout
 * around the model + leaf widgets create_msg makes, then adds the tab. The
 * accessors/setters below are the seam it uses (create_msg is C, the layout
 * assembly is Rust). */
extern struct msgwin *create_msgwin (guint16 uid, char *name);
extern struct msgwin *create_msg (guint16 uid, char *name);
extern GtkWidget *hx_msgwin_outputbuf (struct msgwin *msg);
extern GtkWidget *hx_msgwin_vscroll (struct msgwin *msg);
extern GtkWidget *hx_msgwin_inputbuf (struct msgwin *msg);
extern void hx_msgwin_set_window (struct msgwin *msg, GtkWidget *w);
extern void hx_msgwin_set_info_image (struct msgwin *msg, GtkWidget *w);
extern void hx_msgwin_set_info_label (struct msgwin *msg, GtkWidget *w);
extern struct msgwin *msgwin_with_uid (guint16 uid);
extern void msg_output (char *name, guint16 uid, char *buf);

/* msg-signal renderer. Same as msg_output but reads from
 * a pre-parsed HxMsgEvent (uid + UTF-8-validated name/body +
 * is_self flag from hx_msg_event_new). */
struct _HxMsgEvent;
extern void msg_output_from_event (struct _HxMsgEvent *event);

/* Render a received HTLS_HDR_MSG_BROADCAST. sender_name + sender_color
 * are from the wire chunk (NULL/0 when the server didn't include
 * them — older Hotline servers and anonymous "rate-limit" notes).
 * When sender_name is non-NULL the chat log line uses "[name] body"
 * with sender_color picking the name's mIRC slot; otherwise it
 * falls back to the legacy "[hx] broadcast: ..." form. */
extern void broadcastmsg (const char *sender_name, guint16 sender_color,
                          char *text);
/* Re-render the recipient info pane from the cached hx_user. Used
 * at create_msgwin time when the cached struct already reflects
 * current state. */
extern void msgwin_refresh_user_info (struct msgwin *msg);

/* Re-render the recipient info pane from explicit just-changed
 * values. Called from users.c's user_change handler, which receives
 * the NEW name/icon/color as direct args before rcv.c has patched
 * them onto the cached hx_user struct. */
extern void msgwin_apply_user_change (struct msgwin *msg, const char *nam,
                                      guint16 icon, guint16 color);

extern void hx_send_msg (struct htlc_conn *htlc, guint16 uid, const char *msg,
                         guint16 len, void *p);

/* Send a server-wide broadcast (HTLC_HDR_MSG_BROADCAST). Requires
 * HL_ACCESS_CAN_BROADCAST on the account; the toolbar button is
 * hidden otherwise but the server still enforces it. */
extern void hx_send_broadcast (struct htlc_conn *htlc, const char *msg,
                               guint16 len);

#endif
