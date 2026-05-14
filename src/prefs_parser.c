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
 * Lightweight prefs-value parsers, broken out of options.c so the
 * unit tests can drive them without the GTK / Adwaita widget tree
 * options.c pulls in. See prefs_parser.h for the contract.
 */

#include "config.h"
#include <glib.h>
#include "prefs_parser.h"

gboolean
prefs_parse_boolean (const char *s, unsigned char *out)
{
    unsigned char v;

    if (!s) {
        return FALSE;
    }

    /* First character drives the decision. Matches the historical
	 * options.c parser plus the Phase 5 fix to accept GKeyFile's
	 * "true" / "false" / "yes" / "no". */
    switch (*s) {
    case '0':
    case 'f':
    case 'F':
    case 'n':
    case 'N':
        v = 0;
        break;
    case '1':
    case 't':
    case 'T':
    case 'y':
    case 'Y':
        v = 1;
        break;
    default:
        return FALSE;
    }

    if (out) {
        *out = v;
    }
    return TRUE;
}
