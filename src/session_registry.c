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
#include "hxnet_bridge.h"
#include "hxnet_htxf.h" /* hxnet_hope_aead_free */

#ifdef HAVE_VOICE
#include "voice_model.h"
#include "voice_runtime.h"
#endif

/* Every live session, in creation order. Strong: the registry owns them.
 * NULL until the first hx_session_new, so a lookup before startup reads as
 * empty rather than crashing. */
static GPtrArray *sessions;

/* Index into `sessions` of the one the user is looking at. Meaningless when
 * the array is empty, which is why every reader checks the length first. */
static guint focused;

static void hx_session_free (session *sess);

/* Connections freed so far. Debug-only, and it exists because freeing has no
 * other visible effect in a headless run: the close hook's "does this serial
 * still resolve?" assertions pass whether or not the connection was freed,
 * since a serial stops resolving the moment the *session* leaves the
 * collection. Counting the frees is the only thing that can tell the two
 * apart. */
static guint conns_freed;

guint
hx_debug_conns_freed (void)
{
    return conns_freed;
}

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

session *
hx_session_with_serial (guint16 serial)
{
    if (sessions == NULL || serial == 0) {
        return NULL;
    }
    for (guint i = 0; i < sessions->len; i++) {
        session *sess = g_ptr_array_index (sessions, i);

        if (hx_conn_serial (sess->htlc) == serial) {
            return sess;
        }
    }
    return NULL;
}

struct htlc_conn *
hx_conn_with_serial (guint16 serial)
{
    session *sess = hx_session_with_serial (serial);

    return sess ? sess->htlc : NULL;
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

    hx_session_free (sess);
    return TRUE;
}

/* Free a connection, releasing anything it is still holding first.
 *
 * Everything below is normally already released: `hx_htlc_close` stops the
 * keepalive and the GIF-icons probe, `rcv.c` stops the post-login fallback,
 * and the HOPE material and the transport handle go the same way. But
 * `hx_session_close` only runs that path for a connection that still has a
 * socket, and this is the last moment anything can be released at all — a
 * timer left armed would fire into freed memory, and the two allocations would
 * simply leak. Doing it unconditionally costs nothing and does not depend on
 * every close path having been walked in the right order.
 *
 * `hx_bridge_uninstall` rather than a bare clear: the handle owns a live hxnet
 * actor, and dropping the pointer would leave it running against a connection
 * that no longer exists. */
static void
hx_conn_release (struct htlc_conn *htlc)
{
    guint id;

    if (htlc == NULL) {
        return;
    }

    if ((id = hx_conn_ping_timer (htlc)) != 0) {
        g_source_remove (id);
        hx_conn_set_ping_timer (htlc, 0);
    }
    if ((id = hx_conn_post_login_timer (htlc)) != 0) {
        g_source_remove (id);
        hx_conn_set_post_login_timer (htlc, 0);
    }
    if ((id = hx_conn_gif_icons_probe_timer (htlc)) != 0) {
        g_source_remove (id);
        hx_conn_set_gif_icons_probe_timer (htlc, 0);
    }

    if (hx_conn_bridge_handle (htlc) != NULL) {
        hx_bridge_uninstall (htlc);
    }
    if (hx_conn_hope_aead (htlc) != NULL) {
        hxnet_hope_aead_free (hx_conn_hope_aead (htlc));
        hx_conn_set_hope_aead (htlc, NULL);
    }

    hx_conn_free (htlc);
    conns_freed++;
}

/* Release a session and everything it owns. Static because a session still in
 * the collection must never be freed, and hx_session_remove is the only place
 * that knows it isn't.
 *
 * Only reachable through hx_session_remove, so a freed session is never in the
 * collection — which is what makes the connection serial a safe thing to hold
 * in place of a pointer: `hx_session_with_serial` answers NULL the moment this
 * runs, and every long-lived holder asks it rather than keeping a pointer of
 * its own.
 *
 * The three collections destroy their entries: `tasks` through `task_free`,
 * `msg_windows` through `msgwin_free`, and the chat registry through the
 * `chat_free` it was built with — which is also what tears down each chat's
 * attached view.
 *
 * Most widget fields on the session are borrowed: children of a dock page that
 * `hx_session_close` destroyed before getting here, gone with their parent.
 * One is not, and is released explicitly below, because a borrowed pointer and
 * an owned one look identical at the struct:
 *
 *   - `users_view` arrives transfer-full from Rust; the session holds the only
 *     reference, and dropping it is also what lets the view's `dispose`
 *     disconnect its handler on the process-lifetime theme object — a handler
 *     that would otherwise fire against a view holding a freed session.
 *
 * The task queue is not on this list any more. It is one shared widget for the
 * whole application now, and a departing connection's rows are swept by
 * `gtasks_delete_on_conn` from `hx_htlc_close` — earlier, while the connection
 * is still the thing being torn down, rather than here.
 *
 * Ordering matters within each clear, and `g_clear_pointer` is what provides
 * it: the macro NULLs the field *before* calling the destroy function, not
 * after. So a destroy callback that re-enters — a chat's `chat_free`, a task's
 * `ptr_free` — finds `sess->chats` / `sess->tasks` already NULL and takes the
 * empty-table path, rather than walking a table mid-destruction. Reading it as
 * "destroy, then NULL" is the natural mistake, and would be a use-after-free
 * waiting for the first callback that looks back at its session.
 *
 * The connection goes too. That used to be impossible: hxnet posts main-loop
 * events carrying the connection, and a shutdown already on the idle queue can
 * be dispatched after the close that provoked it, so freeing here would have
 * turned `hx_bridge_dispatch_shutdown`'s "is this a late duplicate?" read into
 * a use-after-free. The callbacks carry the connection's *serial* now and
 * resolve it, which answers NULL for a connection that has gone — so a late
 * event drops itself, the same way it already dropped one from a stale actor.
 *
 * The back-pointer is cut first, but do not mistake that for a safety net any
 * more. It used to be one: while the connection outlived everything, a holder
 * of a stale connection got a graceful NULL out of `sess_from_htlc`. The two
 * statements are now adjacent, so that NULL is observable for no main-loop
 * iterations at all — a stale connection pointer reads freed memory inside
 * `hx_conn_sess` before it can answer anything. The rule that replaces it:
 * anything holding a connection across a turn of the main loop holds a serial
 * and resolves it. The cut is kept because it costs nothing and keeps the
 * struct honest for the instant between the two lines. */
static void
hx_session_free (session *sess)
{
    if (sess == NULL) {
        return;
    }

    g_clear_pointer (&sess->tasks, g_hash_table_destroy);
    g_clear_pointer (&sess->msg_windows, g_hash_table_destroy);
    g_clear_pointer (&sess->chats, hx_chats_free);
    g_clear_pointer (&sess->server_name, g_free);

    g_clear_object (&sess->users_view);

#ifdef HAVE_VOICE
    g_clear_object (&sess->voice_model);
    /* Normally already gone: hx_htlc_close frees it, and hx_session_close runs
     * that first. Not unconditionally, though — it only closes a connection
     * that still has a socket — so a runtime outliving a connection that was
     * already down would otherwise take a GStreamer pipeline with it. */
    if (sess->voice_runtime != NULL) {
        gtkhx_voice_runtime_free (sess->voice_runtime);
        sess->voice_runtime = NULL;
    }
#endif

    hx_conn_set_sess (sess->htlc, NULL);
    hx_conn_release (sess->htlc);
    g_free (sess);
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
