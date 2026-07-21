/*
 * Copyright (C) 2000-2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

/*
 * src/tracker_bridge.h — narrow C accessors into not-yet-ported global
 * session / prefs state, for the Rust tracker window (gtkhx-ui crate,
 * Phase R5). The tracker window UI itself lives in Rust now; these three
 * shims let it reach `the_session.htlc`, `gtkhx_prefs`, and the chat
 * logger without the Rust side having to mirror those large C structs.
 *
 * Named for the tracker (its first consumer), but connect_apply /
 * log_info are generic and future R5 window ports reuse them.
 */

#ifndef HX_TRACKER_BRIDGE_H
#define HX_TRACKER_BRIDGE_H

#include <glib.h>

G_BEGIN_DECLS

/* Read gtkhx_prefs.track_case (1 = match case). */
extern int gtkhx_tracker_pref_case (void);

/* Direct double-click connect: reset the session htlc's cipher /
 * compress alg, set cipheralg to `cipher_name` when non-NULL, then
 * hx_connect() to `the_session.htlc`. `secure` / `tls` are 0/1 flags. */
extern void gtkhx_tracker_connect_apply (const char *address, guint16 port,
                                         char secure, char tls,
                                         const char *cipher_name);

/* hx_printf_prefix(the_session.htlc, 0, INFOPREFIX, "%s", msg) — an
 * INFO-prefixed status line in the chat output. */
extern void gtkhx_tracker_log_info (const char *msg);

G_END_DECLS

#endif /* HX_TRACKER_BRIDGE_H */
