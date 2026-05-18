/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 * Public License for more details.
 */

/*
 * algo_list_nth — extract the nth entry from a HOPE-style algorithm
 * list. Wire layout: u16:count followed by `count` records of
 * { u8:namelen, namelen bytes }. Used for MAC / cipher / compression
 * algorithm negotiation.
 *
 * Extracted from src/connect.c so the Tier 1 unit test can drive
 * the malformed-input cases without dragging in GTK.
 *
 * Returns a pointer to the {namelen, name...} bytes of the nth
 * record on success, or NULL on any malformed / out-of-range input.
 * The HOPE-Secure-Login spec (Docs/Protocol/HOPE-Secure-Login.md in
 * fogWraith/Hotline) names the legacy version of this function:
 *
 *   "Sending an empty compression algorithm list (count=0) causes
 *    shxd-family clients (hx, gtkhx) to crash due to a NULL pointer
 *    dereference in list_n()."
 *
 * The legacy implementation read `list[2]` unconditionally before
 * any bounds check, so any listlen < 3 (or a NULL list) walked off
 * the buffer. This version validates the input up front and
 * re-validates on every step.
 */

#include "config.h"
#include <stddef.h>
#include <glib.h>
#include "algo_list.h"

guint8 *
list_n (guint8 *list, guint16 listlen, unsigned int n)
{
    /* Need at least the 2-byte count header plus one length byte. */
    if (!list || listlen < 3) {
        return NULL;
    }

    unsigned int i;
    guint8 *p = list + 2;

    for (i = 0;; i++) {
        size_t off = (size_t) (p - list);

        /* Need at least 1 byte at p for the namelen. */
        if (off >= listlen) {
            return NULL;
        }
        /* Need the full {namelen, name...} record to fit. */
        if (off + 1 + (size_t)*p > listlen) {
            return NULL;
        }
        if (i == n) {
            return p;
        }
        p += 1 + (size_t)*p;
    }
}
