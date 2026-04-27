#ifndef HX_CHAT_H
#define HX_CHAT_H

extern GdkColor colors[];

extern struct chat *chat_new (session *sess, guint32 cid);
extern void chat_delete (session *sess, struct chat *chat);
extern struct chat *chat_with_cid (session *sess, guint32 cid);
extern struct gtkhx_chat *gchat_with_cid (session *sess, guint32 cid);
extern void gchat_delete (session *sess, struct gtkhx_chat *gchat);
extern void xprintline(GtkWidget *text, char *chat, size_t len);
extern void hx_printf_prefix (struct htlc_conn *htlc, guint32 cid, const char *prefix, const char *fmt, ...);
extern void hx_printf (struct htlc_conn *htlc, guint32 cid, const char *fmt, ...);
extern void generate_colors (GtkWidget *widget);
extern void create_chat (session *sess);
extern void create_chat_window (GtkWidget *widget, gpointer data);
extern struct gtkhx_chat *pchat_new (session *sess, struct chat *chat);
extern void output_chat_subject (struct htlc_conn *htlc, guint32 cid, char *buf);
extern void output_chat_invitation (struct htlc_conn *htlc, guint32 cid, char *name);
extern struct gtkhx_chat *create_pchat_window (struct htlc_conn *htlc, struct chat *chat);
extern void hx_clear_chat (struct htlc_conn *htlc, guint32 cid, int subj);
extern int word_check (GtkWidget* xtext, char *word);

extern void hx_chat_user (struct htlc_conn *htlc, guint16 uid);
extern void hx_invite_user(struct htlc_conn *htlc, guint16 uid, guint32 cid);
extern void hx_chat_join (struct htlc_conn *htlc, guint32 cid);
extern void hx_part_chat(struct htlc_conn *htlc, guint32 cid);
extern void hx_change_subject(struct htlc_conn *htlc, guint32 cid, char *subject);
extern void hx_send_chat (struct htlc_conn *htlc, char *str, guint32 cid, guint16 style);

#endif
