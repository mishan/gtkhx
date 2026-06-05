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
extern void xprintline (GtkWidget *text, char *chat, size_t len);

/* Phase 5+: chat-signal renderer. Takes a pre-parsed HxChatEvent
 * (sender/body slices + is_info/is_self flags from
 * hx_chat_event_new). Bypasses the legacy hx_printf round-trip
 * the log-line path still uses. */
struct _HxChatEvent;
extern void output_chat_from_event (struct htlc_conn *htlc,
                                    struct _HxChatEvent *event);

/* Chat-history extension (fogWraith Capabilities-Chat-History.md).
 * Render a batch of historical entries received from the server
 * into chat `cid`'s output buffer. Each entry is rendered in a
 * muted (mIRC 14 grey) colour to visually distinguish from live
 * chat. The first batch since reconnect prepends a
 * "─── chat history ───" divider; subsequent batches (from
 * "Load older" — Phase 3) prepend a thinner separator.
 *
 * `entries` is a GPtrArray<HxHistoryEntry*> borrowed for the
 * duration of the call. has_more is informational — Phase 2
 * doesn't render anything different based on it, but Phase 3
 * uses it to decide whether to show the "Load older" row. */
extern void output_chat_history_batch (struct htlc_conn *htlc, guint32 cid,
                                       GPtrArray *entries, gboolean has_more);

/* Phase 3.2: clickable "Load older messages" sentinel rendered when
 * the server says has_more=TRUE. The internal joiners are NBSP
 * (U+00A0 = "\xc2\xa0") so xtext's word tokenizer (is_del checks for
 * ASCII space / '\n' / '<' / '>' / NUL) treats the whole string as
 * one clickable token. The leading U+2191 (up-arrow, "\xe2\x86\x91")
 * also acts as a secondary signature on the off chance the user's
 * click lands on a "word" that doesn't include the full sentinel.
 *
 * Functions instead of #define: the visible text is translated via
 * gettext, and chat_history_word_click compares the received word
 * against the same function so the click match keeps working in any
 * locale. Both functions cache the composed UTF-8 on first call. The
 * translatable msgids are the bare phrases "Load older messages" and
 * "Loading older messages..."; the leading arrow + NBSP joiners are
 * stitched in at runtime so translators don't have to worry about
 * preserving non-breaking-space characters by hand. */
extern const char *hx_load_older_sentinel (void);
extern const char *hx_loading_older_sentinel (void);

/* word_click handler that recognises the Load-older sentinel and
 * fires a BEFORE= chat-history fetch. Connected on every xtext that
 * renders chat history (the main chat output plus pchat outputs)
 * alongside gtkurl_xtext_word_click — both handlers run, each self-
 * filters on its own pattern. The session pointer is the user_data;
 * the cid the click belongs to is recovered by walking session
 * gchats and matching the xtext widget. */
extern void chat_history_word_click (GtkWidget *xtext, char *word,
                                     GdkEvent *event, gpointer data);
/* Phase 3 follow-up: hx_printf / hx_printf_prefix moved to
 * gtkhx_log.{c,h}; #include "gtkhx_log.h" rather than chat.h to
 * pull the decls in (chat.h forwards the include for source
 * compat with the existing call sites that include chat.h). */
#include "gtkhx_log.h"

/* The view-side handler for the "chat-log-line" signal. Connected
 * once in gtkhx_connect_signals at startup; the implementation
 * (xoutput_chat fan-out) lives in chat.c. */
struct _GtkhxSession;
typedef struct _GtkhxSession GtkhxSession;
extern void chat_log_line_handler (GtkhxSession *emitter,
                                   struct htlc_conn *htlc, guint cid,
                                   gpointer body, gpointer user_data);
extern void generate_colors (GtkWidget *widget);
extern void create_chat (session *sess);
extern void create_chat_window (GtkWidget *toolbar_window, gpointer data);
extern struct gtkhx_chat *pchat_new (session *sess, struct chat *chat);
extern void output_chat_subject (struct htlc_conn *htlc, guint32 cid,
                                 char *buf);
extern void output_chat_invitation (struct htlc_conn *htlc, guint32 cid,
                                    char *name);
extern struct gtkhx_chat *create_pchat_window (struct htlc_conn *htlc,
                                               struct chat *chat);
extern void hx_clear_chat (struct htlc_conn *htlc, guint32 cid, int subj);
extern int word_check (GtkWidget *xtext, char *word);

extern void hx_chat_user (struct htlc_conn *htlc, guint16 uid);
extern void hx_invite_user (struct htlc_conn *htlc, guint16 uid, guint32 cid);
extern void hx_chat_join (struct htlc_conn *htlc, guint32 cid);
extern void hx_part_chat (struct htlc_conn *htlc, guint32 cid);
extern void hx_change_subject (struct htlc_conn *htlc, guint32 cid,
                               char *subject);
extern void hx_send_chat (struct htlc_conn *htlc, char *str, guint32 cid,
                          guint16 style);

/* Refresh xtext palette slots that depend on Light / Dark theme
 * (XTEXT_FG / XTEXT_BG plus the selection colours) and push the
 * new palette into every live xtext widget. Called once at startup
 * after AdwStyleManager comes up, and again whenever the manager's
 * `dark` property flips. The mIRC palette slots (0..31) are theme-
 * agnostic and stay put. */
extern void gtkhx_apply_theme_palette (gboolean dark);

#endif
