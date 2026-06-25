/*
 * gtkhx_session.c — see gtkhx_session.h.
 *
 * The singleton emitter that replaces the hx_output vtable's
 * notification half. Phase 3 of the MVC cleanup ships one signal
 * at a time, with view-side handlers connected at startup in
 * gtkhx.c's init path; this file holds the GObject scaffolding +
 * signal table.
 *
 * Why a singleton rather than per-chat or per-session GObjects:
 *
 *   - We only ever have one session (MAX_CONN is a half-built
 *     abstraction — see CLAUDE.md). Per-session emitters would
 *     just be the singleton in disguise.
 *   - Per-chat GObjects would let `g_signal_emit_by_name(chat,
 *     "user-joined", ...)` target a specific chat — but it would
 *     require GObject-ifying struct chat / hx_user / task /
 *     gtkhx_chat, which is a multi-thousand-line refactor for
 *     marginal gain. Detail strings on signals ("user-changed::cid-42")
 *     give us the same per-chat filtering capability without
 *     touching the data structs.
 *
 * The signal handler signature matches the old vtable signature so
 * the existing output_* implementations in chat.c / users.c / etc.
 * can be connected directly with no glue.
 */

#include "config.h"
#include <glib-object.h>
#include "protocol.h"
#include "gtkhx_session.h"

/* Phase R3.0: hxbridge Rust crate exposes the pointer-pair signal
 * emit. Hand-declared extern per the R2 FFI convention — a signature
 * mismatch surfaces as a link-time undefined symbol. */
extern void gtkhx_bridge_emit_pointer_pair_signal (void *session_ptr,
                                                   const char *signal_name,
                                                   void *arg0,
                                                   void *arg1);

struct _GtkhxSession {
    GObject parent_instance;
};

G_DEFINE_FINAL_TYPE (GtkhxSession, gtkhx_session, G_TYPE_OBJECT)

enum {
    SIGNAL_CHAT,
    SIGNAL_CHAT_SUBJECT,
    SIGNAL_CHAT_INVITATION,
    SIGNAL_CHAT_HISTORY_BATCH,
    SIGNAL_MSG,
    SIGNAL_AGREEMENT,
    SIGNAL_NEWS_FILE,
    SIGNAL_NEWS_POST,
    SIGNAL_NEWS_FOLDER,
    SIGNAL_NEWS_CATALOG,
    SIGNAL_NEWS_THREAD,
    SIGNAL_USER_CREATE,
    SIGNAL_USER_DELETE,
    SIGNAL_USER_CHANGE,
    SIGNAL_USERS_CLEAR,
    SIGNAL_USER_INFO,
    SIGNAL_FILE_INFO,
    SIGNAL_FILE_LIST,
    SIGNAL_FILE_UPDATE,
    SIGNAL_XFER_QUEUE,
    SIGNAL_XFER_DESTROYED,
    SIGNAL_TRACKER_SERVER_CREATE,
    SIGNAL_TRACKER_BATCH_BEGIN,
    SIGNAL_TASK_UPDATE,
    SIGNAL_CHAT_LOG_LINE,
    SIGNAL_CONNECTION_STATE,
    SIGNAL_LAST
};

static guint signals[SIGNAL_LAST];

static void
gtkhx_session_init (GtkhxSession *self)
{
    (void)self;
}

