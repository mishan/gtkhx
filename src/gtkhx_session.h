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
#include "proto_helpers.h" /* HxChatEvent (chat signal payload) */

G_BEGIN_DECLS

/* Forward-declare types whose pointers we accept; the full struct
 * definitions live in session.h (which transitively pulls in GTK).
 * Letting model-side callers include gtkhx_session.h without
 * dragging GTK in via session.h matters for unit tests. */
typedef struct _session session;
struct gnews_folder;
struct gnews_catalog;
struct news_post;
struct htxf_conn;
struct chat;
struct hx_user;
struct cached_filelist;
struct hl_filelist_hdr;

#include <netinet/in.h> /* struct in_addr for tracker signal */
#include "tracker_event.h" /* HxTrackerServer (boxed payload) */
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
/* The "chat" signal payload was once raw bytes (body + len). It's
 * now a single boxed HxChatEvent so every subscriber sees the same
 * UTF-8-validated, sender/body-split, info/self-classified view of
 * the line. The emitter constructs the event from wire bytes via
 * hx_chat_event_new; subscribers get a borrowed pointer for the
 * duration of the signal emission (boxed copy/free run if a
 * subscriber needs to keep it). */
void gtkhx_session_emit_chat (GtkhxSession *self, struct htlc_conn *htlc,
                              HxChatEvent *event);

void gtkhx_session_emit_chat_subject (GtkhxSession *self,
                                      struct htlc_conn *htlc, guint32 cid,
                                      const char *subj);

void gtkhx_session_emit_chat_invitation (GtkhxSession *self,
                                         struct htlc_conn *htlc, guint32 cid,
                                         const char *name);

/* "chat-history-batch" — reply from a TRAN_GET_CHAT_HISTORY (700)
 * request landed. Payload: (htlc, cid, GPtrArray<HxHistoryEntry*>,
 * has_more).
 *
 * The GPtrArray is owned by the emitter for the duration of the
 * signal emission; entries are freed automatically (the array's
 * free_func is hx_history_entry_free) after the last subscriber
 * returns. Subscribers that need to keep entry data past the emit
 * must copy what they need (timestamp, nick, message bytes) before
 * returning — pointers into the array are invalid after.
 *
 * cid is the channel id requested. Today the only meaningful
 * channel is 0 (public chat); the spec reserves 1+ for future
 * named channels. */
void gtkhx_session_emit_chat_history_batch (GtkhxSession     *self,
                                            struct htlc_conn *htlc,
                                            guint32           cid,
                                            GPtrArray        *entries,
                                            gboolean          has_more);

/* The "msg" (private message) signal payload is a boxed HxMsgEvent
 * for the same reason "chat" carries an HxChatEvent: every
 * subscriber gets the same UTF-8-sanitised, self-classified view
 * of the message. */
void gtkhx_session_emit_msg (GtkhxSession *self, HxMsgEvent *event);

/* Login + news notifications. agreement fires once after the
 * AGREEMENT chunks arrive post-login; the news-* variants fire
 * for the four 1.x / 1.5+ news flows. */
void gtkhx_session_emit_agreement (GtkhxSession *self, session *sess,
                                   const char *agreement, guint16 len);
void gtkhx_session_emit_news_file (GtkhxSession *self, struct htlc_conn *htlc,
                                   const char *news, guint16 len);
void gtkhx_session_emit_news_post (GtkhxSession *self, struct htlc_conn *htlc,
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
void gtkhx_session_emit_user_create (GtkhxSession *self, struct htlc_conn *htlc,
                                     struct chat *chat, struct hx_user *user,
                                     const char *nam, guint16 icon,
                                     guint16 color);
void gtkhx_session_emit_user_delete (GtkhxSession *self, struct htlc_conn *htlc,
                                     struct chat *chat, struct hx_user *user);
void gtkhx_session_emit_user_change (GtkhxSession *self, struct htlc_conn *htlc,
                                     struct chat *chat, struct hx_user *user,
                                     const char *nam, guint16 icon,
                                     guint16 color);
void gtkhx_session_emit_users_clear (GtkhxSession *self, struct htlc_conn *htlc,
                                     struct chat *chat);
void gtkhx_session_emit_user_info (GtkhxSession *self, guint16 uid,
                                   const char *nam, const char *info,
                                   guint16 len);

/* Files / transfers / tracker / tasks. */
void gtkhx_session_emit_file_info (GtkhxSession *self, const char *path,
                                   const char *name, const char *creator,
                                   const char *type, const char *comments,
                                   const char *modified, const char *created,
                                   guint64 size);
void gtkhx_session_emit_file_list (GtkhxSession *self,
                                   struct cached_filelist *cfl,
                                   struct hl_filelist_hdr *fh, void *data);
void gtkhx_session_emit_file_update (GtkhxSession *self, session *sess,
                                     struct htxf_conn *htxf);
void gtkhx_session_emit_xfer_queue (GtkhxSession *self, session *sess,
                                    struct htxf_conn *htxf);
/* "xfer-destroyed" — fires when an htxf_conn is about to leave the
 * live xfers[] list. The view (tasks window) must NULL any cached
 * pointers to this htxf in response, because a subsequent unref may
 * free the slab. Handlers run synchronously inside
 * xfer_remove_from_list; they should clear pointers and not call
 * back into xfer_* APIs. */
void gtkhx_session_emit_xfer_destroyed (GtkhxSession *self, session *sess,
                                        struct htxf_conn *htxf);
/* tracker-server-create — one server record landed during a
 * tracker listing fetch. Payload is a boxed HxTrackerServer
 * carrying address (printable), addr_type discriminator,
 * port/nusers, UTF-8 name+desc, optional v3 TLV trailer, and the
 * per-batch total. Subscribers get a borrowed pointer for the
 * duration of the signal emit; boxed copy hooks let any that
 * want to keep the event past return take a ref. */
void gtkhx_session_emit_tracker_server_create (GtkhxSession *self,
                                               HxTrackerServer *event);

/* tracker-batch-begin — emitted once per tracker URL right after
 * network.c settles on a wire version, BEFORE any records for
 * that batch land. Lets the view create / recycle a per-tracker
 * section, choose which columns to show, and render an
 * "expecting N..." subtitle. Records for the batch arrive via
 * tracker-server-create until the next batch-begin (or the run
 * ends). */
void gtkhx_session_emit_tracker_batch_begin (GtkhxSession *self,
                                             const char *tracker_url,
                                             guint8 version,
                                             guint16 expected_count);

void gtkhx_session_emit_task_update (GtkhxSession *self, session *sess,
                                     struct task *tsk);

/* chat-log-line — "show this string in chat `cid`'s output". The
 * payload is already fully formatted text (post-vsnprintf).
 * gtkhx_log.{c,h} wraps this in printf-style helpers
 * (hx_printf / hx_printf_prefix); model-side files keep calling
 * those, but the model→view edge is now signal-shaped. */
void gtkhx_session_emit_chat_log_line (GtkhxSession *self,
                                       struct htlc_conn *htlc, guint32 cid,
                                       const char *body);

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
    GTKHX_CONNECTION_LOGIN_READY,    /* hx_post_login_fetches has
	                                  * fired — the server has accepted
	                                  * us as a fully-joined user
	                                  * (either AGREEMENTAGREE went out
	                                  * for 1.5+ servers, or the 1.0/
	                                  * 1.2 fallback timer fired). This
	                                  * is the boundary at which post-
	                                  * login RPCs (USER_GETLIST,
	                                  * FILE_LIST for the files
	                                  * browser, chat history catch-up)
	                                  * are spec-safe to send. */
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
