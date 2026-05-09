/*
 * Copyright (C) 2000-2002 Misha Nasledov <misha@nasledov.com>
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


/* This whole file is gated off behind #if 0 — the chat-logging code
 * was never finished. The follow-up is tracked in TODO; revisit when
 * we wire log writing into chat.c / news.c / msg.c output paths. */
#if 0

#include "config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <gtk/gtk.h>
#include <sys/time.h>
#include <time.h>
#include <netinet/in.h>
#include <dirent.h>
#include <fcntl.h>

#include "hx.h"
#include "network.h"
#include "gtkutil.h"
#include "gtkhx.h"
#include "gtk_hlist.h"
#include "tasks.h"
#include "rcv.h"
#include "files.h"
#include "log.h"

static struct log *loglist = NULL;

struct log *create_log(char *name)
{
	struct log *log = malloc(sizeof(struct log));
	/* Phase 5: logs live under $CONFIG/logs/. Drop the legacy
	 * ~/.hx/logs/ path entirely — the rest of this TU is wrapped in
	 * an #if 0 (logging is the TODO Features 'Log chat history /
	 * server' bullet, never finished), so there's no on-disk legacy
	 * to preserve. */
	char *path = g_build_filename (gtkhx_config_dir (), "logs", NULL);

	log->filename = g_strdup_printf ("%s/%s.log", path, name);
	g_mkdir_with_parents (path, S_IRUSR | S_IWUSR | S_IXUSR);
	g_free (path);

	log->fd = open(log->filename, O_WRONLY|O_APPEND|O_CREAT, S_IRUSR | S_IWUSR);

	log->next = NULL;
	log->prev = loglist;
	if(loglist) loglist->next = log;

	return log;
}

void print_log(struct log *log, char *buf)
{
	write(log->fd, buf, strlen(buf));
	fsync(log->fd);
}

void close_log(struct log *log)
{
	if(!log) {
		return;
	}
	
	if (log->next)
		log->next->prev = log->prev;
	if (log->prev)
		log->prev->next = log->next;
	if (log == loglist)
		loglist = log->prev;

	g_free(log->filename);
	close(log->fd);
}

void close_logs(void)
{
	struct log *log = loglist, *prev;

	for(log = loglist; log; log = prev) {
		prev = log->prev;
		close_log(log);
	}
}

#endif
