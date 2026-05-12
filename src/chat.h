#ifndef HX_CHAT_H
#define HX_CHAT_H

extern GdkRGBA colors[];

/* Phase 5+: lazy-allocate the session's chat GHashTable AND seed it
 * with the always-required public chat at cid=0. Safe to call
 * multiple times — only the first call constructs the table.
 * gtkhx.c calls it during init, before any other chat_* operation. */
extern void chats_init (session *sess);

/* Phase 5+: lazy-allocate the session's gtkhx_chat (UI side)
 * GHashTable. create_chat seeds the public chat (cid=0) UI;
 * pchat_new inserts pchat windows. */
extern void gchats_init (session *sess);

extern struct chat *chat_new (session *sess, guint32 cid);
extern void chat_delete (session *sess, struct chat *chat);
extern struct chat *chat_with_cid (session *sess, guint32 cid);
extern struct gtkhx_chat *gchat_with_cid (session *sess, guint32 cid);
extern void gchat_delete (session *sess, struct gtkhx_chat *gchat);
extern void xprintline(GtkWidget *text, char *chat, size_t len);

/* Phase 5+: chat-signal renderer. Takes a pre-parsed HxChatEvent
 * (sender/body slices + is_info/is_self flags from
 * hx_chat_event_new). Bypasses the legacy hx_printf round-trip
 * the log-line path still uses. */
struct _HxChatEvent;
extern void output_chat_from_event (struct htlc_conn *htlc,
                                     struct _HxChatEvent *event);
/* Phase 3 follow-up: hx_printf / hx_printf_prefix moved to
 * gtkhx_log.{c,h}; #include "gtkhx_log.h" rather than chat.h to
 * pull the decls in (chat.h forwards the include for source
 * compat with the existing call sites that include chat.h). */
#include "gtkhx_log.h"

/* The view-side handler for the "chat-log-line" signal. Connected
 * once in gtkhx_connect_signals at startup; the implementation
 * (xoutput_chat fan-out) lives in chat.c. */
struct _GtkhxSession; typedef struct _GtkhxSession GtkhxSession;
extern void chat_log_line_handler (GtkhxSession *emitter,
                                   struct htlc_conn *htlc, guint cid,
                                   gpointer body, gpointer user_data);
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
