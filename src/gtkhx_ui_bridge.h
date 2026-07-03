/*
 * Copyright (C) 2000-2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

/*
 * src/gtkhx_ui_bridge.h — narrow C accessors into not-yet-ported global
 * session state for the Rust gtkhx-ui windows (Phase R5), for windows
 * beyond the tracker (tracker_bridge.{c,h} covers the tracker). Keeps the
 * Rust UI from mirroring the large `session` / `htlc_conn` C structs.
 */

#ifndef HX_GTKHX_UI_BRIDGE_H
#define HX_GTKHX_UI_BRIDGE_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

struct _session;

/* Agreement window (agreement.rs). The Rust window sends AGREEMENTAGREE /
 * drops the connection through these, and hands the window back to the
 * session so the disconnect-cleanup path (gtkutil.c) can close it. */
extern void gtkhx_agreement_agree (struct _session *sess);
extern void gtkhx_agreement_disagree (struct _session *sess);
extern void gtkhx_session_set_agreementwin (struct _session *sess,
                                            GtkWidget *win);

/* &hx_active_session()->htlc — the focused connection's htlc_conn, for
 * the User Editor's wire senders (hx_useredit_create/delete/open). */
struct htlc_conn;
extern struct htlc_conn *gtkhx_active_htlc (void);

/* Connect dialog (connect.rs). Set the session's HOPE compress / cipher
 * algorithm names (NULL or "" clears them) and fire hx_connect. Keeps
 * the sess->htlc.{compressalg,cipheralg} field pokes in C so the Rust
 * Connect dialog doesn't mirror the htlc_conn struct. `compress_name` /
 * `cipher_name` are already-resolved HOPE algorithm names (the Rust
 * side does the dropdown-index / stable-byte → name translation). */
extern void gtkhx_connect_apply (struct _session *sess, const char *server,
                                 guint16 port, const char *login,
                                 const char *pass, char secure,
                                 const char *compress_name,
                                 const char *cipher_name, char tls);

G_END_DECLS

#endif /* HX_GTKHX_UI_BRIDGE_H */
