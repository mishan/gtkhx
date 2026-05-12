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

/* Phase 5+ async connect (network.c): gtkhx_invoke_sync used to live
 * here to let hx_thread_connect bracket short bits of main-thread
 * work synchronously from inside its worker. The connect path is
 * pure-async on the main loop now, so nothing needs sync-invoke.
 * Tracker fetch is still on a worker but only uses the async
 * gtkhx_post_to_main path above. */
