/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * Inline-media extension client-side helpers: the per-send cap gate and
 * the LOGIN-time debug log of the server's advertised limits. The upload
 * and download are hxhandlers' media.rs.
 */

#include "config.h"
#include <glib.h>
#include <stdlib.h>
#include <string.h>

#include "compat.h" /* PACKED — required before hotline.h */
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
                   (guint64)hx_conn_caps (htlc));
        return FALSE;
    }
    return TRUE;
}
