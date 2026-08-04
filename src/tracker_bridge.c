/*
 * Copyright (C) 2000-2026 Misha Nasledov <misha@nasledov.com>
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
 */

/*
 * tracker_bridge.c — see tracker_bridge.h. Thin session/prefs accessors
 * for the Rust tracker window. Deliberately small: all the tracker UI
 * logic is in the gtkhx-ui crate; only the raw pokes at the focused
 * session, `gtkhx_prefs` and the logger live here, because those C structs
 * aren't ported yet (R6).
 */

#include "config.h"

#include <string.h>
#include <glib.h>

#include "session.h"          /* hx_active_session, hx_connect */
#include "conn_tabs.h"
#include "gtkutil.h"          /* hx_session_label, changetitlesconnected */
#include "session_registry.h" /* hx_session_open */
#include "hxconn.h"           /* hx_conn_set_cipheralg / _compressalg */
#include "prefs.h"            /* gtkhx_prefs */
#include "gtkhx_log.h"        /* hx_printf_prefix, INFOPREFIX */
#include "tracker_bridge.h"

/* These are UI-side actions — a tracker double-click, a connect, a status
 * line — so they route through hx_active_session(): they act on whichever
 * connection the user is looking at. That is the seam M0 introduced
 * (docs/multi-connection.md); when the tab strip lands, the focus moves and
 * these follow without edits. */

int
gtkhx_tracker_pref_case (void)
{
    return gtkhx_prefs.track_case ? 1 : 0;
}

void
gtkhx_tracker_connect_apply (const char *address, guint16 port, char secure,
                             char tls, const char *cipher_name,
                             const char *listed_name)
{
    session *sess = hx_active_session ();

    /* A tracker double-click is a connect like any other, so it opens a new
     * tab rather than dropping whatever the focused connection is doing. The
     * tracker is a *browser* of servers — picking one from a list is the last
     * moment you would expect to lose the server you are on. */
    if (hx_conn_fd (sess->htlc)) {
        sess = hx_session_open (address ? address : "");
    }

    hx_conn_set_compressalg (sess->htlc, NULL);
    hx_conn_set_cipheralg (sess->htlc, cipher_name);
    hx_connect (sess->htlc, address ? address : "", port, "", "", secure, tls);

    /* After the connect, not before: its preamble clears the name fields so a
     * reconnect can't fly the last server's flag, and this one has to survive
     * that. A tracker knows what a server calls itself before we have spoken
     * to it — and a 1.2-era server never sends a name at all — so without
     * this, picking a recognisable server out of a listing gives you a tab
     * labelled with an IP. */
    if (listed_name && *listed_name) {
        g_free (sess->listed_name);
        sess->listed_name = g_strdup (listed_name);

        /* The connect above already titled the tab with the address. */
        char *label = hx_session_label (sess);
        gtkhx_conn_tabs_set_title (sess->htlc, label);
        g_free (label);
        changetitlesconnected (sess);
    }
}

void
gtkhx_tracker_log_info (const char *msg)
{
    hx_printf_prefix (hx_active_session ()->htlc, 0, INFOPREFIX, "%s",
                      msg ? msg : "");
}
