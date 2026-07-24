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
#include <stdlib.h>
#include <gtk/gtk.h>
#include "hx.h"
#include "gtkhx.h"
#include "sound.h"

/* Playback backend: the cross-platform Rust `hxsound` crate
 * (rust/crates/hxsound). Plays the WAV at `path` fire-and-forget via
 * rodio/cpal (ALSA on Linux, CoreAudio on macOS, WASAPI on Windows),
 * replacing the Linux-only GSound/libcanberra path. Thread-safe, and a no-op
 * when no audio device is available — so this file no longer needs a
 * HAVE_GSOUND gate or a silent-stub fallback. */
extern void hx_sound_play (const char *path);

/* The alert-sound on/off preferences. options.c binds these settings. */
struct hx_sounds hxsnd = { 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };

/* Resolve a sound file by name across the layered sound search path:
 *   1. $CONFIG/sounds/<name>              — per-user drop-ins
 *   2. <system-data-dir>/gtkhx/sounds/    — every g_get_system_data_dirs() entry
 *   3. <module-dir>/share/gtkhx/sounds/   — Windows: relative to the running exe
 *   4. $PREFIX/share/gtkhx/sounds/        — compiled-in install prefix (Unix)
 *
 * Step 2 is what makes relocatable bundles work without a hardcoded prefix: the
 * macOS .app launcher points XDG_DATA_DIRS at its Resources, and GLib derives
 * the data dirs from the install location on Windows. Step 3 is a Windows
 * belt-and-braces path from the exe itself; step 4 keeps a normal `meson
 * install` on Unix working. Returns the first existing path, or NULL. Caller
 * g_frees. */
static char *
sound_resolve (const char *name)
{
    char *candidate;

    candidate = g_build_filename (gtkhx_config_dir (), "sounds", name, NULL);
    if (g_file_test (candidate, G_FILE_TEST_EXISTS)) {
        return candidate;
    }
    g_free (candidate);

    for (const gchar * const *d = g_get_system_data_dirs (); d && *d; d++) {
        candidate = g_build_filename (*d, "gtkhx", "sounds", name, NULL);
        if (g_file_test (candidate, G_FILE_TEST_EXISTS)) {
            return candidate;
        }
        g_free (candidate);
    }

#ifdef G_OS_WIN32
    {
        /* …\bin\gtkhx.exe -> install root; find <root>\share\gtkhx\sounds. */
        char *root = g_win32_get_package_installation_directory_of_module (NULL);
        if (root) {
            candidate = g_build_filename (root, "share", "gtkhx", "sounds",
                                          name, NULL);
            g_free (root);
            if (g_file_test (candidate, G_FILE_TEST_EXISTS)) {
                return candidate;
            }
            g_free (candidate);
        }
    }
#endif

    candidate = g_build_filename (PREFIX, "share", "gtkhx", "sounds", name, NULL);
    if (g_file_test (candidate, G_FILE_TEST_EXISTS)) {
        return candidate;
    }
    g_free (candidate);

    return NULL;
}

static void
play (const char *name)
{
    char *path = sound_resolve (name);

    if (!path) {
        g_message ("sound: '%s' not found on the sound search path; skipping",
                   name);
        return;
    }

    /* Fire-and-forget: hxsound copies the path onto its audio thread. */
    hx_sound_play (path);
    g_free (path);
}

/* Dispatch the play() side of play_sound on the main thread. sound_resolve
 * touches GLib config state (gtkhx_config_dir), so keep it single-threaded;
 * the Rust hx_sound_play call it makes is itself thread-safe, but marshalling
 * here keeps the whole path uniform and lets worker-thread callers (xfers.c)
 * in without touching GLib off the main loop. */
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
    /* Safe-from-any-thread entry point. Most callers (rcv.c, tasks.c) are on
     * the main thread; xfers.c's FILE_DONE hits run on the file-transfer
     * worker threads. g_idle_add is thread-safe at the call site and the
     * callback runs on the main loop's owner thread. */
    g_idle_add (play_sound_idle_cb, GINT_TO_POINTER (sound));
}
