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
#include <glib.h>

#include "compat.h" /* PACKED — required before hotline.h / protocol.h */
#include "hotline.h"
#include "protocol.h"
#include "proto_helpers.h"
#include "cipher_aead.h"
#include "htxf_io.h"
#include "htxf_subchannel.h"

size_t
hx_htxf_subchannel_pack_preamble (guint8 *buf, size_t cap,
                                  guint32 ref, guint64 total_size,
                                  guint16 type, guint16 flags,
                                  gboolean size64)
{
    if (!buf) {
        return 0;
    }
    if (size64) {
        if (cap < SIZEOF_HTXF_HDR + 8) {
            return 0;
        }
    } else {
        if (cap < SIZEOF_HTXF_HDR) {
            return 0;
        }
        if (total_size > G_MAXUINT32) {
            /* Caller asked for the legacy 16-byte handshake but the
             * size doesn't fit in the 32-bit field. Fail closed
             * instead of silently truncating. */
            return 0;
        }
    }

    guint16 wire_flags = flags;
    guint32 wire_len;
    if (size64) {
        wire_flags |= HTXF_FLAG_LARGE_FILE | HTXF_FLAG_SIZE64;
        /* Spec: when SIZE64 is set, zero the legacy 32-bit field
         * (otherwise a non-large-file peer might attempt a partial
         * read and treat it as complete). */
        wire_len = 0;
    } else {
        wire_len = (guint32) total_size;
    }

    hl_htxf_hdr_pack (buf, ref, wire_len, type, wire_flags);

    if (!size64) {
        return SIZEOF_HTXF_HDR;
    }

    guint64 be = GUINT64_TO_BE (total_size);
    memcpy (buf + SIZEOF_HTXF_HDR, &be, sizeof (be));
    return SIZEOF_HTXF_HDR + sizeof (be);
}

void
hx_htxf_subchannel_arm_aead (struct htxf_conn *xfer,
                             const guint8 *session_key, gsize session_key_len,
                             const chacha_aead_state *ctrl_encode,
                             const chacha_aead_state *ctrl_decode,
                             guint32 ref)
{
    if (!xfer) {
        return;
    }
    htxf_io_init (xfer);
    cipher_aead_derive_transfer_keys (
        &xfer->xfer_encode, &xfer->xfer_decode,
        session_key, session_key_len,
        ctrl_encode, ctrl_decode,
        ref);
    xfer->aead_active = TRUE;
}
