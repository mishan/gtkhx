/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * src/banner_dispatch.c — pure dispatch helpers carved out of
 * banner.c so the Tier 2 test suite can drive them without
 * dragging in GtkPicture / libsoup / pthread. See banner_dispatch.h
 * for the contract; behaviour is byte-for-byte identical to the
 * inline versions in banner_handle_message + banner_handle_htxf
 * _reply.
 */

#include "config.h"
#include <glib.h>
#include "banner_dispatch.h"

gboolean
hx_banner_type_is_url (const char *type)
{
    if (!type) {
        return FALSE;
    }

    /* Copy up to sizeof(trimmed)-1 (7) chars from `type` into a
     * NUL-terminated scratch buffer, stopping at the first space
     * or NUL. mhxd pads short codes with a trailing space
     * ("URL "); other servers might NUL-terminate at 3
     * ("URL\0"). Strip both before comparing so both shapes
     * classify the same.
     *
     * 8 bytes is way more than the protocol allows (the type
     * chunk is fixed at 4 on the wire) — the extra room here
     * is purely so a future spec change to wider codes wouldn't
     * trip a buffer bug in this helper. The strcasecmp below
     * still anchors the match on "URL" exactly, so a longer
     * prefix copied here ("URLABCD") will NOT silently match. */
    gchar trimmed[8];
    gsize n = 0;
    while (n < sizeof (trimmed) - 1 && type[n] && type[n] != ' ') {
        trimmed[n] = type[n];
        n++;
    }
    trimmed[n] = '\0';

    return g_ascii_strcasecmp (trimmed, "URL") == 0;
}

hx_banner_htxf_validation
hx_banner_validate_htxf_reply (guint32 ref, guint32 size)
{
    /* Order matches the inline checks in banner_handle_htxf_reply
     * for byte-for-byte behavioural parity: ref+size pair is
     * checked first (server didn't actually allocate a transfer
     * for us), then the size ceiling (server wants us to allocate
     * absurdly much memory). */
    if (ref == 0) {
        return HX_BANNER_HTXF_ZERO_REF;
    }
    if (size == 0) {
        return HX_BANNER_HTXF_ZERO_SIZE;
    }
    if (size > HX_BANNER_MAX_HTXF_SIZE) {
        return HX_BANNER_HTXF_TOO_LARGE;
    }
    return HX_BANNER_HTXF_OK;
}
