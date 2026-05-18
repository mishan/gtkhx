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

char *
gtkhx_text_for_wire (const char *utf8, gsize utf8_len, gboolean utf8_mode,
                     gboolean is_body, gsize *out_len)
{
    char *wire;
    gsize wire_len = 0;

    if (!utf8) {
        if (out_len) {
            *out_len = 0;
        }
        return g_strdup ("");
    }

    if (utf8_mode) {
        /* UTF-8 negotiated. Pass through verbatim — no encoding
		 * conversion, no line-ending munging. Modern systems use
		 * LF and the spec says UTF-8 clients receive LF as-is. */
        wire = g_strndup (utf8, utf8_len);
        wire_len = utf8_len;
    } else {
        /* Legacy mode: encode to Mac Roman.
		 * g_convert_with_fallback substitutes the supplied fallback
		 * string ('?') for any codepoint outside the target
		 * encoding's repertoire. We pass the empty error sink — a
		 * NULL return would only happen on iconv-not-available,
		 * which is essentially impossible on a glibc / musl /
		 * macOS / Win build. Defend against it anyway and fall back
		 * to the input bytes (lossy but better than a NULL deref). */
        wire = g_convert_with_fallback (utf8, (gssize)utf8_len, "MACINTOSH",
                                        "UTF-8", "?", NULL, &wire_len, NULL);
        if (!wire) {
            wire = g_strndup (utf8, utf8_len);
            wire_len = utf8_len;
        }

        /* Phase E3: LF → CR normalisation for body fields on
		 * legacy clients. The spec calls this out: "Outbound
		 * (server → client): Before encoding to a legacy encoding,
		 * replace LF (0x0A) with CR (0x0D) for Mac Roman /
		 * Shift-JIS / Latin-1 clients." Same applies in reverse
		 * for the client-to-server direction we're in here, since
		 * legacy servers expect CR-terminated lines on the wire. */
        if (is_body) {
            for (gsize i = 0; i < wire_len; i++) {
                if (wire[i] == 0x0a) {
                    wire[i] = 0x0d;
                }
            }
        }
    }

    if (out_len) {
        *out_len = wire_len;
    }
    return wire;
}
