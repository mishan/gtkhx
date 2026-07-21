/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

/*
 * voice_bridge.c — see voice_bridge.h. Field accessors keeping the
 * session / htlc_conn struct layout on the C side; the Rust voice UI calls
 * these instead of duplicating the layout.
 */

#include "config.h"

#include "hx.h"
#include "protocol.h"
#include "session.h"
#include "hotline.h"
#include "hl_access.h"
#include "voice_bridge.h"

struct _HxVoiceModel *
hx_session_voice_model (session *sess)
{
    return sess ? sess->voice_model : NULL;
}

struct gtkhx_voice_runtime *
hx_session_voice_runtime (session *sess)
{
    return sess ? sess->voice_runtime : NULL;
}

void
hx_session_set_voice_runtime (session *sess, struct gtkhx_voice_runtime *rt)
{
    if (sess) {
        sess->voice_runtime = rt;
    }
}

struct htlc_conn *
hx_session_htlc (session *sess)
{
    return sess ? sess->htlc : NULL;
}

gboolean
hx_htlc_voice_cap (struct htlc_conn *htlc)
{
    return htlc && (htlc->caps & HTLC_CAP_VOICE) != 0;
}

gboolean
hx_htlc_voice_access (struct htlc_conn *htlc)
{
    return htlc
           && hl_access_has ((const guint8 *) &htlc->access,
                             HL_ACCESS_VOICE_CHAT);
}

guint16
hx_htlc_uid (struct htlc_conn *htlc)
{
    return htlc ? htlc->uid : 0;
}
