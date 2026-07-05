/*
 * sound_events.h — event-driven sound-effect dispatch.
 *
 * A thin view-side subscriber that connects to GtkhxSession (and the
 * per-session HxVoiceModel) and plays the matching alert sound for each
 * event. This is the sound analogue of notify.c: the model side
 * (rcv.c / network.c / hxvoice-model) just emits signals; it never calls
 * play_sound directly. The mapping lives here so the "what event plays
 * what sound" policy is in one place.
 *
 * Signal → sound:
 *   "chat"                 → CHAT_POST
 *   "msg"                  → MSG   (broadcasts play MSG from msg.c, which
 *                                   has no "msg" signal — see broadcastmsg)
 *   "news-post"            → NEWS_POST
 *   "chat-invitation"      → CHAT_INVITE
 *   "user-create"          → USER_JOIN  (only when incremental == TRUE)
 *   "user-delete"          → USER_PART  (only when incremental == TRUE)
 *   "logged-in"            → LOGIN
 *   voice "voice-presence-chime" → VOICE_JOIN / VOICE_LEAVE
 *
 * play_sound() itself (sound.c) is thread-safe (it marshals to main via
 * g_idle_add), but every signal here is emitted on the main thread
 * anyway.
 *
 * Threading: call gtkhx_sound_events_init once from fe_init, after the
 * session's voice_model is created and the other signal handlers are
 * connected.
 */

#ifndef HX_SOUND_EVENTS_H
#define HX_SOUND_EVENTS_H

#include <glib-object.h>
#include "gtkhx_session.h"

/* Connect the sound handlers to `emitter` (the GtkhxSession singleton)
 * and `voice_model` (the per-session HxVoiceModel, passed as gpointer so
 * this header stays independent of the HAVE_VOICE-gated voice_model.h).
 * `voice_model` may be NULL — in a no-voice build, or before the model
 * exists — in which case only the GtkhxSession-driven chimes are wired. */
extern void gtkhx_sound_events_init (GtkhxSession *emitter,
                                     gpointer voice_model);

#endif /* HX_SOUND_EVENTS_H */
