/*
 * Copyright (C) 2001 Misha Nasledov <misha@nasledov.com>
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
 *
 * You should have received a copy of the GNU General
 * Public License along with this program; if not, write to the
 * Free Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "config.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <gtk/gtk.h>
#include <netinet/in.h>
#ifdef HAVE_GSOUND
#include <gsound.h>
#endif
#include "hx.h"
#include "gtkhx.h"
#include "sound.h"

/* The alert-sound on/off preferences. Always present — options.c binds these
 * settings regardless of whether a playback backend was compiled in; when
 * HAVE_GSOUND is unset the toggles simply have no audible effect. */
struct hx_sounds hxsnd = { 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };

#ifdef HAVE_GSOUND

/* resolve a sound file by name across the layered sound
 * search path:
 *   1. $CONFIG/sounds/<name>        — per-user drop-ins
 *   2. $PREFIX/share/gtkhx/sounds/  — distro / system default
 *
 * Returns the first existing path, or NULL if none was found.
 * Caller g_frees. */
static char *
sound_resolve (const char *name)
{
    char *candidate;

    candidate = g_build_filename (gtkhx_config_dir (), "sounds", name, NULL);
    if (g_file_test (candidate, G_FILE_TEST_EXISTS)) {
        return candidate;
    }
    g_free (candidate);

    candidate = g_build_filename (PREFIX "/share/gtkhx/sounds", name, NULL);
    if (g_file_test (candidate, G_FILE_TEST_EXISTS)) {
        return candidate;
    }
    g_free (candidate);

    return NULL;
}

/* GSound (a thin GLib-style wrapper over libcanberra) is the
 * one and only playback path. Fire-and-forget, in-process, async — no
 * fork, no execlp, no zombie children, no waitpid. The "snd_cmd"
 * preference and the legacy fork+exec fallback have been retired;
 * libcanberra speaks PulseAudio / PipeWire / ALSA directly via its
 * usual backend autodetection. */

static GSoundContext *gtkhx_gsound_ctx = NULL;

static GSoundContext *
gtkhx_gsound_get (void)
{
    GError *err = NULL;

    if (gtkhx_gsound_ctx) {
        return gtkhx_gsound_ctx;
    }

    gtkhx_gsound_ctx = gsound_context_new (NULL, &err);
    if (!gtkhx_gsound_ctx) {
        g_warning ("gsound_context_new failed: %s",
                   err ? err->message : "(no detail)");
        g_clear_error (&err);
        return NULL;
    }

    return gtkhx_gsound_ctx;
}

static void
gtkhx_gsound_play_done (GObject *source, GAsyncResult *result, gpointer data)
{
    GError *err = NULL;
    char *path = data;

    if (!gsound_context_play_full_finish (GSOUND_CONTEXT (source), result,
                                          &err)) {
        /* G_IO_ERROR_CANCELLED is the only failure we don't want to
		 * surface — that's the "user already triggered another sound
		 * or the context is being torn down" path. Everything else
		 * (file not found, decoder unavailable, audio server
		 * unreachable) the user wants to know about; otherwise sounds
		 * just silently never work and the failure mode is
		 * mysterious. */
        if (!g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_warning ("gsound: failed to play %s: %s", path ? path : "(null)",
                       err ? err->message : "(no detail)");
        }
        g_clear_error (&err);
    }
    g_free (path);
}

static void
play (char *name)
{
    GSoundContext *ctx;
    char *path;

    path = sound_resolve (name);
    if (!path) {
        g_message ("sound: '%s' not found in $CONFIG/sounds or " PREFIX
                   "/share/gtkhx/sounds; skipping",
                   name);
        return;
    }

    ctx = gtkhx_gsound_get ();
    if (!ctx) {
        g_free (path);
        return;
    }

    /* play_full so we collect errors via the callback. The callback
	 * owns `path` (transferred via user_data) and frees it. */
    gsound_context_play_full (ctx, NULL, gtkhx_gsound_play_done, path,
                              GSOUND_ATTR_MEDIA_FILENAME, path,
                              GSOUND_ATTR_MEDIA_ROLE, "event", NULL);
}

/* Dispatch the play() side of play_sound on the main thread. All
 * GSound and GLib state — the lazy-initialised gtkhx_gsound_ctx
 * singleton in gtkhx_gsound_get, the play_full async machinery, the
 * play_done callback that frees `path` — is single-threaded by
 * design. Without this marshal, two file-transfer workers finishing
 * within microseconds of each other both raced into gtkhx_gsound_get
 * (helgrind flagged the play↔gtkhx_gsound_get conflict), and even
 * after init the play_full call isn't documented thread-safe. */
static gboolean
play_sound_idle_cb (gpointer data)
{
    int sound = GPOINTER_TO_INT (data);

    if (!hxsnd.on) {
        g_debug ("play_sound: hxsnd.on is false, skipping sound %d", sound);
        return G_SOURCE_REMOVE;
    }

    switch (sound) {
    case CHAT_INVITE:
        if (hxsnd.invite) {
            play ("chatinvite.wav");
        }
        break;
    case CHAT_POST:
        if (hxsnd.chat) {
            play ("chatpost.wav");
        }
        break;
    case ERROR:
        if (hxsnd.error) {
            play ("error.wav");
        }
        break;
    case FILE_DONE:
        if (hxsnd.file) {
            play ("filedone.wav");
        }
        break;
    case USER_JOIN:
        if (hxsnd.join) {
            play ("join.wav");
        }
        break;
    case LOGIN:
        if (hxsnd.login) {
            play ("logged-in.wav");
        }
        break;
    case MSG:
        if (hxsnd.msg) {
            play ("message.wav");
        }

        break;
    case NEWS_POST:
        if (hxsnd.news) {
            play ("newspost.wav");
        }
        break;
    case USER_PART:
        if (hxsnd.part) {
            play ("part.wav");
        }
        break;
    case VOICE_JOIN:
        if (hxsnd.voice_join) {
            play ("voice_join.wav");
        }
        break;
    case VOICE_LEAVE:
        if (hxsnd.voice_leave) {
            play ("voice_leave.wav");
        }
        break;
    }

    return G_SOURCE_REMOVE;
}

void
play_sound (int sound)
{
    /* Safe-from-any-thread entry point. Most callers (rcv.c, tasks.c)
     * are on the main thread already; xfers.c's FILE_DONE hits run on
     * the file-transfer worker threads (get_thread / put_thread /
     * folder_get_thread / folder_put_thread) and these MUST marshal
     * to avoid the GSound/GLib races above.
     *
     * g_idle_add is thread-safe at the call site and the callback
     * runs on the main loop's owner thread, which is where every
     * other GTK/GLib call in this process already runs. */
    g_idle_add (play_sound_idle_cb, GINT_TO_POINTER (sound));
}

#else /* !HAVE_GSOUND */

/* No playback backend on this platform (GSound/libcanberra has no Homebrew
 * or MSYS2 package). Keep the public entry point so every caller in rcv.c /
 * tasks.c / xfers.c links unchanged; it just does nothing. */
void
play_sound (int sound)
{
    (void) sound;
}

#endif /* HAVE_GSOUND */
