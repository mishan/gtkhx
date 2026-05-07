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
#include "hx.h"
#include "gtkhx.h"
#include "sound.h"

#ifdef HAVE_GSOUND
#include <gsound.h>
#endif

struct hx_sounds hxsnd =
{
	0,
	1, 1, 1, 1, 1, 1, 1, 1, 1
};

/* Phase 5: resolve a sound file by name across the layered sound
 * search path:
 *   1. $CONFIG/sounds/<name>        — per-user drop-ins
 *   2. $PREFIX/share/gtkhx/sounds/  — distro / system default
 *
 * The legacy SOUNDPATH cfgvar (an explicit single-directory fallback)
 * was retired with the path-pref cleanup. Drop sound files into
 * $CONFIG/sounds/ instead.
 *
 * Returns the first existing path, or NULL if none was found.
 * Caller g_frees. */
static char *
sound_resolve (const char *name)
{
	char *candidate;

	candidate = g_build_filename (gtkhx_config_dir (), "sounds", name, NULL);
	if (g_file_test (candidate, G_FILE_TEST_EXISTS))
		return candidate;
	g_free (candidate);

	candidate = g_build_filename (PREFIX "/share/gtkhx/sounds", name, NULL);
	if (g_file_test (candidate, G_FILE_TEST_EXISTS))
		return candidate;
	g_free (candidate);

	return NULL;
}

#ifdef HAVE_GSOUND

/* Phase 5: replaced the fork+execlp("play", ...) path with GSound, a
 * tiny GLib-style wrapper over libcanberra. play_simple_async
 * decodes + mixes the .aiff in-process and hands it to PulseAudio /
 * PipeWire / ALSA without spawning a player; the call returns
 * immediately so the caller (often a worker thread completing a file
 * transfer) doesn't pay fork-in-multithreaded-process latency or
 * leave zombie children behind. We never look at the result — for
 * notification sounds, dropping a play on the floor is fine. */

static GSoundContext *gtkhx_gsound_ctx = NULL;

/* Initialised on first call; caller must hold the GTK thread lock or
 * be on the main thread. GSoundContext is thread-safe per its own
 * docs (libcanberra's pulse backend handles its own locking) but
 * lazy-init on first call is simpler than a g_once_init pattern and
 * fine for a tool whose first sound plays after the user has done
 * something that ran on the main loop. */
static GSoundContext *
gtkhx_gsound_get (void)
{
	GError *err = NULL;

	if (gtkhx_gsound_ctx)
		return gtkhx_gsound_ctx;

	gtkhx_gsound_ctx = gsound_context_new (NULL, &err);
	if (!gtkhx_gsound_ctx) {
		g_warning ("gsound_context_new failed: %s",
		           err ? err->message : "(no detail)");
		g_clear_error (&err);
		return NULL;
	}

	return gtkhx_gsound_ctx;
}

void play (char *name)
{
	GSoundContext *ctx;
	char          *path;

	path = sound_resolve (name);
	if (!path)
		return;

	ctx = gtkhx_gsound_get ();
	if (!ctx) {
		g_free (path);
		return;
	}

	/* Fire and forget. NULL callback means "we don't care about the
	 * result" — GSound will still log a warning on its own bus if
	 * playback fails for some reason (no audio server, file not
	 * decodable, etc.). */
	gsound_context_play_simple (ctx, NULL, NULL,
	                            GSOUND_ATTR_MEDIA_FILENAME, path,
	                            GSOUND_ATTR_MEDIA_ROLE,     "event",
	                            NULL);
	g_free (path);
}

#else /* !HAVE_GSOUND — legacy fork+execlp fallback */

void play (char *name)
{
	pid_t pid;
	char *arg = sound_resolve (name);

	/* If we couldn't find the sound anywhere, don't bother forking.
	 * Avoids spawning a player just to have it fail with ENOENT. */
	if (!arg)
		return;

	pid = fork();
	if(pid == -1) {
		g_free (arg);
		return;
	}

	if(pid == 0) {
		execlp(gtkhx_prefs.snd_cmd, gtkhx_prefs.snd_cmd, arg, NULL);
		_exit(127);
	}

	g_free (arg);
}

#endif /* HAVE_GSOUND */

void play_sound(int sound)
{
	if(!hxsnd.on) {
		return;
	}

	switch(sound) {
	case CHAT_INVITE:
		if (hxsnd.invite)
			play("chatinvite.aiff");
		break;
	case CHAT_POST:
		if (hxsnd.chat)
			play("chatpost.aiff");
		break;
	case ERROR:
		if (hxsnd.error)
			play("error.aiff");
		break;
	case FILE_DONE:
		if (hxsnd.file)
			play("filedone.aiff");
		break;
	case USER_JOIN:
		if (hxsnd.join)
			play("join.aiff");
		break;
	case LOGIN:
		if (hxsnd.login)
			play("logged-in.aiff");
		break;
	case MSG:
		if(hxsnd.msg)

			play("message.aiff");

		break;
	case NEWS_POST:
		if(hxsnd.news)
			play("newspost.aiff");
		break;
	case USER_PART:
		if(hxsnd.part)
			play("part.aiff");
		break;
	}

}

