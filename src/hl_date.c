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
 * hl_date_decode — parse Hotline's 8-byte timestamp into a Unix
 * time_t. Auto-detects the wire format the server picked based on
 * the year field:
 *
 *   Mac 1904 epoch  year == 1904. Seconds field is total seconds
 *                   since 1904-01-01 00:00:00 UTC. Original wire
 *                   format used by vintage Mac servers (and mhxd /
 *                   hlserver / Mobius default).
 *   Modern          year != 1904. Seconds field is seconds since
 *                   Jan 1 of `year`, local time. New wire format
 *                   per the Capabilities.md "Date Format Selection"
 *                   section — servers that see DATA_CAPABILITIES
 *                   from us switch to this format because the
 *                   1904 epoch's u32 seconds field overflows
 *                   in 2040 and several client implementations
 *                   treat the year literally with overflow-prone
 *                   arithmetic.
 *
 * Wire layout (big-endian):
 *
 *   bytes 0..1  year (u16)
 *   bytes 2..3  milliseconds (u16, typically zero, ignored here)
 *   bytes 4..7  seconds (u32)
 *
 * Returns TRUE on a successful decode (out_t filled in), FALSE on
 * a clearly-invalid input (all-zero seconds field which servers
 * use as "no timestamp set", or year ranges that can't be made
 * sense of).
 *
 * Lives in its own translation unit so the Tier 1 unit test can
 * exercise it without dragging in GTK / Adwaita / rcv.c's pile.
 */

#include "config.h"
#include <time.h>
#include <glib.h>
#include "hl_date.h"

/* Offset between the Mac classic epoch (1904-01-01 00:00:00 UTC)
 * and the Unix epoch (1970-01-01 00:00:00 UTC), in seconds. */
#define MAC_TO_UNIX_EPOCH_OFFSET 2082844800U

gboolean
hl_date_decode (const guint8 *buf, time_t *out_t)
{
    if (!buf || !out_t) {
        return FALSE;
    }

    guint16 year = ((guint16)buf[0] << 8) | (guint16)buf[1];
    guint32 secs = ((guint32)buf[4] << 24) | ((guint32)buf[5] << 16)
                   | ((guint32)buf[6] << 8) | (guint32)buf[7];

    /* "No timestamp" sentinel — both formats agree on zero. Some
	 * servers send all-zero for never-set timestamps; rendering
	 * them as 1904-01-01 (or 1970-01-01) would be misleading. */
    if (secs == 0) {
        return FALSE;
    }

    if (year == 1904) {
        /* Legacy Mac 1904 epoch. Convert by subtracting the
		 * 66-year offset to land in Unix time. */
        *out_t = (time_t)secs - (time_t)MAC_TO_UNIX_EPOCH_OFFSET;
        return TRUE;
    }

    /* Modern format. `secs` is seconds elapsed since Jan 1 of `year`
	 * in local time. Build a struct tm at Jan 1 of `year` and
	 * advance it by `secs`. mktime interprets struct tm in local
	 * time, which matches what the spec asks for. */
    if (year < 1970 || year > 2200) {
        /* Outside any reasonable Hotline-era range — refuse rather
		 * than overflow on the year - 1900 below. */
        return FALSE;
    }
    struct tm tm = { 0 };
    tm.tm_year = (int)year - 1900;
    tm.tm_mon = 0;
    tm.tm_mday = 1;
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = (int)secs; /* mktime normalises out-of-range seconds */
    tm.tm_isdst = -1;      /* let libc determine DST for the date */

    time_t t = mktime (&tm);
    if (t == (time_t)-1) {
        return FALSE;
    }
    *out_t = t;
    return TRUE;
}
