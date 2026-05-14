#ifndef HX_MSG_H
#define HX_MSG_H

/* Phase 5+: lazy-allocate the session's PM-window GHashTable. Safe to
 * call multiple times — only the first call constructs the table.
 * gtkhx.c calls it before the first create_msgwin at startup. */
extern void msg_windows_init (session *sess);

extern struct msgwin *create_msgwin (guint16 uid, char *name);
extern struct msgwin *msgwin_with_uid (guint16 uid);
extern struct msgwin *create_msgwin (guint16 uid, char *name);
extern void msg_output (char *name, guint16 uid, char *buf);

/* Phase 5+: msg-signal renderer. Same as msg_output but reads from
 * a pre-parsed HxMsgEvent (uid + UTF-8-validated name/body +
 * is_self flag from hx_msg_event_new). */
struct _HxMsgEvent;
extern void msg_output_from_event (struct _HxMsgEvent *event);

extern void broadcastmsg (char *text);
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

#endif
