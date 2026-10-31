#ifndef HX_MSG_H
#define HX_MSG_H

/* Private messages: gtkhx-ui's `msg` module, over hxmodel::pm. */

/* Show a private message received on htlc, opening its conversation. */
struct _HxMsgEvent;
extern void msg_output_from_event (struct htlc_conn *htlc,
                                   struct _HxMsgEvent *event);

/* Open the conversation with uid, or select it if it is open. */
extern void hx_msg_open (session *sess, guint16 uid, const char *name);

/* Redraw the open conversation with uid, if any, for the user's new name,
 * icon and status. Passed in because the user list hasn't taken them yet. */
extern void hx_msg_user_changed (session *sess, guint16 uid, const char *nam,
                                 guint16 icon, guint16 color);

/* Restyle each of sess's conversations: the palette, the chat font, and the
 * chat preferences. */
extern void hx_msg_set_palette (session *sess, const GdkRGBA *palette);
extern void hx_msg_set_font (session *sess, const char *font);
extern void hx_msg_configure (session *sess);

/* Render a broadcast received on htlc (gtkhx-ui's `broadcast` module).
 * sender_name + sender_color are from the wire (NULL/0 when the server
 * didn't include them — older Hotline servers and anonymous "rate-limit"
 * notes). With a name, the chat log line is "[name] body" with
 * sender_color picking the name's slot; otherwise "[hx] broadcast: ...". */
extern void broadcastmsg (struct htlc_conn *htlc, const char *sender_name,
                          guint16 sender_color, char *text);

/* SEND_MSG, whose reply the session reads (hxhandlers::send::msg). */
extern void hx_send_msg (struct htlc_conn *htlc, guint16 uid, const char *msg);

#endif
