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
 * Worker → main thread bridges. See gtkthreads.h for the design
 * rationale.
 */

#include "config.h"
#include <glib.h>
#include "gtkthreads.h"

void gtkhx_post_to_main (GSourceFunc fn, gpointer data)
{
	g_main_context_invoke (NULL, fn, data);
}

/* Synchronous worker → main bridge. The condition variable + done
 * flag are the standard idiom for "wait until another thread runs
 * my callback." A small trampoline wraps the user's callback so
 * the user's GSourceFunc keeps its normal contract (called with
 * the user's data, returns G_SOURCE_REMOVE) — the trampoline
 * ignores the return value, sets done, and signals.
 *
 * If we're already on the main thread, just run inline. Otherwise
 * post the trampoline and wait. */
struct gtkhx_sync_call {
	GSourceFunc  fn;
	gpointer     data;
	GMutex       mu;
	GCond        cond;
	gboolean     done;
};

static gboolean
gtkhx_sync_trampoline (gpointer p)
{
	struct gtkhx_sync_call *c = p;
	c->fn (c->data);
	g_mutex_lock (&c->mu);
	c->done = TRUE;
	g_cond_signal (&c->cond);
	g_mutex_unlock (&c->mu);
	return G_SOURCE_REMOVE;
}

void gtkhx_invoke_sync (GSourceFunc fn, gpointer data)
{
	GMainContext *ctx = g_main_context_default ();
	struct gtkhx_sync_call c;

	if (g_main_context_is_owner (ctx)) {
		fn (data);
		return;
	}

	c.fn = fn;
	c.data = data;
	g_mutex_init (&c.mu);
	g_cond_init (&c.cond);
	c.done = FALSE;

	g_main_context_invoke (ctx, gtkhx_sync_trampoline, &c);

	g_mutex_lock (&c.mu);
	while (!c.done)
		g_cond_wait (&c.cond, &c.mu);
	g_mutex_unlock (&c.mu);

	g_cond_clear (&c.cond);
	g_mutex_clear (&c.mu);
}
