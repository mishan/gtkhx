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
 * human_size / human_readable — format a byte count as the abbreviated
 * "8.3k" / "127M" / "53G" form used in the tasks window, the file
 * browser size column, transfer progress labels, and a few other
 * places. The implementation is verbatim from fileutils-4.0/lib/human.c
 * (GPL-2.0-or-later; the original was the canonical pre-coreutils
 * implementation), modulo replacing intmax_t with guint32 to match
 * what Hotline's wire format ships.
 *
 * Extracted to its own translation unit so the Tier 1 unit test can
 * link it without dragging in files.c's GTK + Adwaita pile.
 *
 * The "1024 vs 1000" choice is encoded in the output_block_size sign
 * trick: negative = base- |output_block_size| (e.g. -1024 = binary
 * suffixes); positive = absolute units. human_size hard-codes -1024,
 * the convention every GtkHx caller used at extraction time.
 */

#include "config.h"
#include <string.h>
#include <glib.h>
#include "human_readable.h"

const char human_suffixes[] = {
    0,   /* not used */
    'k', /* kilo */
    'M', /* Mega */
    'G', /* Giga */
    'T', /* Tera */
    'P', /* Peta */
    'E', /* Exa */
    'Z', /* Zetta */
    'Y'  /* Yotta */
};

/* Convert N to a human readable format in BUF.

   N is expressed in units of FROM_BLOCK_SIZE.  FROM_BLOCK_SIZE must
   be positive.

   If OUTPUT_BLOCK_SIZE is positive, use units of OUTPUT_BLOCK_SIZE in
   the output number.  OUTPUT_BLOCK_SIZE must be a multiple of
   FROM_BLOCK_SIZE or vice versa.

   If OUTPUT_BLOCK_SIZE is negative, use a format like "127k" if
   possible, using powers of -OUTPUT_BLOCK_SIZE; otherwise, use
   ordinary decimal format.  Normally -OUTPUT_BLOCK_SIZE is either
   1000 or 1024; it must be at least 2.  Most people visually process
   strings of 3-4 digits effectively, but longer strings of digits are
   more prone to misinterpretation.  Hence, converting to an
   abbreviated form usually improves readability.  Use a suffix
   indicating which power is being used.  For example, assuming
   -OUTPUT_BLOCK_SIZE is 1024, 8500 would be converted to 8.3k,
   133456345 to 127M, 56990456345 to 53G, and so on.  Numbers smaller
   than -OUTPUT_BLOCK_SIZE aren't modified.  */

char *
human_readable (guint64 n, char *buf, int from_block_size,
                int output_block_size)
{
    guint64 amt;
    uint base;
    int to_block_size;
    uint tenths;
    uint power = 0;
    char *p;

    /* 0 means adjusted N == AMT.TENTHS;
     1 means AMT.TENTHS < adjusted N < AMT.TENTHS + 0.05;
     2 means adjusted N == AMT.TENTHS + 0.05;
     3 means AMT.TENTHS + 0.05 < adjusted N < AMT.TENTHS + 0.1.  */
    uint rounding;

    if (output_block_size < 0) {
        base = -output_block_size;
        to_block_size = 1;
    } else {
        base = 0;
        to_block_size = output_block_size;
    }

    p = buf + LONGEST_HUMAN_READABLE;
    *p = '\0';

    /* Adjust AMT out of FROM_BLOCK_SIZE units and into TO_BLOCK_SIZE units.  */

    if (to_block_size <= from_block_size) {
        int multiplier = from_block_size / to_block_size;
        amt = n * multiplier;
        tenths = rounding = 0;

        if (amt / multiplier != n) {
            /* Overflow occurred during multiplication.  We should use
	     multiple precision arithmetic here, but we'll be lazy and
	     resort to floating point.  This can yield answers that
	     are slightly off.  In practice it is quite rare to
	     overflow uintmax_t, so this is good enough for now.  */

            double damt = n * (double)multiplier;

            if (!base) {
                g_snprintf (buf, LONGEST_HUMAN_READABLE, "%.0f", damt);
            } else {
                double e = 1;
                power = 0;

                do {
                    e *= base;
                    power++;
                } while (e * base <= damt
                         && power < sizeof (human_suffixes) - 1);

                damt /= e;

                g_snprintf (buf, LONGEST_HUMAN_READABLE, "%.1f%c", damt,
                            human_suffixes[power]);
                if (4 < strlen (buf)) {
                    g_snprintf (buf, LONGEST_HUMAN_READABLE, "%.0f%c", damt,
                                human_suffixes[power]);
                }
            }

            return buf;
        }
    } else {
        uint divisor = to_block_size / from_block_size;
        uint r10 = (n % divisor) * 10;
        uint r2 = (r10 % divisor) * 2;
        amt = n / divisor;
        tenths = r10 / divisor;
        rounding = r2 < divisor ? 0 < r2 : 2 + (divisor < r2);
    }

    /* Use power of BASE notation if adjusted AMT is large enough.  */

    if (base && base <= amt) {
        power = 0;

        do {
            uint r10 = (amt % base) * 10 + tenths;
            uint r2 = (r10 % base) * 2 + (rounding >> 1);
            amt /= base;
            tenths = r10 / base;
            rounding
                = (r2 < base ? 0 < r2 + rounding : 2 + (base < r2 + rounding));
            power++;
        } while (base <= amt && power < sizeof (human_suffixes) - 1);

        *--p = human_suffixes[power];

        tenths += 2 < rounding + (tenths & 1);

        if (tenths == 10) {
            amt++;
            tenths = 0;
        }

        *--p = '0' + tenths;
        *--p = '.';
        tenths = 0;
    }

    if (5 < tenths + (2 < rounding + (amt & 1))) {
        amt++;

        if (amt == base && power < sizeof (human_suffixes) - 1) {
            *p = human_suffixes[power + 1];
            *--p = '0';
            *--p = '.';
            amt = 1;
        }
    }

    do {
        *--p = '0' + (int)(amt % 10);
    } while ((amt /= 10) != 0);

    return p;
}

char *
human_size (char *sizstr, guint64 size)
{
    return human_readable (size, sizstr, 1, -1024);
}
