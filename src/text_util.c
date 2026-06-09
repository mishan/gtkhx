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
 * No GTK / Adwaita / xtext dependency. The unit-test binary pairs
 * this with src/debug.c (one direct extern import) and — since
 * Phase R2 — links the Rust hotline-proto staticlib for the
 * MACINTOSH decode below. tests/meson.build declares both deps;
 * a future refactor that tries to drop either dependency will
 * see undefined symbols at link time.
 *
 * See gtkutil.h for the full strategy comment on
 * gtkhx_text_to_utf8.
 *
 * Phase R2: the Mac Roman → UTF-8 decode table moved to the Rust
 * hotline-proto crate (text.rs::to_utf8, mirroring glibc's iconv
 * "MACINTOSH" mapping byte-for-byte). The C entry point below now
 * delegates: it sizes the worst-case output buffer (3× input, the
 * Mac Roman → UTF-8 expansion ceiling), hands it to the Rust shim,
 * appends the NUL the legacy contract owes its callers, and
 * shrinks via g_realloc. Embedded-NUL preservation for the
 * "already valid UTF-8" branch falls out naturally because the
 * Rust function passes such input through verbatim.
 */

#include "config.h"
#include <string.h>
#include <glib.h>
#include "hotline_proto.h"  /* gtkhx_proto_text_to_utf8 */
#include "text_util.h"

/* GTKHX_TEXT_TO_UTF8_MAX_LEN is defined in text_util.h alongside the
 * prototype. Pathological `len` values above it are rejected here as
 * empty output rather than risk:
 *   - reading out of bounds in g_utf8_validate, which takes a gssize
 *     (signed) length; values above G_MAXSSIZE wrap negative and tell
 *     GLib to scan to the first NUL.
 *   - integer overflow on `len * 3 + 1` (the slow-path worst-case
 *     allocation) wrapping to a small number.
 *   - violating Rust's slice::from_raw_parts_mut isize::MAX requirement
 *     when the decoded-cap (`len * 3`) is passed across the FFI.
 *
 * The MAX_LEN bound is `(G_MAXSSIZE - 1) / 3`, which is the tightest
 * of the three constraints — `len * 3 + 1 <= G_MAXSSIZE` guarantees
 * both the gsize multiply and the isize cap fit. */
char *
gtkhx_text_to_utf8 (const char *bytes, gsize len, gsize *out_len)
{
    if (!bytes || len > GTKHX_TEXT_TO_UTF8_MAX_LEN) {
        if (out_len) {
            *out_len = 0;
        }
        return g_strdup ("");
    }

    /* Fast path: already-valid UTF-8 is the common case for any modern
     * (UTF-8-mode) server. Skip the worst-case 3× allocation and the
     * FFI round-trip — just g_strndup the input. Embedded NULs are
     * preserved because g_strndup copies `len` bytes verbatim before
     * appending its own trailing NUL.
     *
     * (The Rust to_utf8_into has an equivalent fast path internally,
     * but g_strndup avoids both the FFI boundary AND the 3× over-
     * allocation we'd have to size for the worst case before the call,
     * so we keep this branch in C.)
     *
     * The (gssize) cast is bounded by the length guard above. */
    if (g_utf8_validate (bytes, (gssize) len, NULL)) {
        if (out_len) {
            *out_len = len;
        }
        return g_strndup (bytes, len);
    }

    /* Slow path: Mac Roman decode through the Rust crate. Worst-case
     * expansion is 3× (Mac Roman → UTF-8 codepoints in the BMP).
     * Allocate (len * 3) bytes for the decoded output plus 1 trailing
     * byte reserved for the NUL the C contract owes — total
     * `len * 3 + 1`. The length guard above ensures len * 3 + 1
     * doesn't overflow. We pass only the decoded-output capacity
     * (`len * 3`) to the FFI; the FFI never sees the NUL slot, so
     * `buf[written] = '\0'` below is always in bounds even when the
     * decoded output uses the full 3× expansion. */
    gsize decoded_cap = len * 3;
    gsize total_cap = decoded_cap + 1;
    char *buf = g_malloc (total_cap);

    gsize written = gtkhx_proto_text_to_utf8 ((const uint8_t *) bytes, len,
                                              (uint8_t *) buf, decoded_cap);
    buf[written] = '\0';
    if (out_len) {
        *out_len = written;
    }
    /* g_realloc to the actual size — keeps the heap footprint
     * honest for callers that hold onto the result for a while
     * (chat / news / message windows). */
    return g_realloc (buf, written + 1);
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
