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

/*
 * Phase 3.3: drop the deprecated gdk_threads_enter/leave + gdk_threads_init
 * pair (gone in GTK 4 entirely, deprecated since GTK 3.6) and reimplement
 * the same single-mutex semantics on top of GRecMutex + a custom main-loop
 * poll function.
 *
 * The behavior we faithfully preserve from the GDK 2 lock:
 *   - The main thread holds the lock for the lifetime of gtk_main(), and
 *     only releases it while blocked inside poll() waiting for events.
 *   - Worker threads (network.c, xfers.c) call gtk_threads_enter() before
 *     touching any GTK/GDK state and gtk_threads_leave() afterwards.
 *   - A worker thread's enter() will block until the main thread is in
 *     poll(), at which point it acquires the lock, mutates UI state, and
 *     releases — and the main thread reacquires when poll returns.
 *
 * Why not g_main_context_invoke() at every call site? The 56 enter/leave
 * brackets in network.c and xfers.c each compose multiple GTK calls with
 * captured locals, so a literal g_main_context_invoke() conversion would
 * require lifting each block into a dedicated marshaling struct +
 * callback. That refactor is real work and is deferred — this change just
 * gets us off the deprecated GDK lock without churn at every site.
 */

#include "config.h"
#include <gtk/gtk.h>
#include <glib.h>
#include "gtkthreads.h"

static GRecMutex gtkhx_main_lock;
static GPollFunc gtkhx_orig_poll = NULL;

/* Custom poll wrapper that releases the lock while blocking and
 * re-acquires it before returning. Called by GLib's main context every
 * iteration of the main loop. */
static gint
gtkhx_locked_poll(GPollFD *ufds, guint nfsd, gint timeout)
{
	gint ret;

	g_rec_mutex_unlock(&gtkhx_main_lock);
	ret = gtkhx_orig_poll ? gtkhx_orig_poll(ufds, nfsd, timeout)
	                      : g_poll(ufds, nfsd, timeout);
	g_rec_mutex_lock(&gtkhx_main_lock);

	return ret;
}

void gtk_threads_init(void)
{
	GMainContext *ctx = g_main_context_default();

	g_rec_mutex_init(&gtkhx_main_lock);
	g_rec_mutex_lock(&gtkhx_main_lock);

	gtkhx_orig_poll = g_main_context_get_poll_func(ctx);
	g_main_context_set_poll_func(ctx, gtkhx_locked_poll);
}

void gtk_threads_enter(void)
{
	g_rec_mutex_lock(&gtkhx_main_lock);
}

void gtk_threads_leave(void)
{
	g_rec_mutex_unlock(&gtkhx_main_lock);
}

void gtk_thread_exit(void)
{
	/* Nothing to clean up — the mutex outlives the process. */
}
