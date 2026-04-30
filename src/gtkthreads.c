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
 * Phase 2.9: Threading first cut.
 *
 * The original gtkthreads.c approximated GDK's global lock with a custom
 * pipe + pthread_cond_t + pthread_mutex_t + gdk_input_add() integration —
 * because GTK+ 1.2 had no real threading support and the pipe trick was
 * how you marshaled wake-ups onto the GTK main loop.
 *
 * GTK 2 ships gdk_threads_enter() / gdk_threads_leave() (and the GDK
 * global lock that backs them), which is exactly the abstraction the
 * old code was reinventing. Replace the custom plumbing with thin wrappers
 * so the ~60 enter/leave call sites in network.c and xfers.c don't have
 * to change.
 *
 * gdk_threads_init() is called once from gtkhx.c init() before gtk_init().
 *
 * Note: gdk_threads_enter / leave are themselves deprecated in GTK 3.
 * Phase 3 will rip the lock out entirely and switch worker→UI marshaling
 * to g_main_context_invoke(). For now this is the minimal change that
 * gets us off the custom code without churn at every call site.
 */

#include "config.h"
#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include "gtkthreads.h"

void gtk_threads_init(void)
{
	/* The main thread holds the GDK lock for the lifetime of gtk_main();
	 * worker threads acquire it via gtk_threads_enter() before touching
	 * any GDK/GTK state. */
	gdk_threads_enter();
}

void gtk_threads_main(void)
{
	gtk_main();
	gdk_threads_leave();
}

void gtk_threads_enter(void)
{
	gdk_threads_enter();
}

void gtk_threads_leave(void)
{
	gdk_threads_leave();
}

void gtk_thread_exit(void)
{
	/* Nothing to clean up — the GDK lock manages itself. */
}