static void
gtkhx_session_class_init (GtkhxSessionClass *klass)
{
    /* "chat" — incoming chat message. Phase 5+: payload is a
	 * boxed HxChatEvent that carries cid, UTF-8 line, sender /
	 * body slices, is_info / is_self flags — every subscriber
	 * gets the same parse result without re-doing the work. The
	 * boxed type's copy/free hooks (G_DEFINE_BOXED_TYPE in
	 * proto_helpers.c) handle the multi-subscriber lifetime. */
    signals[SIGNAL_CHAT]
        = g_signal_new ("chat", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                        0,          /* class_offset */
                        NULL, NULL, /* accumulator + data */
                        NULL,       /* marshaller: NULL → libffi-derived */
                        G_TYPE_NONE, 2, G_TYPE_POINTER, /* struct htlc_conn * */
                        HX_TYPE_CHAT_EVENT); /* HxChatEvent * (boxed) */

    /* "chat-subject" — subject changed in chat `cid`. Replaces
	 * hx_output.chat_subject. The arg list is (htlc, cid,
	 * subject-string). */
    signals[SIGNAL_CHAT_SUBJECT]
        = g_signal_new ("chat-subject", G_TYPE_FROM_CLASS (klass),
                        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 3,
                        G_TYPE_POINTER, G_TYPE_UINT, G_TYPE_POINTER);

    /* "chat-invitation" — server invited us to chat `cid`. Replaces
	 * hx_output.chat_invitation. `name` is the inviter's display
	 * name (server-provided, already sanitised by the rcv path). */
    signals[SIGNAL_CHAT_INVITATION]
        = g_signal_new ("chat-invitation", G_TYPE_FROM_CLASS (klass),
                        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 3,
                        G_TYPE_POINTER, G_TYPE_UINT, G_TYPE_POINTER);

    /* "chat-history-batch" — fogWraith Chat History extension.
	 * Payload: (htlc *, cid, GPtrArray<HxHistoryEntry*> *, has_more).
	 * Emitter (rcv_task_chat_history in rcv.c) owns the GPtrArray
	 * for the duration of the emission; entries are freed by the
	 * array's free_func (hx_history_entry_free) right after.
	 * Subscribers that need to keep entry data past the emit MUST
	 * copy the bytes they want — the pointers go invalid. */
    signals[SIGNAL_CHAT_HISTORY_BATCH]
        = g_signal_new ("chat-history-batch", G_TYPE_FROM_CLASS (klass),
                        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 4,
                        G_TYPE_POINTER, /* struct htlc_conn *       */
                        G_TYPE_UINT,    /* cid (guint32)            */
                        G_TYPE_POINTER, /* GPtrArray<HxHistoryEntry*>* */
                        G_TYPE_BOOLEAN); /* has_more                */

    /* "msg" — incoming private message. Phase 5+: payload is a
	 * boxed HxMsgEvent (uid + name + body + is_self/is_broadcast
	 * flags), same architectural shape as the "chat" signal. */
    signals[SIGNAL_MSG]
        = g_signal_new ("msg", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                        NULL, NULL, NULL, G_TYPE_NONE, 1, HX_TYPE_MSG_EVENT);

    /* "agreement" — post-login agreement text from the server.
	 * (session*, agreement-string, len). */
    signals[SIGNAL_AGREEMENT]
        = g_signal_new ("agreement", G_TYPE_FROM_CLASS (klass),
                        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 3,
                        G_TYPE_POINTER, G_TYPE_POINTER, G_TYPE_UINT);

    /* News notifications — 1.x flat news (news-file / news-post) +
	 * 1.5+ threaded news (news-folder / news-catalog / news-thread). */
    signals[SIGNAL_NEWS_FILE]
        = g_signal_new ("news-file", G_TYPE_FROM_CLASS (klass),
                        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 3,
                        G_TYPE_POINTER, G_TYPE_POINTER, G_TYPE_UINT);

    signals[SIGNAL_NEWS_POST]
        = g_signal_new ("news-post", G_TYPE_FROM_CLASS (klass),
                        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 3,
                        G_TYPE_POINTER, G_TYPE_POINTER, G_TYPE_UINT);

    signals[SIGNAL_NEWS_FOLDER] = g_signal_new (
        "news-folder", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
        NULL, NULL, G_TYPE_NONE, 1, G_TYPE_POINTER);

    signals[SIGNAL_NEWS_CATALOG] = g_signal_new (
        "news-catalog", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
        NULL, NULL, G_TYPE_NONE, 1, G_TYPE_POINTER);

    signals[SIGNAL_NEWS_THREAD] = g_signal_new (
        "news-thread", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
        NULL, NULL, G_TYPE_NONE, 1, G_TYPE_POINTER);

    /* Per-chat user-list mutations. user-create / user-change
	 * carry the NEW (nam, icon, color) so view diffs against the
	 * still-OLD user->* are possible. */
    signals[SIGNAL_USER_CREATE] = g_signal_new (
        "user-create", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
        NULL, NULL, G_TYPE_NONE, 6, G_TYPE_POINTER, /* htlc */
        G_TYPE_POINTER,                             /* chat */
        G_TYPE_POINTER,                             /* user */
        G_TYPE_POINTER,                             /* nam */
        G_TYPE_UINT,                                /* icon */
        G_TYPE_UINT);                               /* color */

    signals[SIGNAL_USER_DELETE]
        = g_signal_new ("user-delete", G_TYPE_FROM_CLASS (klass),
                        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 3,
                        G_TYPE_POINTER, G_TYPE_POINTER, G_TYPE_POINTER);

    signals[SIGNAL_USER_CHANGE] = g_signal_new (
        "user-change", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
        NULL, NULL, G_TYPE_NONE, 6, G_TYPE_POINTER, G_TYPE_POINTER,
        G_TYPE_POINTER, G_TYPE_POINTER, G_TYPE_UINT, G_TYPE_UINT);

    signals[SIGNAL_USERS_CLEAR] = g_signal_new (
        "users-clear", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
        NULL, NULL, G_TYPE_NONE, 2, G_TYPE_POINTER, G_TYPE_POINTER);

    signals[SIGNAL_USER_INFO] = g_signal_new (
        "user-info", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
        NULL, NULL, G_TYPE_NONE, 4, G_TYPE_UINT, /* uid (guint16 widened) */
        G_TYPE_POINTER,                          /* nam */
        G_TYPE_POINTER,                          /* info body */
        G_TYPE_UINT);                            /* len */

    /* Files / transfers / tracker / tasks. */
    signals[SIGNAL_FILE_INFO]
        = g_signal_new ("file-info", G_TYPE_FROM_CLASS (klass),
                        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 8,
                        G_TYPE_POINTER, G_TYPE_POINTER, /* path, name */
                        G_TYPE_POINTER, G_TYPE_POINTER, /* creator, type */
                        G_TYPE_POINTER,                 /* comments */
                        G_TYPE_POINTER, G_TYPE_POINTER, /* modified, created */
                        G_TYPE_UINT64);                 /* size — 64-bit for
														 * large-file support */

    signals[SIGNAL_FILE_LIST]
        = g_signal_new ("file-list", G_TYPE_FROM_CLASS (klass),
                        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 3,
                        G_TYPE_POINTER, G_TYPE_POINTER, G_TYPE_POINTER);

    signals[SIGNAL_FILE_UPDATE] = g_signal_new (
        "file-update", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
        NULL, NULL, G_TYPE_NONE, 2, G_TYPE_POINTER, G_TYPE_POINTER);

    signals[SIGNAL_XFER_QUEUE] = g_signal_new (
        "xfer-queue", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
        NULL, NULL, G_TYPE_NONE, 2, G_TYPE_POINTER, G_TYPE_POINTER);

    /* "xfer-destroyed" — emitted when an htxf_conn is about to leave
	 * the xfers[] live-list. Subscribers (specifically the tasks
	 * window) must NULL out any cached pointers to the htxf because
	 * a subsequent unref may free it (the worker thread's ref is
	 * still alive when xfer_delete fires from rcv.c / cancel, but
	 * the next cleanup_dispatch on the worker exit unrefs to zero
	 * and the slab is freed). Without this signal the tasks window
	 * holds a dangling htxf pointer that crashes on the next click.
	 * Handlers run synchronously; they should clear local pointers
	 * and not call back into xfer_* APIs.
	 *
	 * Payload mirrors xfer-queue / file-update for diffability:
	 * (session *, struct htxf_conn *). */
    signals[SIGNAL_XFER_DESTROYED] = g_signal_new (
        "xfer-destroyed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
        NULL, NULL, G_TYPE_NONE, 2, G_TYPE_POINTER, G_TYPE_POINTER);

    /* tracker-server-create — boxed HxTrackerServer carrying the
     * full parsed record (addr_type, printable address, port /
     * users, UTF-8 name + description, optional TLV trailer for
     * v3, batch total). Same pattern as the Phase 5 HxChatEvent
     * / HxMsgEvent payloads: future-proof for v3's richer
     * metadata without growing the marshaller signature each
     * time. */
    signals[SIGNAL_TRACKER_SERVER_CREATE] = g_signal_new (
        "tracker-server-create", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL, G_TYPE_NONE, 1, HX_TYPE_TRACKER_SERVER);

    /* tracker-batch-begin — fires once per tracker URL right after
     * network.c settles on a wire version (v1 vs v3) and BEFORE any
     * tracker-server-create signals carrying records for that batch
     * arrive. Used by the view (tracker.c) to:
     *   - create (or recycle) a per-tracker section in the result UI
     *   - choose which columns to show (v1 sections hide Country / Caps)
     *   - render a "fetching N..." subtitle (expected_count is the
     *     wire-declared total: nservers for v1, record_count for v3).
     *
     * Signal types are (G_TYPE_STRING, G_TYPE_UCHAR, G_TYPE_UINT):
     * tracker_url is a NUL-terminated UTF-8 string, version is a
     * single byte (1 or 3), and expected_count is widened from the
     * wire's u16 to a G_TYPE_UINT here so the marshaller doesn't
     * have to learn about a separate u16 type. View-side handlers
     * just cast back to guint16 after reading.
     *
     * Multiple trackers fire sequentially (run_ctx walks the list one
     * at a time), so the most recently emitted tracker_url is the
     * implicit "current section" for the records that follow until
     * the next batch-begin.
     */
    signals[SIGNAL_TRACKER_BATCH_BEGIN] = g_signal_new (
        "tracker-batch-begin", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL, G_TYPE_NONE, 3,
        G_TYPE_STRING, G_TYPE_UCHAR, G_TYPE_UINT);

    signals[SIGNAL_TASK_UPDATE] = g_signal_new (
        "task-update", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
        NULL, NULL, G_TYPE_NONE, 2, G_TYPE_POINTER, G_TYPE_POINTER);

    /* "chat-log-line" — show this already-formatted string in
	 * chat `cid`'s output. The body is owned by the emitter for
	 * the duration of the emit (so no buffer ownership transfer
	 * through the signal). */
    signals[SIGNAL_CHAT_LOG_LINE]
        = g_signal_new ("chat-log-line", G_TYPE_FROM_CLASS (klass),
                        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 3,
                        G_TYPE_POINTER, G_TYPE_UINT, G_TYPE_POINTER);

    /* "connection-state-changed" — high-level connection FSM
	 * (DISCONNECTED / CONNECTING / TCP_CONNECTED / HANDSHAKE_DONE).
	 * Single GUINT arg carries the GtkhxConnectionState value. */
    signals[SIGNAL_CONNECTION_STATE] = g_signal_new (
        "connection-state-changed", G_TYPE_FROM_CLASS (klass),
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_UINT);
}

