/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 * Public License for more details.
 */

/*
 * sound_events.c — see sound_events.h for the design overview.
 *
 * Each handler is a one-liner: map the signal to a play_sound() id.
 * play_sound (sound.c) does the pref gate (hxsnd.*) and the actual
 * GSound playback, so there's no policy here beyond "which event plays
 * which sound". Handlers run on the main thread (every signal is
 * emitted there), and play_sound is safe from any thread regardless.
 */

#include "config.h"
#include <gtk/gtk.h>
#include "sound.h"
#include "sound_events.h"
#ifdef HAVE_VOICE
#include "voice_model.h"
#endif

/* ---- GtkhxSession handlers ----------------------------------------- */

static void
sound_on_chat (GtkhxSession *emitter, struct htlc_conn *htlc, gpointer event,
               gpointer user_data)
{
    (void)emitter;
    (void)htlc;
    (void)event;
    (void)user_data;
    play_sound (CHAT_POST);
}

static void
sound_on_msg (GtkhxSession *emitter, gpointer event, gpointer user_data)
{
    (void)emitter;
    (void)event;
    (void)user_data;
    play_sound (MSG);
}

static void
sound_on_news_post (GtkhxSession *emitter, struct htlc_conn *htlc,
                    gpointer news, guint len, gpointer user_data)
{
    (void)emitter;
    (void)htlc;
    (void)news;
    (void)len;
    (void)user_data;
    play_sound (NEWS_POST);
}

static void
sound_on_chat_invitation (GtkhxSession *emitter, struct htlc_conn *htlc,
                          guint cid, gpointer name, gpointer user_data)
{
    (void)emitter;
    (void)htlc;
    (void)cid;
    (void)name;
    (void)user_data;
    play_sound (CHAT_INVITE);
}

static void
sound_on_user_create (GtkhxSession *emitter, struct htlc_conn *htlc,
                      gpointer chat, gpointer user, gpointer nam, guint icon,
                      guint color, gboolean incremental, gpointer user_data)
{
    (void)emitter;
    (void)htlc;
    (void)chat;
    (void)user;
    (void)nam;
    (void)icon;
    (void)color;
    (void)user_data;
    /* Only a genuine join broadcast chimes — the bulk user-list load
     * emits with incremental == FALSE so we stay silent on login. */
    if (incremental) {
        play_sound (USER_JOIN);
    }
}

static void
sound_on_user_delete (GtkhxSession *emitter, struct htlc_conn *htlc,
                      gpointer chat, gpointer user, gboolean incremental,
                      gpointer user_data)
{
    (void)emitter;
    (void)htlc;
    (void)chat;
    (void)user;
    (void)user_data;
    if (incremental) {
        play_sound (USER_PART);
    }
}

static void
sound_on_logged_in (GtkhxSession *emitter, struct htlc_conn *htlc,
                    gpointer user_data)
{
    (void)emitter;
    (void)htlc;
    (void)user_data;
    play_sound (LOGIN);
}

/* ---- HxVoiceModel handler ------------------------------------------ */

#ifdef HAVE_VOICE
static void
sound_on_voice_presence_chime (HxVoiceModel *model, guint uid, gboolean joined,
                               gpointer user_data)
{
    (void)model;
    (void)uid;
    (void)user_data;
    play_sound (joined ? VOICE_JOIN : VOICE_LEAVE);
}
#endif /* HAVE_VOICE */

/* ---- Public API ---------------------------------------------------- */

void
gtkhx_sound_events_init (GtkhxSession *emitter, gpointer voice_model)
{
    g_return_if_fail (emitter != NULL);

    g_signal_connect (emitter, "chat", G_CALLBACK (sound_on_chat), NULL);
    g_signal_connect (emitter, "msg", G_CALLBACK (sound_on_msg), NULL);
    g_signal_connect (emitter, "news-post", G_CALLBACK (sound_on_news_post),
                      NULL);
    g_signal_connect (emitter, "chat-invitation",
                      G_CALLBACK (sound_on_chat_invitation), NULL);
    g_signal_connect (emitter, "user-create",
                      G_CALLBACK (sound_on_user_create), NULL);
    g_signal_connect (emitter, "user-delete",
                      G_CALLBACK (sound_on_user_delete), NULL);
    g_signal_connect (emitter, "logged-in", G_CALLBACK (sound_on_logged_in),
                      NULL);

#ifdef HAVE_VOICE
    if (voice_model) {
        g_signal_connect (voice_model, "voice-presence-chime",
                          G_CALLBACK (sound_on_voice_presence_chime), NULL);
    }
#else
    (void)voice_model;
#endif
}
