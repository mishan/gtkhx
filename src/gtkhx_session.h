/*
 * gtkhx_session.h — singleton GObject that emits the model→view
 * notifications the rest of the codebase used to fire through the
 * hx_output vtable.
 *
 * Phase 3 of the MVC cleanup: signals replace the function-pointer
 * vtable. The advantages:
 *
 *   - Multiple subscribers. Today the chat output, the URL handler,
 *     and the optional protocol log might all want to see incoming
 *     chat messages — the vtable forces a single dispatch point
 *     that fans out by hand. Signals just have everyone subscribe.
 *   - Detail strings. A future multi-window UI can subscribe to a
 *     specific chat's events ("user-changed::cid-42") without
 *     filtering in the handler.
 *   - Loose coupling. Model-side files don't need to know about the
 *     view structure at all — they emit; the view subscribes at
 *     construction time.
 *
 * Migration is incremental: each signal replaces one vtable entry
 * at a time, with the existing output_* view functions continuing
 * to live in their current files as signal handlers connected at
 * startup. Lifecycle hooks (init, loop) stay as direct calls —
 * they aren't notifications.
 */

#ifndef HX_GTKHX_SESSION_H
#define HX_GTKHX_SESSION_H 1

#include <glib-object.h>
#include "protocol.h"

G_BEGIN_DECLS

/* Forward-declare types whose pointers we accept; the full struct
 * definitions live in session.h (which transitively pulls in GTK).
 * Letting model-side callers include gtkhx_session.h without
 * dragging GTK in via session.h matters for unit tests. */
typedef struct _session       session;
struct gnews_folder;
struct gnews_catalog;
struct news_post;
struct htxf_conn;
struct chat;
struct hx_user;
struct cached_filelist;
struct hl_filelist_hdr;

#include <netinet/in.h>          /* struct in_addr for tracker signal */
/* struct task already defined in protocol.h (included above). */

#define GTKHX_TYPE_SESSION (gtkhx_session_get_type ())
G_DECLARE_FINAL_TYPE (GtkhxSession, gtkhx_session, GTKHX, SESSION, GObject)

/* Returns the singleton emitter. Allocated lazily on first call;
 * lives until the process exits. Callers may freely reference and
 * unreference, but the singleton itself owns the canonical reference. */
GtkhxSession *gtkhx_session_get_default (void);

/* Signal emit helpers — small inline-friendly wrappers around
 * g_signal_emit_by_name that hide the singleton lookup from the
 * model-side caller. Names mirror the old hx_output.* members for
 * easy diffability against the vtable.
 *
 * Each helper corresponds to one signal declared in
 * gtkhx_session.c's class_init. When you add a new helper, add the
 * matching signal there too (and connect the existing view-side
 * handler in gtkhx.c's init path). */
void gtkhx_session_emit_chat (GtkhxSession *self,
                              struct htlc_conn *htlc,
                              guint32 cid, const char *body, guint16 len);

void gtkhx_session_emit_chat_subject (GtkhxSession *self,
                                      struct htlc_conn *htlc,
                                      guint32 cid, const char *subj);

void gtkhx_session_emit_chat_invitation (GtkhxSession *self,
                                         struct htlc_conn *htlc,
                                         guint32 cid, const char *name);

void gtkhx_session_emit_msg (GtkhxSession *self,
                             const char *name, guint16 uid,
                             const char *body);

/* Login + news notifications. agreement fires once after the
 * AGREEMENT chunks arrive post-login; the news-* variants fire
 * for the four 1.x / 1.5+ news flows. */
void gtkhx_session_emit_agreement (GtkhxSession *self,
                                   session *sess,
                                   const char *agreement, guint16 len);
void gtkhx_session_emit_news_file (GtkhxSession *self,
                                   struct htlc_conn *htlc,
                                   const char *news, guint16 len);
void gtkhx_session_emit_news_post (GtkhxSession *self,
                                   struct htlc_conn *htlc,
                                   const char *news, guint16 len);
void gtkhx_session_emit_news_folder (GtkhxSession *self,
                                     struct gnews_folder *gfnews);
void gtkhx_session_emit_news_catalog (GtkhxSession *self,
                                      struct gnews_catalog *gcnews);
void gtkhx_session_emit_news_thread (GtkhxSession *self,
                                     struct news_post *post);

