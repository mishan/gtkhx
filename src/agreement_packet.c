/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#include "config.h"
#include <string.h>
#include <arpa/inet.h>
#include <glib.h>

#include "compat.h"
#include "hotline.h"
#include "proto_helpers.h"
#include "agreement_packet.h"

int
hx_agreement_agree_build_chunks (const hx_agreement_agree_request *req,
                                 struct hx_chunk *chunks, int chunks_cap,
                                 guint8 *scratch, size_t scratch_cap)
{
    if (!req || !chunks || !scratch) {
        return 0;
    }
    if (chunks_cap < HX_AGREEMENT_AGREE_MAX_CHUNKS) {
        return 0;
    }
    if (scratch_cap < 4) {
        /* Two u16s laid out back-to-back. */
        return 0;
    }

    /* Lay out the two big-endian u16s in scratch. icon at offset 0,
     * options at offset 2. Chunks point at those slots. */
    guint16 icon_be    = htons (req->icon);
    guint16 options_be = htons (req->options);
    memcpy (scratch + 0, &icon_be,    2);
    memcpy (scratch + 2, &options_be, 2);

    chunks[0].type = HTLC_DATA_ICON;
    chunks[0].len  = 2;
    chunks[0].data = scratch + 0;

    chunks[1].type = HTLC_DATA_NAME;
    chunks[1].len  = req->display_name_len;
    chunks[1].data = (req->display_name && req->display_name_len)
                         ? (const void *) req->display_name
                         : (const void *) "";

    chunks[2].type = HTLC_DATA_OPTIONS;
    chunks[2].len  = 2;
    chunks[2].data = scratch + 2;

    return HX_AGREEMENT_AGREE_MAX_CHUNKS;
}
