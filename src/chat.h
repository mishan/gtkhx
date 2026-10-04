#ifndef HX_CHAT_H
#define HX_CHAT_H

extern GdkRGBA colors[];

/* lazy-allocate the session's chat registry (see below) AND seed it
 * with the always-required public chat at cid=0. Safe to call
 * multiple times — only the first call constructs the registry.
 * gtkhx.c calls it during init, before any other chat_* operation. */
extern void chats_init (session *sess);

extern struct chat *chat_new (session *sess, guint32 cid);
extern void chat_delete (session *sess, struct chat *chat);
extern struct chat *chat_with_cid (session *sess, guint32 cid);

/* The per-session cid → conversation registry (was a GHashTable), now the Rust
 * HxChatRegistry in the gtkhx-core crate (rust/crates/gtkhx-core/src/
 * chat_registry.rs). Opaque here; sess->chats holds one. chats_init creates it
 * with chat_free as the destroy callback; chat_new / _delete / _with_cid wrap
 * insert / remove / lookup. Disconnect teardown and the palette push walk it
 * with hx_chats_count + hx_chats_get_at, plus hx_chats_cid_at where the cid is
 * needed without reaching the (gtkhx-ui) conversation accessors. It lives in
 * gtkhx-core — the crate the headless wire-level tests link — so network.c's
 * teardown resolves these symbols without pulling in the UI crate. */
typedef struct HxChatRegistry HxChatRegistry;
typedef void (*HxChatDestroyFn) (void *chat);
extern HxChatRegistry *hx_chats_new (HxChatDestroyFn destroy);
extern void hx_chats_free (HxChatRegistry *reg);
extern void hx_chats_insert (HxChatRegistry *reg, guint32 cid, void *chat);
extern void hx_chats_remove (HxChatRegistry *reg, guint32 cid);
extern void *hx_chats_lookup (HxChatRegistry *reg, guint32 cid);
extern guint32 hx_chats_count (HxChatRegistry *reg);
extern void *hx_chats_get_at (HxChatRegistry *reg, guint32 i);
extern guint32 hx_chats_cid_at (HxChatRegistry *reg, guint32 i);

/* struct chat is an opaque HxConversation handle
 * (gtkhx-ui/src/conversation.rs): C reaches its fields only through these
 * accessors. hx_conversation_new / _free are the storage lifecycle used by
 * chat_new / chat_free; hx_chat_cid (declared lower down) + the rest are field
 * access. The member model is a borrowed GObject* (do not unref); the view is
 * the open struct gtkhx_chat* or NULL. */
extern struct chat *hx_conversation_new (guint32 cid);
extern void hx_conversation_free (struct chat *chat);
extern const char *hx_chat_subject (struct chat *chat);
extern void hx_chat_set_subject (struct chat *chat, const char *s, gsize len);
extern void *hx_chat_member_model (struct chat *chat);
/* Conversation-owned Rust handles (created in hx_conversation_new, freed in
 * hx_conversation_free): the input line history and the inline-media token
 * table. Borrowed — do not free. */
extern void *hx_chat_input_history (struct chat *chat);
extern void *hx_chat_media_table (struct chat *chat);
extern struct gtkhx_chat *hx_chat_view (struct chat *chat);
extern void hx_chat_set_view (struct chat *chat, struct gtkhx_chat *view);
extern struct gtkhx_chat *gchat_with_cid (session *sess, guint32 cid);
extern void gchat_delete (session *sess, struct gtkhx_chat *gchat);
/* Render one log line. `tag` is the gutter tag ("hx", a broadcast
 * sender) or NULL for an untagged line; `tag_color` its palette index.
 * They arrive as parameters rather than escapes embedded in `chat` —
 * see gtkhx_session.h's chat-log-line note.
 *
 * `cid` is the conversation being rendered into, and is what a nick
 * lookup resolves against — a private chat has its own membership, so
 * assuming the public room here misidentifies speakers in pchats. */
extern void xprintline (GtkWidget *text, guint32 cid, char *chat, size_t len,
                        const char *tag, gint16 tag_color);

/* chat-signal renderer. Takes a pre-parsed HxChatEvent
 * (sender/body slices + is_info/is_self flags from
 * gtkhx-core's chat_event_new). Bypasses the legacy hx_printf round-trip
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

/* A chat output, configured for GtkHx (gtkhx-ui's chat_view.rs): the
 * chat preferences, the hotline:// scheme, the avatar resolver, the URL
 * menu. `palette` holds ROTULUS_PAL_COLS colors. Returned floating. */
extern GtkWidget *gtkhx_chat_view_new (const GdkRGBA *palette,
                                       const char *font);
/* Re-apply the chat preferences to a view gtkhx_chat_view_new built. */
extern void gtkhx_chat_view_configure (GtkWidget *view);

/* Palette slots the chat code reaches for, by what they mean here. */
#define HX_CHAT_INFO_COLOR ROTULUS_PAL_SYSTEM /* "[hx]" */
#define HX_CHAT_INFO_BRACKET_COLOR ROTULUS_PAL_SYSTEM_BRACKET
#define HX_CHAT_HIGHLIGHT_COLOR ROTULUS_PAL_HIGHLIGHT
#define HX_CHAT_PLACEHOLDER_COLOR ROTULUS_PAL_MUTED /* media alt text */

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
                                   gpointer name, gint color, gpointer body,
                                   gpointer user_data);
extern void generate_colors (GtkWidget *widget);

/* Palette slot a nick renders in: the self-nick role for our own, one
 * of the theme's per-nick colors (picked by a stable hash of the name)
 * when it has any, the plain nick role otherwise. */
extern gint16 hx_chat_nick_color (const char *nick, gsize nick_len,
                                  gboolean is_self);