GtkhxSession *
gtkhx_session_get_default (void)
{
    static GtkhxSession *singleton = NULL;
    if (G_UNLIKELY (singleton == NULL)) {
        singleton = g_object_new (GTKHX_TYPE_SESSION, NULL);
    }
    return singleton;
}

void
gtkhx_session_emit_chat (GtkhxSession *self, struct htlc_conn *htlc,
                         HxChatEvent *event)
{
    g_signal_emit (self, signals[SIGNAL_CHAT], 0, htlc, event);
}

void
gtkhx_session_emit_chat_subject (GtkhxSession *self, struct htlc_conn *htlc,
                                 guint32 cid, const char *subj)
{
    g_signal_emit (self, signals[SIGNAL_CHAT_SUBJECT], 0, htlc, cid, subj);
}

void
gtkhx_session_emit_chat_invitation (GtkhxSession *self, struct htlc_conn *htlc,
                                    guint32 cid, const char *name)
{
    g_signal_emit (self, signals[SIGNAL_CHAT_INVITATION], 0, htlc, cid, name);
}

void
gtkhx_session_emit_chat_history_batch (GtkhxSession     *self,
                                       struct htlc_conn *htlc,
                                       guint32           cid,
                                       GPtrArray        *entries,
                                       gboolean          has_more)
{
    g_signal_emit (self, signals[SIGNAL_CHAT_HISTORY_BATCH], 0, htlc, cid,
                   entries, has_more);
}

