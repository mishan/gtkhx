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
 * Encoding-conversion helpers extracted from gtkutil.c so the unit
 * tests can link a single-file translation unit without dragging in
 * GTK / Adwaita / the rest of the gtkhx kitchen-sink header pile.
 *
 * Pure GLib — no dependency on hx.h, GTK widgets, or anything that
 * would force the test binary to also link xtext, the debug logger,
 * or the protocol layer.
 *
 * See gtkutil.h for the full strategy comment on
 * gtkhx_text_to_utf8.
 */

#include "config.h"
#include <string.h>
#include <glib.h>
#include "text_util.h"

char *
gtkhx_text_to_utf8 (const char *bytes, gsize len, gsize *out_len)
{
    char *converted;
    gsize bytes_written = 0;

    if (!bytes) {
        if (out_len) {
            *out_len = 0;
        }
        return g_strdup ("");
    }

    if (g_utf8_validate (bytes, len, NULL)) {
        if (out_len) {
            *out_len = len;
        }
        return g_strndup (bytes, len);
    }

    converted = g_convert (bytes, (gssize)len, "UTF-8", "MACINTOSH", NULL,
                           &bytes_written, NULL);
    if (converted) {
        if (out_len) {
            *out_len = bytes_written;
        }
        return converted;
    }

    converted = g_utf8_make_valid (bytes, (gssize)len);
    if (out_len) {
        *out_len = strlen (converted);
    }
    return converted;
}
