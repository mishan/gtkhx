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

struct hx_sounds hxsnd =
{
	0,
	1, 1, 1, 1, 1, 1, 1, 1, 1
};

/* Phase 5: resolve a sound file by name across the layered sound
 * search path:
 *   1. $CONFIG/sounds/<name>          — per-user drop-ins
 *   2. gtkhx_prefs.sound_path/<name>  — legacy single-path config
 *   3. $PREFIX/share/gtkhx/sounds/    — distro / system default
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

	if (gtkhx_prefs.sound_path && *gtkhx_prefs.sound_path) {
		candidate = g_build_filename (gtkhx_prefs.sound_path, name, NULL);
		if (g_file_test (candidate, G_FILE_TEST_EXISTS))
			return candidate;
		g_free (candidate);
	}

	candidate = g_build_filename (PREFIX "/share/gtkhx/sounds", name, NULL);
	if (g_file_test (candidate, G_FILE_TEST_EXISTS))
		return candidate;
	g_free (candidate);

	return NULL;
}

void play(char *name)
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