void
gtkhx_session_emit_msg (GtkhxSession *self, HxMsgEvent *event)
{
    g_signal_emit (self, signals[SIGNAL_MSG], 0, event);
}

void
gtkhx_session_emit_agreement (GtkhxSession *self, session *sess,
                              const char *agreement, guint16 len)
{
    g_signal_emit (self, signals[SIGNAL_AGREEMENT], 0, sess, agreement,
                   (guint)len);
}

void
gtkhx_session_emit_news_file (GtkhxSession *self, struct htlc_conn *htlc,
                              const char *news, guint16 len)
{
    g_signal_emit (self, signals[SIGNAL_NEWS_FILE], 0, htlc, news, (guint)len);
}

void
gtkhx_session_emit_news_post (GtkhxSession *self, struct htlc_conn *htlc,
                              const char *news, guint16 len)
{
    g_signal_emit (self, signals[SIGNAL_NEWS_POST], 0, htlc, news, (guint)len);
}

void
gtkhx_session_emit_news_folder (GtkhxSession *self, struct gnews_folder *gfnews)
{
    g_signal_emit (self, signals[SIGNAL_NEWS_FOLDER], 0, gfnews);
}

void
gtkhx_session_emit_news_catalog (GtkhxSession *self,
                                 struct gnews_catalog *gcnews)
{
    g_signal_emit (self, signals[SIGNAL_NEWS_CATALOG], 0, gcnews);
}

void
gtkhx_session_emit_news_thread (GtkhxSession *self, struct news_post *post)
{
    g_signal_emit (self, signals[SIGNAL_NEWS_THREAD], 0, post);
}

void
gtkhx_session_emit_user_create (GtkhxSession *self, struct htlc_conn *htlc,
                                struct chat *chat, struct hx_user *user,
                                const char *nam, guint16 icon, guint16 color)
{
    g_signal_emit (self, signals[SIGNAL_USER_CREATE], 0, htlc, chat, user, nam,
                   (guint)icon, (guint)color);
}