extern void create_chat (session *sess);
/* create_chat_window is the gtkhx-ui `chat` Rust shell (dock registration via
 * dock_bridge, CENTER area); these are its C content-build + post-embed hooks,
 * mirroring users_bridge.c / news_browser.c. build_content builds the public
 * chat + the AdwTabView hosting the pchat/PM tabs and returns the content box;
 * after_embed carries the session onto the panel + focuses the input. */
extern void create_chat_window (GtkWidget *toolbar_window, gpointer data);
/* gtkhx_chat_build_content moved to Rust (gtkhx-ui chat.rs::build_content).
 * These are the C leaf seams it calls: build_leaves makes the public chat's
 * C-coupled leaf widgets (subject / input / media button), and
 * clear_content_ptrs is the panel-content "destroy" handler. */
extern struct gtkhx_chat *gtkhx_chat_build_leaves (session *sess);
extern void gtkhx_chat_clear_content_ptrs (struct gtkhx_chat *gchat);
extern void gtkhx_chat_after_embed (session *sess);
extern struct gtkhx_chat *pchat_new (session *sess, struct chat *chat);
extern void output_chat_subject (struct htlc_conn *htlc, guint32 cid,
                                 char *buf);
extern void output_chat_invitation (struct htlc_conn *htlc, guint32 cid,
                                    char *name);
/* create_pchat_window is the gtkhx-ui `pchat` Rust content port: it assembles
 * the private-chat tab layout around these C-built leaf widgets + the user
 * sidebar, then adds the tab. Mirrors msg.c's create_msg + hx_msgwin_* seam. */
extern struct gtkhx_chat *create_pchat_window (struct htlc_conn *htlc,
                                               struct chat *chat);
extern struct gtkhx_chat *gtkhx_pchat_new (struct htlc_conn *htlc,
                                           struct chat *chat);
extern GtkWidget *gtkhx_pchat_user_sidebar (struct htlc_conn *htlc,
                                            struct chat *chat);
extern guint32 hx_chat_cid (struct chat *chat);
extern GtkWidget *hx_gchat_output (struct gtkhx_chat *g);
extern GtkWidget *hx_gchat_vscroll (struct gtkhx_chat *g);
extern GtkWidget *hx_gchat_input (struct gtkhx_chat *g);
extern GtkWidget *hx_gchat_subject (struct gtkhx_chat *g);
extern GtkWidget *hx_gchat_media_btn (struct gtkhx_chat *g);
extern void hx_gchat_set_window (struct gtkhx_chat *g, GtkWidget *w);
extern GtkWidget *hx_gchat_window (struct gtkhx_chat *g);
extern guint32 hx_gchat_cid (struct gtkhx_chat *g);
/* The pchat's user-list view (HxUserListView*), or NULL. Opaque here; users.c
 * has the real type via users_view.h. */
struct _HxUserListView;
extern struct _HxUserListView *hx_gchat_userlist (struct gtkhx_chat *g);
extern void hx_clear_chat (struct htlc_conn *htlc, guint32 cid, int subj);

/* Install the chat / pchat input key handler on `view` (Rust, chat_input.rs) —
 * Ctrl+K, Return-to-send, Shift+Return newline, Tab completion, Up/Down
 * history. Replaces the C chat_input_key_pressed + its GtkEventControllerKey
 * wiring; captures sess, cid, and the input-history handle for the input
 * widget's lifetime. Idempotent + NULL-safe. */
extern void gtkhx_chat_input_attach (GtkWidget *view, session *sess,
                                     guint32 cid, void *history);

/* Nick-completion core, called from the Rust input handler. Rewrites the
 * entry buffer + caret with the completion against `member_model`. `pos` is
 * the caret's char offset; `reverse` steps the Tab-cycle backwards. */
extern int tab_nick_comp (session *sess, void *member_model, char *text,
                          gboolean reverse, int pos, GtkWidget *entry);

extern void hx_chat_user (struct htlc_conn *htlc, guint16 uid);
extern void hx_invite_user (struct htlc_conn *htlc, guint16 uid, guint32 cid);
extern void hx_chat_join (struct htlc_conn *htlc, guint32 cid);
extern void hx_part_chat (struct htlc_conn *htlc, guint32 cid);
extern void hx_change_subject (struct htlc_conn *htlc, guint32 cid,
                               char *subject);
extern void hx_send_chat (struct htlc_conn *htlc, char *str, guint32 cid,
                          guint16 style);

/* Refresh xtext palette slots that depend on Light / Dark theme
 * (ROTULUS_PAL_FG / ROTULUS_PAL_BG plus the selection colours) and push the
 * new palette into every live xtext widget. Called once at startup
 * after AdwStyleManager comes up, and again whenever the manager's
 * `dark` property flips. The mIRC palette slots (0..31) are theme-
 * agnostic and stay put. */
extern void gtkhx_apply_theme_palette (gboolean dark);

/* View-side handler for the "chat-subject-notice" signal — the "Subject Changed
 * to: <subject>" chat line emitted by the Rust chat-subject receive handler
 * (hxhandlers::recv::chat). Applies gettext + INFOPREFIX. Connected in
 * gtkhx_connect_signals. */
extern void chat_subject_notice_handler (GtkhxSession *emitter,
                                         struct htlc_conn *htlc, guint cid,
                                         gpointer subject, gpointer user_data);

/* View-side handler for the "user-notice" signal (roster join / parts / rename
 * lines emitted by the Rust hxhandlers::recv::user handlers). Applies the showjoin pref +
 * gettext + INFOPREFIX. Connected in gtkhx_connect_signals. */
extern void user_notice_handler (GtkhxSession *emitter, struct htlc_conn *htlc,
                                 guint cid, guint kind, gpointer name,
                                 gpointer old_name, gpointer user_data);

#endif
