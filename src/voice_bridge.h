/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

/*
 * voice_bridge.h — thin session / htlc field accessors for the Rust voice
 * UI (gtkhx-ui `users_voice_col` + `voice_panel` modules).
 *
 * The voice column + panel moved to Rust behind the crate's `voice` Cargo
 * feature. Rather than mirror the `session` / `htlc_conn` struct layouts on
 * the Rust side (fragile: they change often and carry packed protocol
 * fields), the Rust modules reach the handful of fields they need through
 * these accessors. The bridge stays with the rest of the voice C — it is
 * only referenced from the feature-on Rust build.
 */

#ifndef HX_VOICE_BRIDGE_H
#define HX_VOICE_BRIDGE_H 1

#include <glib.h>

#include "session.h"

G_BEGIN_DECLS

struct htlc_conn;
struct gtkhx_voice_runtime;
struct _HxVoiceModel;

/* sess->voice_model (the per-session HxVoiceModel GObject); NULL when the
 * session has none. Borrowed — no ref taken. */
struct _HxVoiceModel *hx_session_voice_model (session *sess);

/* sess->voice_runtime get / lazy-set. The panel constructs the runtime on
 * first Join and stashes it back on the session so a later Leave / signal
 * callback reuses the same handle. Borrowed. */
struct gtkhx_voice_runtime *hx_session_voice_runtime (session *sess);
void hx_session_set_voice_runtime (session *sess,
                                   struct gtkhx_voice_runtime *rt);

/* sess->htlc — the connection the wire senders + runtime constructor take.
 * The pointer is stable for the session's lifetime. */
struct htlc_conn *hx_session_htlc (session *sess);

/* htlc->caps & HTLC_CAP_VOICE — the server echoed voice support. */
gboolean hx_htlc_voice_cap (struct htlc_conn *htlc);

/* hl_access_has(&htlc->access, HL_ACCESS_VOICE_CHAT) — the account may use
 * voice chat. (Deliberately the strict check, not hl_access_permits: the
 * panel greys out until the bit is actually set.) */
gboolean hx_htlc_voice_access (struct htlc_conn *htlc);

/* htlc->uid — our own user id, for the runtime's self-uid + the voice
 * model's self-exclusion. */
guint16 hx_htlc_uid (struct htlc_conn *htlc);

G_END_DECLS

#endif /* HX_VOICE_BRIDGE_H */
