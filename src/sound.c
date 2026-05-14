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
#include <gsound.h>
#include "hx.h"
#include "gtkhx.h"
#include "sound.h"

struct hx_sounds hxsnd = { 0, 1, 1, 1, 1, 1, 1, 1, 1, 1 };

/* Phase 5: resolve a sound file by name across the layered sound
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

/* Phase 5: GSound (a thin GLib-style wrapper over libcanberra) is the
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

void
play_sound (int sound)
{
    if (!hxsnd.on) {
        g_debug ("play_sound: hxsnd.on is false, skipping sound %d", sound);
        return;
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
    }
}
