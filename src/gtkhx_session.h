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

/* Connects every Phase 3 signal handler to the supplied emitter.
 * Called once from fe_init at startup. The handlers themselves are
 * static adapters in gtkhx.c that bridge the marshaller signature
 * to the legacy view function signatures. */
void gtkhx_connect_signals (GtkhxSession *emitter);

G_END_DECLS

#endif /* HX_GTKHX_SESSION_H */