void
gtkhx_session_emit_user_delete (GtkhxSession *self, struct htlc_conn *htlc,
                                struct chat *chat, struct hx_user *user)
{
    g_signal_emit (self, signals[SIGNAL_USER_DELETE], 0, htlc, chat, user);
}

void
gtkhx_session_emit_user_change (GtkhxSession *self, struct htlc_conn *htlc,
                                struct chat *chat, struct hx_user *user,
                                const char *nam, guint16 icon, guint16 color)
{
    g_signal_emit (self, signals[SIGNAL_USER_CHANGE], 0, htlc, chat, user, nam,
                   (guint)icon, (guint)color);
}

void
gtkhx_session_emit_users_clear (GtkhxSession *self, struct htlc_conn *htlc,
                                struct chat *chat)
{
    g_signal_emit (self, signals[SIGNAL_USERS_CLEAR], 0, htlc, chat);
}

void
gtkhx_session_emit_user_info (GtkhxSession *self, guint16 uid, const char *nam,
                              const char *info, guint16 len)
{
    g_signal_emit (self, signals[SIGNAL_USER_INFO], 0, (guint)uid, nam, info,
                   (guint)len);
}

void
gtkhx_session_emit_file_info (GtkhxSession *self, const char *path,
                              const char *name, const char *creator,
                              const char *type, const char *comments,
                              const char *modified, const char *created,
                              guint64 size)
{
    g_signal_emit (self, signals[SIGNAL_FILE_INFO], 0, path, name, creator,
                   type, comments, modified, created, size);
}

void
gtkhx_session_emit_file_list (GtkhxSession *self, struct cached_filelist *cfl,
                              struct hl_filelist_hdr *fh, void *data)
{
    g_signal_emit (self, signals[SIGNAL_FILE_LIST], 0, cfl, fh, data);
}

void
gtkhx_session_emit_file_update (GtkhxSession *self, session *sess,
                                struct htxf_conn *htxf)
{
    g_signal_emit (self, signals[SIGNAL_FILE_UPDATE], 0, sess, htxf);
}

void
gtkhx_session_emit_xfer_queue (GtkhxSession *self, session *sess,
                               struct htxf_conn *htxf)
{
    g_signal_emit (self, signals[SIGNAL_XFER_QUEUE], 0, sess, htxf);
}

void
gtkhx_session_emit_xfer_destroyed (GtkhxSession *self, session *sess,
                                   struct htxf_conn *htxf)
{
    g_signal_emit (self, signals[SIGNAL_XFER_DESTROYED], 0, sess, htxf);
}

void
gtkhx_session_emit_tracker_server_create (GtkhxSession *self,
                                          HxTrackerServer *event)
{
    g_signal_emit (self, signals[SIGNAL_TRACKER_SERVER_CREATE], 0, event);
}

void
gtkhx_session_emit_tracker_batch_begin (GtkhxSession *self,
                                        const char *tracker_url,
                                        guint8 version,
                                        guint16 expected_count)
{
    g_signal_emit (self, signals[SIGNAL_TRACKER_BATCH_BEGIN], 0,
                   tracker_url ? tracker_url : "",
                   version, (guint) expected_count);
}

void
gtkhx_session_emit_task_update (GtkhxSession *self, session *sess,
                                struct task *tsk)
{
    /* Phase R3.0 reference port: route the emit through the Rust
     * hxbridge crate. This is the canonical "drive a C-side signal
     * emit from Rust" example documented in
     * docs/rust/glib-interop.md. The wrapping uses from_glib_none
     * (one extra g_object_ref) so the session stays alive across
     * the synchronous emit even if a connected C handler runs
     * g_object_unref. Picked as the foothold because task-update
     * is a (G_TYPE_POINTER, G_TYPE_POINTER) signal with no boxed-
     * type marshaling — the boxed-type pattern is R4 work. */
    gtkhx_bridge_emit_pointer_pair_signal (self, "task-update", sess, tsk);
}

void
gtkhx_session_emit_chat_log_line (GtkhxSession *self, struct htlc_conn *htlc,
                                  guint32 cid, const char *body)
{
    g_signal_emit (self, signals[SIGNAL_CHAT_LOG_LINE], 0, htlc, cid, body);
}

void
gtkhx_session_emit_connection_state (GtkhxSession *self,
                                     GtkhxConnectionState state)
{
    g_signal_emit (self, signals[SIGNAL_CONNECTION_STATE], 0, (guint)state);
}
