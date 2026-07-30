/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "config.h"

#include <string.h>

#include "pict_embed.h"

/* Mac OS classic stored PICT files with a 512-byte zero-filled
 * lead — that's the data-fork padding from when the resource fork
 * lived in the same physical file. Modern PICT files conventionally
 * keep the padding (it's part of the QuickDraw spec); some
 * tools strip it. We try both: skip 512 if it looks zero-padded,
 * scan from the start otherwise. */
#define PICT_HEADER_SIZE 512

static gboolean
header_looks_padded (const guint8 *data, gsize len)
{
    /* The header should be all zeros, but in practice some tools
     * left a few stray bytes near the end (the 2-byte picSize that
     * v1 PICTs stored at offset 512, the picFrame Rect, etc.). Be
     * loose: if the first 256 bytes are all zero, assume the header
     * is present. */
    if (len < 256) {
        return FALSE;
    }
    for (gsize i = 0; i < 256; i++) {
        if (data[i] != 0) {
            return FALSE;
        }
    }
    return TRUE;
}

/* Search `hay` for the first occurrence of `needle`, returning the
 * offset or (gsize)-1 if not found. memmem(3) would do but it's
 * GNU-only; rolling our own keeps us portable to anywhere GLib
 * builds. The naive byte-by-byte scan is fine — needles are 3-8
 * bytes, haystacks are at most a few MB. */
static gsize
find_bytes (const guint8 *hay, gsize hay_len, const guint8 *needle,
            gsize needle_len)
{
    if (needle_len == 0 || hay_len < needle_len) {
        return (gsize)-1;
    }
    for (gsize i = 0; i + needle_len <= hay_len; i++) {
        if (hay[i] == needle[0] && memcmp (hay + i, needle, needle_len) == 0) {
            return i;
        }
    }
    return (gsize)-1;
}

GBytes *
hx_pict_extract_embedded (const guint8 *data, gsize len)
{
    /* Recognised image signatures. Order matters only for tie-
     * breaking when two would match at the same offset, which won't
     * happen — these byte patterns don't overlap. JPEG SOI is the
     * shortest (3 bytes) and therefore most prone to a false-
     * positive match in random data; we treat it the same as the
     * others because in practice PICT files don't contain stray
     * 0xFF 0xD8 0xFF byte triples outside of an embedded JPEG. */
    static const guint8 sig_jpeg[3] = { 0xFF, 0xD8, 0xFF };
    static const guint8 sig_png[8]
        = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
    static const guint8 sig_gif87[6] = { 'G', 'I', 'F', '8', '7', 'a' };
    static const guint8 sig_gif89[6] = { 'G', 'I', 'F', '8', '9', 'a' };
    /* TIFF: II*\0 (little-endian) or MM\0* (big-endian). */
    static const guint8 sig_tiff_le[4] = { 'I', 'I', 0x2A, 0x00 };
    static const guint8 sig_tiff_be[4] = { 'M', 'M', 0x00, 0x2A };

    const struct {
        const guint8 *bytes;
        gsize len;
    } sigs[] = {
        { sig_jpeg, sizeof sig_jpeg },
        { sig_png, sizeof sig_png },
        { sig_gif87, sizeof sig_gif87 },
        { sig_gif89, sizeof sig_gif89 },
        { sig_tiff_le, sizeof sig_tiff_le },
        { sig_tiff_be, sizeof sig_tiff_be },
    };

    const guint8 *scan;
    gsize scan_len;
    gsize best = (gsize)-1;

    if (!data || len == 0) {
        return NULL;
    }

    /* Skip the 512-byte padding when present. When stripped (the
     * tool that produced the file already excised it), scan the
     * whole input. We don't know up front which case we're in, so
     * pick based on a cheap zero-prefix check. */
    if (header_looks_padded (data, len) && len > PICT_HEADER_SIZE) {
        scan = data + PICT_HEADER_SIZE;
        scan_len = len - PICT_HEADER_SIZE;
    } else {
        scan = data;
        scan_len = len;
    }

    for (gsize i = 0; i < G_N_ELEMENTS (sigs); i++) {
        gsize off = find_bytes (scan, scan_len, sigs[i].bytes, sigs[i].len);
        if (off != (gsize)-1 && (best == (gsize)-1 || off < best)) {
            best = off;
        }
    }

    if (best == (gsize)-1) {
        return NULL;
    }

    return g_bytes_new (scan + best, scan_len - best);
}
