/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * session_registry.c — the collection of connections, and which one has focus.
 *
 * `the_session` used to be a file-static struct in gtkhx.c, and
 * `hx_active_session()` returned its address. That is one connection by
 * construction: there is nowhere to put a second, and nothing to switch
 * between. This is the collection it becomes.
 *
 * Two ideas, and keeping them apart is the point:
 *
 *   - The *collection* is every live connection. Model code never asks it
 *     anything — an event belongs to the connection it arrived on, and
 *     `sess_from_htlc()` answers that directly.
 *   - The *focus* is the one the user is looking at. `hx_active_session()`
 *     is now a read of that, which is what makes the tab-switched layout
 *     express itself: switching tabs moves the focus and every UI call site
 *     routed through the accessor follows without further edits.
 *
 * A session is a heap object here rather than a static, but nothing outside
 * changes: every consumer already held a `session *`.
 *
 * The factory owns exactly what is per-session. Three things that ran
 * alongside it in `fe_init` are deliberately *not* here, because running
 * them per connection would be wrong rather than merely wasteful:
 * `gtkhx_connect_signals` (the signal wiring is per-*type*, once for the
 * process), `gtkhx_sound_events_init` (one speaker), and
 * `create_toolbar_window` (one window). `fe_init` is where that split is
 * visible.
 */

#include "config.h"

#include <gtk/gtk.h>

/* hx.h first: session.h and the per-module headers below all assume the
 * umbrella's types (session, GdkRGBA, the GLib shims) are already in scope. */
#include "hx.h"

#include "chat.h"
#include "hxconn.h"
#include "msg.h"
#include "session_registry.h"
#include "tasks.h"

#ifdef HAVE_VOICE
#include "voice_model.h"
#endif

/* Every live session, in creation order. Strong: the registry owns them.
 * NULL until the first hx_session_new, so a lookup before startup reads as
 * empty rather than crashing. */
static GPtrArray *sessions;

/* Index into `sessions` of the one the user is looking at. Meaningless when
 * the array is empty, which is why every reader checks the length first. */
static guint focused;

static void
ensure_array (void)
{
    if (sessions == NULL) {
        sessions = g_ptr_array_new ();
    }
}

session *
hx_session_new (void)
{
    session *sess = g_new0 (session, 1);

    /* The connection comes first: the per-session collections below don't
     * need it, but the back-pointer does, and a session without a connection
     * is not a state anything downstream is prepared for. */
    sess->htlc = hx_conn_new ();
    hx_conn_set_sess (sess->htlc, sess);

    /* The per-session collections. chats_init additionally seeds the public
     * chat at cid 0, which must exist for as long as the table does.
     *
     * Two of these also register a process-global tab-close handler on the
     * way past (chats_init → pchat_close, msg_windows_init → msg_tab_on_close).
     * Re-registering the same function pointer per session is idempotent, so
     * this is harmless rather than merely tolerated — but it is a per-process
     * concern living inside a per-session call, and if either ever becomes
     * per-connection it has to move out of here.
     *
     * All four are pure model calls — no widgets — which is what lets the
     * factory run at the very top of fe_init, ahead of prefs_read. */
    chats_init (sess);
    tasks_init (sess);
    msg_windows_init (sess);

#ifdef HAVE_VOICE
    /* Per-session because each server's user list renders its own speaker
     * indicators. The *runtime* is the one that has to be exclusive, and
     * that is a separate arbiter — see docs/multi-connection.md. */
    sess->voice_model = hx_voice_model_new ();
#endif

    ensure_array ();
    g_ptr_array_add (sessions, sess);
    return sess;
}

session *
hx_active_session (void)
{
    if (sessions == NULL || sessions->len == 0) {
        return NULL;
    }
    if (focused >= sessions->len) {
        /* Can only happen if a session was removed without the focus being
         * moved. Clamp rather than read past the end — a stale focus is a
         * bug, but crashing the UI over it is worse. */
        g_critical ("session registry: focus %u past end (%u sessions)",
                    focused, sessions->len);
        focused = sessions->len - 1;
    }
    return g_ptr_array_index (sessions, focused);
}

gboolean
hx_session_set_active (session *sess)
{
    if (sessions == NULL || sess == NULL) {
        return FALSE;
    }
    for (guint i = 0; i < sessions->len; i++) {
        if (g_ptr_array_index (sessions, i) == sess) {
            focused = i;
            return TRUE;
        }
    }
    return FALSE;
}

guint
hx_session_count (void)
{
    return sessions ? sessions->len : 0;
}

session *
hx_session_at (guint i)
{
    if (sessions == NULL || i >= sessions->len) {
        return NULL;
    }
    return g_ptr_array_index (sessions, i);
}

gboolean
hx_session_remove (session *sess)
{
    guint i;

    if (sessions == NULL || sess == NULL) {
        return FALSE;
    }
    i = hx_session_index (sess);
    if (i == HX_SESSION_NOT_FOUND) {
        return FALSE;
    }
    g_ptr_array_remove_index (sessions, i);

    /* Keep the focus in range. A caller is expected to have moved it
     * somewhere sensible already — closing the tab you are looking at should
     * select a neighbour first — but clamping here means a caller that
     * forgets gets a wrong-but-live session rather than a read past the end. */
    if (focused >= sessions->len && sessions->len > 0) {
        focused = sessions->len - 1;
    }

    /* The struct itself is deliberately not freed.
     *
     * Raw `session *` pointers are held in places that have no way to learn
     * one has gone: the connection tab strip's index, the voice arbiter's
     * token, a dialog captured mid-answer. Every one of them is written
     * against "sessions are immortal", which has been true since the registry
     * was built and is what makes those pointers safe to hold at all.
     *
     * So closing a connection leaks one session struct and its collections.
     * That is the honest trade until those holders can be told — an audit
     * worth doing on its own, not on the way past. It is bounded by how many
     * connections a user opens and closes in one run. */
    return TRUE;
}

guint
hx_session_index (session *sess)
{
    if (sessions != NULL && sess != NULL) {
        for (guint i = 0; i < sessions->len; i++) {
            if (g_ptr_array_index (sessions, i) == sess) {
                return i;
            }
        }
    }
    return HX_SESSION_NOT_FOUND;
}
