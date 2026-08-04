/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * session_registry.h — the collection of connections, and which one has focus.
 *
 * See session_registry.c for why the collection and the focus are separate
 * ideas. The short version, because it decides which function you want:
 *
 *   - Model code (a receive handler, a task reply, anything that arrived on
 *     the wire) already holds the connection the event belongs to. It uses
 *     `sess_from_htlc()` and never touches this header.
 *   - View code (a button, a menu item, a dialog) acts on the connection the
 *     user is looking at, which is `hx_active_session()`.
 *
 * Getting that backwards is how a background server's event ends up mutating
 * the focused server's widgets, which is the bug class the whole
 * multi-connection effort is unwinding.
 */

#ifndef HX_SESSION_REGISTRY_H
#define HX_SESSION_REGISTRY_H

#include <glib.h>

#include "session.h"

G_BEGIN_DECLS

/* Returned by hx_session_index for a session the registry doesn't hold. */
#define HX_SESSION_NOT_FOUND G_MAXUINT

/* Build a session, with its connection and its per-session collections, and
 * add it to the collection. Never NULL.
 *
 * This is the whole of what is per-connection. Anything that must happen once
 * for the process — the signal wiring, the sound subscriber, the toolbar
 * window — belongs to startup, not here, and running it per session would be
 * wrong rather than merely wasteful. */
session *hx_session_new (void);

/* Open a connection: build a session, its model-side state, its content page
 * in every per-connection panel, and its tab — and select that tab, so the new
 * connection is the one the user is looking at.
 *
 * `hx_session_new` above is the model half of this; everything a *usable*
 * connection additionally needs is here. The split matters because startup
 * wants them separately: `fe_init` builds the first session before the
 * preferences are read (a change hook needs a connection) and its panels well
 * after (they need the toolbar window). Every connection after the first
 * wants both at once.
 *
 * Lives in gtkhx.c rather than session_registry.c, because it names the whole
 * window layer — the panel factories and the toolbar window — and the registry
 * deliberately names none of it. Never NULL. */
session *hx_session_open (const char *title);

/* Close a connection: disconnect it, destroy its content page in every
 * per-connection panel, and drop it from the collection.
 *
 * The inverse of hx_session_open, and the same reason it lives in gtkhx.c: it
 * names the panel layer. The caller moves the focus off it first — the tab
 * strip does, by selecting a neighbour before closing.
 *
 * Frees the session on the way out, through hx_session_remove. */
void hx_session_close (session *sess);

/* The session the user is looking at, or NULL before the first one exists.
 *
 * It used to be impossible for this to be NULL, and most callers still don't
 * check. That was safe when it returned the address of a static; it is
 * *still* safe in practice because startup creates a session before any UI
 * can run, but new code should not rely on it. */
session *hx_active_session (void);

/* The live session on the connection with this serial, or NULL when there is
 * none — because it was closed, or because the serial was never a connection's.
 *
 * The safe way to *keep* a reference to a session. A serial is a plain number
 * that outlives the thing it names without becoming a dangling pointer, so
 * anything holding on across a turn of the main loop — the tab strip's index,
 * the voice token, a dialog waiting on an answer — stores one of these and
 * asks here, rather than storing a `session *` that nothing can invalidate.
 *
 * Serial 0 means "no connection" and always answers NULL. */
session *hx_session_with_serial (guint16 serial);

/* The live connection with this serial, or NULL when there is none.
 *
 * The connection-side form of hx_session_with_serial, and the safe way for
 * anything asynchronous to name a connection: a callback queued on the main
 * loop can be dispatched after the close that provoked it, and the connection
 * struct is freed with its session. A serial resolves to NULL from that moment
 * instead of to freed memory. */
struct htlc_conn *hx_conn_with_serial (guint16 serial);

/* Move the focus. FALSE if `sess` isn't in the collection, which leaves the
 * focus alone — a failed switch should not blank the UI. */
gboolean hx_session_set_active (session *sess);

/* How many connections exist. */
guint hx_session_count (void);

/* The `i`th connection in creation order, or NULL when out of range. Creation
 * order is also tab order, which is why this is an index rather than an
 * unordered iteration. */
session *hx_session_at (guint i);

/* Where `sess` sits, or HX_SESSION_NOT_FOUND. */
guint hx_session_index (session *sess);

/* How many connections have been freed since launch. For the debug hooks that
 * exercise open-and-close headlessly — see the note on the definition for why
 * the serial-resolution assertions can't stand in for this. */
guint hx_debug_conns_freed (void);

/* Drop `sess` from the collection and free it. FALSE if it wasn't in it.
 *
 * `sess` is invalid on return. Nothing should be holding a pointer to it:
 * long-lived references are serials resolved through hx_session_with_serial,
 * which answers NULL from here on.
 *
 * The caller is expected to have moved the focus off it first, and to have
 * disconnected and torn down its panels — hx_session_close does all of that. */
gboolean hx_session_remove (session *sess);

G_END_DECLS

#endif /* ndef HX_SESSION_REGISTRY_H */
