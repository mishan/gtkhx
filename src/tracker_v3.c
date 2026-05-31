/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * src/tracker_v3.c — pure encoders / parsers for the Hotline
 * tracker v3 wire format. No network I/O, no GTK; the only
 * dependencies are GLib for `gboolean` / `g_return_val_if_fail` and
 * the wire constants in hotline.h. Drivable from tests/proto/
 * test_tracker_v3.c without the GIO async state machine.
 *
 * Byte-ordering policy: all multi-byte numeric fields go through
 * memcpy-into-aligned-local then ntohs/ntohl. That's the portable
 * idiom — type-punning the byte pointer is undefined behaviour
 * under strict aliasing and SIGBUSes on ARMv6 / SPARC. Both gcc
 * and clang fold the memcpy into a single mov + bswap on x86, so
 * it's free.
 */

#include "config.h"

#include <string.h>
#include <glib.h>
#include <netinet/in.h>             /* ntohs / ntohl */
#include "compat.h"                 /* PACKED — used by structs in hotline.h */
#include "hotline.h"
#include "tracker_v3.h"

/* ---- Small read helpers --------------------------------------- */

static inline guint16
read_u16_be (const guint8 *p)
{
    guint16 v_be;
    memcpy (&v_be, p, sizeof (v_be));
    return ntohs (v_be);
}

static inline guint32
read_u32_be (const guint8 *p)
{
    guint32 v_be;
    memcpy (&v_be, p, sizeof (v_be));
    return ntohl (v_be);
}

static inline void
write_u16_be (guint8 *p, guint16 v)
{
    guint16 v_be = htons (v);
    memcpy (p, &v_be, sizeof (v_be));
}

/* ---- Handshake ------------------------------------------------- */

gboolean
hx_tracker_v3_pack_handshake (guint8 *out, gsize out_len, guint16 features)
{
    if (!out || out_len < HTRK_V3_HANDSHAKE_LEN) {
        return FALSE;
    }
    /* "HTRK" (4) + version u16 (2) + features u16 (2) = 8 bytes.
     * memcpy the literal to avoid embedding the trailing NUL the
     * compiler would add for a string literal. */
    memcpy (out, HTRK_V3_MAGIC_PREFIX, 4);
    write_u16_be (out + 4, HTRK_VERSION_V3);
    write_u16_be (out + 6, features);
    return TRUE;
}

gboolean
hx_tracker_v3_parse_handshake_response (const guint8 *buf, gsize len,
                                        guint16 *version_out,
                                        guint16 *features_out)
{
    if (!buf || !version_out || !features_out) {
        return FALSE;
    }
    if (len != 6 && len != 8) {
        return FALSE;
    }
    if (memcmp (buf, HTRK_V3_MAGIC_PREFIX, 4) != 0) {
        return FALSE;
    }
    *version_out = read_u16_be (buf + 4);
    /* For v3 (8-byte response) the trailing 2 bytes carry the
     * tracker's offered feature flags; we AND them with our offered
     * set elsewhere (in the state machine) to get the negotiated
     * intersection. For v1/v2 (6-byte response) there are no
     * trailing bytes and *features_out stays 0. */
    *features_out = (len == 8) ? read_u16_be (buf + 6) : 0;
    return TRUE;
}

/* ---- Listing request ------------------------------------------ */

gboolean
hx_tracker_v3_pack_listing_request_simple (guint8 *out, gsize out_len,
                                           gsize *out_written)
{
    if (!out || !out_written || out_len < 4) {
        return FALSE;
    }
    /* request_type=0x0001 + field_count=0 — minimum-viable request,
     * equivalent to a v1 "give me everything" listing call. Phase
     * C adds a richer builder. */
    write_u16_be (out + 0, HTRK_V3_REQ_LIST);
    write_u16_be (out + 2, 0);
    *out_written = 4;
    return TRUE;
}

/* ---- Listing response header ---------------------------------- */

gboolean
hx_tracker_v3_parse_response_header (const guint8 *buf, gsize len,
                                     guint16 *response_type_out,
                                     guint32 *total_size_out,
                                     guint16 *total_servers_out,
                                     guint16 *record_count_out)
{
    if (!buf || !response_type_out || !total_size_out || !total_servers_out
        || !record_count_out) {
        return FALSE;
    }
    if (len < HTRK_V3_RESP_HDR_LEN) {
        return FALSE;
    }
    guint16 type = read_u16_be (buf + 0);
    if (type != HTRK_V3_RESP_LIST) {
        return FALSE;
    }
    *response_type_out = type;
    *total_size_out    = read_u32_be (buf + 2);
    *total_servers_out = read_u16_be (buf + 6);
    *record_count_out  = read_u16_be (buf + 8);
    return TRUE;
}

/* ---- Server record -------------------------------------------- */

/* Internal cursor helper. Returns FALSE iff `want` bytes wouldn't
 * fit between *pos and end. */
static inline gboolean
cursor_check (gsize pos, gsize want, gsize end)
{
    return want <= end - pos;
}

