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

#ifndef GTKTHREADS_H
#define GTKTHREADS_H

#include <glib.h>

/*
 * Worker → main thread bridges.
 *
 * GTK 4 requires same-thread access to widgets, GtkListStore,
 * signal-emit reentrancy, and the resize queue. A user-space mutex
 * does not restore the old GTK 2/3 threading contract — cross-thread
 * mutation under a mutex still corrupts state that the main thread
 * later walks during a layout/signal pass, with the crash landing
 * on the main thread well after the offending worker call.
 *
 * Workers therefore do socket I/O, parsing, and pure data
 * transformations only. Anything that touches GTK state or the
 * connection's send buffer (htlc->out, htlc->trans, the cipher /
 * compress encoders) goes through one of the two helpers below.
 */

/*
 * Asynchronous post: queue `fn(data)` on the default main context
 * and return immediately. The callback runs on the main thread,
 * owns `data`, and must free it before returning. Return
 * G_SOURCE_REMOVE so the idle is one-shot.
 *
 * Fire-and-forget — the worker can keep streaming jobs faster than
 * the main thread drains them. Don't post unbounded volume from a
 * tight worker loop without thinking about pile-up.
 */
extern void gtkhx_post_to_main (GSourceFunc fn, gpointer data);

#endif
