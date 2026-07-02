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

#include <gtk/gtk.h>

#include "session.h"  /* session, hx_htlc_close */
#include "network.h"  /* hx_send_agreement_agree, hx_htlc_close */
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
