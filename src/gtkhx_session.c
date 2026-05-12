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

struct _GtkhxSession {
	GObject parent_instance;
};

G_DEFINE_FINAL_TYPE (GtkhxSession, gtkhx_session, G_TYPE_OBJECT)

enum {
	SIGNAL_CHAT,
	SIGNAL_CHAT_SUBJECT,
	SIGNAL_CHAT_INVITATION,
	SIGNAL_MSG,
	SIGNAL_LAST
};

static guint signals[SIGNAL_LAST];

static void
gtkhx_session_init (GtkhxSession *self)
{
	(void) self;
}

static void
gtkhx_session_class_init (GtkhxSessionClass *klass)
{
	/* "chat" — incoming chat-message body on a chat-id. Replaces
	 * hx_output.chat. Matches the vtable signature so the existing
	 * output_chat handler in chat.c connects without an adapter. */
	signals[SIGNAL_CHAT] = g_signal_new (
		"chat",
		G_TYPE_FROM_CLASS (klass),
		G_SIGNAL_RUN_LAST,
		0,                       /* class_offset */
		NULL, NULL,              /* accumulator + data */
		NULL,                    /* marshaller: NULL → libffi-derived */
		G_TYPE_NONE,
		4,
		G_TYPE_POINTER,          /* struct htlc_conn * */
		G_TYPE_UINT,             /* cid */
		G_TYPE_POINTER,          /* body */
		G_TYPE_UINT);            /* len (guint16 widened) */

	/* "chat-subject" — subject changed in chat `cid`. Replaces
	 * hx_output.chat_subject. The arg list is (htlc, cid,
	 * subject-string). */
	signals[SIGNAL_CHAT_SUBJECT] = g_signal_new (
		"chat-subject",
		G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
		NULL, NULL, NULL,
		G_TYPE_NONE, 3,
		G_TYPE_POINTER, G_TYPE_UINT, G_TYPE_POINTER);

	/* "chat-invitation" — server invited us to chat `cid`. Replaces
	 * hx_output.chat_invitation. `name` is the inviter's display
	 * name (server-provided, already sanitised by the rcv path). */
	signals[SIGNAL_CHAT_INVITATION] = g_signal_new (
		"chat-invitation",
		G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
		NULL, NULL, NULL,
		G_TYPE_NONE, 3,
		G_TYPE_POINTER, G_TYPE_UINT, G_TYPE_POINTER);

	/* "msg" — incoming private message. Replaces hx_output.msg.
	 * No htlc arg (the original vtable signature was already
	 * htlc-less); body is NUL-terminated. */
	signals[SIGNAL_MSG] = g_signal_new (
		"msg",
		G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
		NULL, NULL, NULL,
		G_TYPE_NONE, 3,
		G_TYPE_POINTER,          /* sender name */
		G_TYPE_UINT,             /* uid (guint16 widened) */
		G_TYPE_POINTER);         /* body */
}

GtkhxSession *
gtkhx_session_get_default (void)
{
	static GtkhxSession *singleton = NULL;
	if (G_UNLIKELY (singleton == NULL))
		singleton = g_object_new (GTKHX_TYPE_SESSION, NULL);
	return singleton;
}

void
gtkhx_session_emit_chat (GtkhxSession *self,
                         struct htlc_conn *htlc,
                         guint32 cid, const char *body, guint16 len)
{
	g_signal_emit (self, signals[SIGNAL_CHAT], 0,
	               htlc, cid, body, (guint) len);
}

void
gtkhx_session_emit_chat_subject (GtkhxSession *self,
                                 struct htlc_conn *htlc,
                                 guint32 cid, const char *subj)
{
	g_signal_emit (self, signals[SIGNAL_CHAT_SUBJECT], 0,
	               htlc, cid, subj);
}

void
gtkhx_session_emit_chat_invitation (GtkhxSession *self,
                                    struct htlc_conn *htlc,
                                    guint32 cid, const char *name)
{
	g_signal_emit (self, signals[SIGNAL_CHAT_INVITATION], 0,
	               htlc, cid, name);
}

void
gtkhx_session_emit_msg (GtkhxSession *self,
                        const char *name, guint16 uid, const char *body)
{
	g_signal_emit (self, signals[SIGNAL_MSG], 0,
	               name, (guint) uid, body);
}
