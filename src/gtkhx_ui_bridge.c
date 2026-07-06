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
 * gtkhx_ui_bridge.c — see gtkhx_ui_bridge.h. Thin session accessors for
 * the Rust gtkhx-ui windows. Deliberately small: all the UI logic lives
 * in the Rust crate; only the raw pokes at the global `session` /
 * `htlc_conn` (not ported yet) live here.
 */

#include "config.h"

#include <string.h>
#include <gtk/gtk.h>

#include "session.h"  /* session, hx_htlc_close */
#include "network.h"  /* hx_send_agreement_agree, hx_htlc_close, hx_connect */
#include "hotline.h"  /* HTLC_CAP_TEXT_ENCODING */
#include "gtkhx_ui_bridge.h"

void
gtkhx_agreement_agree (session *sess)
{
    if (sess && sess->htlc.fd) {
        hx_send_agreement_agree (&sess->htlc);
    }
}

void
gtkhx_agreement_disagree (session *sess)
{
    if (sess && sess->htlc.fd) {
        hx_htlc_close (&sess->htlc, 1);
    }
}

void
gtkhx_session_set_agreementwin (session *sess, GtkWidget *win)
{
    if (sess) {
        sess->agreementwin = win;
    }
}

struct htlc_conn *
gtkhx_active_htlc (void)
{
    return &hx_active_session ()->htlc;
}

/* The chat window/panel widget of the session that owns `htlc` (NULL if none).
 * The Rust chat-invitation dialog parents itself here: an invite belongs to the
 * session that received it, so it's scoped by htlc rather than the active
 * session — which keeps it correct if multi-conn ever lands. */
GtkWidget *
gtkhx_htlc_chat_window (struct htlc_conn *htlc)
{
    return sess_from_htlc (htlc)->chat_window;
}

/* TRUE if the active session has a live connection (fd set). Used by the
 * Rust Broadcast composer to no-op when disconnected. */
gboolean
gtkhx_active_connected (void)
{
    return hx_active_session ()->htlc.fd != 0;
}

/* TRUE if the active session negotiated HTLC_CAP_TEXT_ENCODING (UTF-8 on the
 * wire vs. legacy Mac Roman). The Rust Broadcast sender passes this to
 * gtkhx_text_for_wire. */
gboolean
gtkhx_active_text_encoding (void)
{
    return (hx_active_session ()->htlc.caps & HTLC_CAP_TEXT_ENCODING) != 0;
}

void
gtkhx_connect_apply (session *sess, const char *server, guint16 port,
                     const char *login, const char *pass, char secure,
                     const char *compress_name, const char *cipher_name,
                     char tls)
{
    if (!sess) {
        return;
    }

    memset (sess->htlc.compressalg, 0, sizeof (sess->htlc.compressalg));
    if (compress_name && *compress_name) {
        g_strlcpy (sess->htlc.compressalg, compress_name,
                   sizeof (sess->htlc.compressalg));
    }
    memset (sess->htlc.cipheralg, 0, sizeof (sess->htlc.cipheralg));
    if (cipher_name && *cipher_name) {
        g_strlcpy (sess->htlc.cipheralg, cipher_name,
                   sizeof (sess->htlc.cipheralg));
    }

    hx_connect (&sess->htlc, server ? server : "", port, login ? login : "",
                pass ? pass : "", secure, tls);
}