gboolean
hx_tracker_v3_parse_record (const guint8 *buf, gsize buf_len,
                            hx_tracker_v3_record *out, gsize *consumed_out)
{
    if (!buf || !out || !consumed_out) {
        return FALSE;
    }

    gsize pos = 0;

    /* 1 byte address type. */
    if (!cursor_check (pos, 1, buf_len)) {
        return FALSE;
    }
    guint8 addr_type = buf[pos];
    pos += 1;

    /* Address bytes — size depends on the type byte. Hostname is
     * the only variable-length form; it's a u16 BE length followed
     * by that many UTF-8 bytes. */
    const guint8 *addr_ptr;
    gsize addr_len;

    if (addr_type == HTRK_V3_ADDR_IPV4) {
        if (!cursor_check (pos, 4, buf_len)) {
            return FALSE;
        }
        addr_ptr = buf + pos;
        addr_len = 4;
        pos += 4;
    } else if (addr_type == HTRK_V3_ADDR_IPV6) {
        if (!cursor_check (pos, 16, buf_len)) {
            return FALSE;
        }
        addr_ptr = buf + pos;
        addr_len = 16;
        pos += 16;
    } else if (addr_type == HTRK_V3_ADDR_HOSTNAME) {
        if (!cursor_check (pos, 2, buf_len)) {
            return FALSE;
        }
        guint16 hn_len = read_u16_be (buf + pos);
        pos += 2;
        if (!cursor_check (pos, hn_len, buf_len)) {
            return FALSE;
        }
        addr_ptr = buf + pos;
        addr_len = hn_len;
        pos += hn_len;
    } else {
        /* Unknown address-type byte — we can't tell how many bytes
         * to skip, so we have to bail. The state machine will close
         * the connection; a future spec rev that adds new address
         * types will need a new wrapper or an explicit "skip
         * unknown" length-prefix convention. */
        return FALSE;
    }

    /* Port (u16) + nusers (u16) + name_len (u16). */
    if (!cursor_check (pos, 6, buf_len)) {
        return FALSE;
    }
    guint16 port      = read_u16_be (buf + pos);
    guint16 nusers    = read_u16_be (buf + pos + 2);
    guint16 name_len  = read_u16_be (buf + pos + 4);
    pos += 6;

    /* Name bytes. */
    if (!cursor_check (pos, name_len, buf_len)) {
        return FALSE;
    }
    const guint8 *name_ptr = buf + pos;
    pos += name_len;

    /* Description len (u16) + description bytes. */
    if (!cursor_check (pos, 2, buf_len)) {
        return FALSE;
    }
    guint16 desc_len = read_u16_be (buf + pos);
    pos += 2;

    if (!cursor_check (pos, desc_len, buf_len)) {
        return FALSE;
    }
    const guint8 *desc_ptr = buf + pos;
    pos += desc_len;

    /* TLV count (u16) — followed by `count` entries of {id, len,
     * value}. We don't decode TLVs here; we just need to figure
     * out where the trailer ends so the caller can advance to the
     * next record. */
    if (!cursor_check (pos, 2, buf_len)) {
        return FALSE;
    }
    guint16 tlv_count = read_u16_be (buf + pos);
    pos += 2;

    const guint8 *tlv_bytes = buf + pos;
    gsize tlv_start_pos = pos;
    guint16 i;
    for (i = 0; i < tlv_count; i++) {
        if (!cursor_check (pos, 4, buf_len)) { /* id(2) + len(2) */
            return FALSE;
        }
        guint16 value_len = read_u16_be (buf + pos + 2);
        pos += 4;
        if (!cursor_check (pos, value_len, buf_len)) {
            return FALSE;
        }
        pos += value_len;
    }
    gsize tlv_bytes_len = pos - tlv_start_pos;

    /* All bounds checks passed — commit the parsed values. */
    out->addr_type     = addr_type;
    out->address       = addr_ptr;
    out->address_len   = addr_len;
    out->port          = port;
    out->nusers        = nusers;
    out->name          = name_ptr;
    out->name_len      = name_len;
    out->desc          = desc_ptr;
    out->desc_len      = desc_len;
    out->tlv_count     = tlv_count;
    out->tlv_bytes     = tlv_bytes;
    out->tlv_bytes_len = tlv_bytes_len;

    *consumed_out = pos;
    return TRUE;
}

/* ---- TLV walker ----------------------------------------------- */

gboolean
hx_tracker_v3_walk_tlvs (const guint8 *buf, gsize buf_len, guint16 count,
                         hx_tracker_v3_tlv_cb cb, gpointer user_data)
{
    if (!buf && buf_len > 0) {
        return FALSE;
    }
    gsize pos = 0;
    guint16 i;
    for (i = 0; i < count; i++) {
        if (!cursor_check (pos, 4, buf_len)) {
            return FALSE;
        }
        guint16 id        = read_u16_be (buf + pos);
        guint16 value_len = read_u16_be (buf + pos + 2);
        pos += 4;
        if (!cursor_check (pos, value_len, buf_len)) {
            return FALSE;
        }
        const guint8 *value = buf + pos;
        pos += value_len;

        if (cb && !cb (id, value_len, value, user_data)) {
            /* Caller asked us to stop; that's not a failure. The
             * "no leftover bytes" check below is also skipped —
             * the caller chose to stop early, so unconsumed bytes
             * past the stop point are by definition expected. */
            return TRUE;
        }
    }
    /* All `count` entries walked. The public contract (tracker_v3.h)
     * promises FALSE when the supplied buf had leftover bytes past
     * the declared TLVs. Record parsers slice the TLV blob to
     * "from tlv_start to record end" exactly, so pos == buf_len
     * holds for that caller; direct users who pass a wider slice
     * fail loudly here rather than silently accept a malformed
     * blob. */
    return pos == buf_len;
}
