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

/* The session the user is looking at, or NULL before the first one exists.
 *
 * It used to be impossible for this to be NULL, and most callers still don't
 * check. That was safe when it returned the address of a static; it is
 * *still* safe in practice because startup creates a session before any UI
 * can run, but new code should not rely on it. */
session *hx_active_session (void);

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

G_END_DECLS

#endif /* ndef HX_SESSION_REGISTRY_H */