/* Per-chat user-list mutations. user-changed carries the NEW values
 * so a view that wants to highlight a rename / icon change can diff
 * them against the user's current cached state (which still holds
 * the OLD values when the signal fires). */
void gtkhx_session_emit_user_create (GtkhxSession *self,
                                     struct htlc_conn *htlc,
                                     struct chat *chat,
                                     struct hx_user *user,
                                     const char *nam,
                                     guint16 icon, guint16 color);
void gtkhx_session_emit_user_delete (GtkhxSession *self,
                                     struct htlc_conn *htlc,
                                     struct chat *chat,
                                     struct hx_user *user);
void gtkhx_session_emit_user_change (GtkhxSession *self,
                                     struct htlc_conn *htlc,
                                     struct chat *chat,
                                     struct hx_user *user,
                                     const char *nam,
                                     guint16 icon, guint16 color);
void gtkhx_session_emit_users_clear (GtkhxSession *self,
                                     struct htlc_conn *htlc,
                                     struct chat *chat);
void gtkhx_session_emit_user_info (GtkhxSession *self,
                                   guint16 uid, const char *nam,
                                   const char *info, guint16 len);

/* Files / transfers / tracker / tasks. */
void gtkhx_session_emit_file_info (GtkhxSession *self,
                                   const char *path, const char *name,
                                   const char *creator, const char *type,
                                   const char *comments,
                                   const char *modified,
                                   const char *created,
                                   guint32 size);
void gtkhx_session_emit_file_list (GtkhxSession *self,
                                   struct cached_filelist *cfl,
                                   struct hl_filelist_hdr *fh,
                                   void *data);
void gtkhx_session_emit_file_update (GtkhxSession *self,
                                     session *sess,
                                     struct htxf_conn *htxf);
void gtkhx_session_emit_xfer_queue (GtkhxSession *self,
                                    session *sess,
                                    struct htxf_conn *htxf);
void gtkhx_session_emit_tracker_server_create (GtkhxSession *self,
                                               struct in_addr addr,
                                               guint16 port,
                                               guint16 nusers,
                                               const char *nam,
                                               const char *desc,
                                               int total);
void gtkhx_session_emit_task_update (GtkhxSession *self,
                                     session *sess,
                                     struct task *tsk);

/* chat-log-line — "show this string in chat `cid`'s output". The
 * payload is already fully formatted text (post-vsnprintf).
 * gtkhx_log.{c,h} wraps this in printf-style helpers
 * (hx_printf / hx_printf_prefix); model-side files keep calling
 * those, but the model→view edge is now signal-shaped. */
void gtkhx_session_emit_chat_log_line (GtkhxSession *self,
                                       struct htlc_conn *htlc,
                                       guint32 cid, const char *body);

/* High-level connection state. The signal's view-side handler
 * translates each state into the per-aspect UI updates (toolbar
 * buttons enabled/disabled, status-bar label, disconnect button
 * visibility, window titles, connect-task progress ticker) that
 * model-side code (network.c hx_connect / connect_fail /
 * hx_htlc_close) used to issue by name. */
typedef enum {
	GTKHX_CONNECTION_DISCONNECTED,   /* No connection. UI shows
	                                  * disconnected chrome. */
	GTKHX_CONNECTION_CONNECTING,     /* DNS resolve + TCP connect in
	                                  * flight. Status bar reads
	                                  * "connecting", disconnect
	                                  * button is shown. */
	GTKHX_CONNECTION_TCP_CONNECTED,  /* TCP connected; magic
	                                  * exchange + login send in
	                                  * flight. Status bar reads
	                                  * "connected". */
	GTKHX_CONNECTION_HANDSHAKE_DONE, /* LOGIN sent. Connect task
	                                  * progress ticker is done. */
} GtkhxConnectionState;

void gtkhx_session_emit_connection_state (GtkhxSession *self,
                                          GtkhxConnectionState state);

/* Connects every Phase 3 signal handler to the supplied emitter.
 * Called once from fe_init at startup. The handlers themselves are
 * static adapters in gtkhx.c that bridge the marshaller signature
 * to the legacy view function signatures. */
void gtkhx_connect_signals (GtkhxSession *emitter);

G_END_DECLS

#endif /* HX_GTKHX_SESSION_H */
