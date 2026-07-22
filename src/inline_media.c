/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * Inline-media extension client-side helpers (Phase 9.A).
 *
 * Wire shape and per-opcode builder/parser logic lives in the Rust
 * crate (rust/crates/hotline-proto/src/inline_media.rs). This file
 * holds:
 *
 *   - inline_media_cap_ok: per-send cap gate.
 *   - inline_media_log_advertised_limits: LOGIN-time debug log of
 *     the server's advertised caps.
 *
 * The actual upload state machine (Phase 9.C), receive handler
 * (Phase 9.D), and Tier 3 wiring (Phase 9.F) build on top.
 */

#include "config.h"
#include <glib.h>
#include <stdlib.h>
#include <string.h>

#include "compat.h"        /* PACKED — required before hotline.h */
#include "hotline.h"
#include "protocol.h"
#include "hxconn.h"
#include "inline_media.h"
#include "hx.h"
#include "debug.h"

gboolean
inline_media_cap_ok (struct htlc_conn *htlc)
{
    if (!htlc) {
        return FALSE;
    }
    if (!(hx_conn_has_cap (htlc, HTLC_CAP_INLINE_MEDIA))) {
        debug_log ("media",
                   "skip inline-media send: server didn't echo "
                   "CAP_INLINE_MEDIA (caps=0x%" G_GINT64_MODIFIER "x)",
                   (guint64) hx_conn_caps (htlc));
        return FALSE;
    }
    return TRUE;
}

void
inline_media_reset_advisory_limits (struct htlc_conn *htlc)
{
    if (!htlc) {
        return;
    }
    hx_conn_reset_media_limits (htlc);
}

void
inline_media_log_advertised_limits (struct htlc_conn *htlc)
{
    if (!htlc) {
        return;
    }
    debug_log ("media",
               "server inline-media limits: max_bytes=%u max_dim=%u "
               "max_pixels=%u chunk_size=%u max_frames=%u "
               "max_duration_ms=%u (0 = use default)",
               (unsigned) hx_conn_media_max_bytes (htlc),
               (unsigned) hx_conn_media_max_dimension (htlc),
               (unsigned) hx_conn_media_max_pixels (htlc),
               (unsigned) hx_conn_media_chunk_size (htlc),
               (unsigned) hx_conn_media_max_frames (htlc),
               (unsigned) hx_conn_media_max_duration_ms (htlc));
}
