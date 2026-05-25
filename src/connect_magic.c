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
#include "compat.h"          /* PACKED — hotline.h's struct attrs */
#include "hotline.h"
#include "connect_magic.h"

gboolean
hx_connect_validate_server_magic (const guint8 *buf, gsize len)
{
    if (!buf || len != HTLS_MAGIC_LEN) {
        return FALSE;
    }
    /* memcmp not strncmp: HTLS_MAGIC contains embedded NUL bytes
     * ("TRTP\0\0\0\0"). strncmp stops as soon as it hits a NUL on
     * either side — so once both byte streams reach position 4 and
     * see '\0' there, strncmp returns 0 (equal) regardless of what
     * bytes follow in the candidate. Production's previous
     * strncmp(HTLS_MAGIC, ctx->magic, HTLS_MAGIC_LEN) would happily
     * accept the 8-byte sequence "TRTP\0XYZ" as a valid server
     * magic (the X, Y, Z bytes are never compared). memcmp doesn't
     * have the NUL-terminator semantics — it compares every one of
     * the HTLS_MAGIC_LEN bytes unconditionally. */
    return memcmp (buf, HTLS_MAGIC, HTLS_MAGIC_LEN) == 0;
}
