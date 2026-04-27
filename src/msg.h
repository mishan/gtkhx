#ifndef HX_MSG_H
#define HX_MSG_H

extern struct msgwin *msg_list;

extern struct msgwin *create_msgwin(guint16 uid, char *name);
extern struct msgwin *msgwin_with_uid(guint16 uid);
extern struct msgwin *create_msgwin (guint16 uid, char *name);
extern void msg_output (char *name, guint16 uid, char *buf);
extern void broadcastmsg(char *text);

extern void hx_send_msg (struct htlc_conn *htlc, guint16 uid,
			 const char *msg, guint16 len, void *p);

#endif
