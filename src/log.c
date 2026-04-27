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


#warning XXX: Logging code unfinished
#if 0 /* XXX: this code isn't done and I don't have the time to work on it,
		 this is so the client doesn't log in its unfinished state in 0.9.4 */

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
	char *home = getenv("HOME");
	char *path1 = g_strdup_printf("%s/.hx", home);
	char *path2 = g_strdup_printf("%s/.hx/logs/", home);
	DIR *dir;

	log->filename = g_strdup_printf("%s/%s.log", path2, name);
	dir = opendir(path1);
	if(!dir) {
		mkdir(path1, S_IRUSR | S_IWUSR | S_IXUSR);
		mkdir(path2, S_IRUSR | S_IWUSR | S_IXUSR);
	}
	else {
		closedir(dir);
		dir = opendir(path2);
		if(!dir) {
			mkdir(path2, S_IRUSR | S_IWUSR | S_IXUSR);
		}
		else {
			closedir(dir);
		}
	}
	g_free(path1);
	g_free(path2);
	
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
