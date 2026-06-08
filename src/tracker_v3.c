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

#include <glib.h>
#include "hotline_proto.h"          /* gtkhx_proto_tracker_v3_* */
#include "tracker_v3.h"

/* Phase R2: pack / parse moved to the Rust hotline-proto crate's
 * tracker_v3 module. The public C surface in tracker_v3.h is
 * preserved unchanged — production callers (the async fetch state
 * machine in network.c, and the meta-TLV walker in
 * tracker_v3_meta.c) don't know the bodies delegate now. */

gboolean
hx_tracker_v3_pack_handshake (guint8 *out, gsize out_len, guint16 features)
{
    return gtkhx_proto_tracker_v3_pack_handshake (out, out_len, features);
}

gboolean
hx_tracker_v3_parse_handshake_response (const guint8 *buf, gsize len,
                                        guint16 *version_out,
                                        guint16 *features_out)
{
    if (!buf || !version_out || !features_out) {
        return FALSE;
    }
    return gtkhx_proto_tracker_v3_parse_handshake_response (buf, len,
                                                            version_out,
                                                            features_out);
}

gboolean
hx_tracker_v3_pack_listing_request_simple (guint8 *out, gsize out_len,
                                           gsize *out_written)
{
    if (!out_written) {
        return FALSE;
    }
    size_t written = 0;
    if (!gtkhx_proto_tracker_v3_pack_listing_request_simple (out, out_len,
                                                              &written)) {
        return FALSE;
    }
    *out_written = written;
    return TRUE;
}

gboolean
hx_tracker_v3_parse_response_header (const guint8 *buf, gsize len,
                                     guint16 *response_type_out,
                                     guint32 *total_size_out,
                                     guint16 *total_servers_out,
                                     guint16 *record_count_out)
{
    if (!buf) {
        return FALSE;
    }
    return gtkhx_proto_tracker_v3_parse_response_header (
        buf, len, response_type_out, total_size_out, total_servers_out,
        record_count_out);
}

gboolean
hx_tracker_v3_parse_record (const guint8 *buf, gsize buf_len,
                            hx_tracker_v3_record *out, gsize *consumed_out)
{
    if (!buf || !out || !consumed_out) {
        return FALSE;
    }
    struct gtkhx_proto_tracker_v3_record parsed;
    if (!gtkhx_proto_tracker_v3_parse_record (buf, buf_len, 0, &parsed)) {
        return FALSE;
    }
    /* All slices in the public struct borrow into the caller's
     * buf — recompute the borrowed pointers from the offsets the
     * Rust shim returned. */
    out->addr_type     = parsed.addr_type;
    out->address       = buf + parsed.addr_off;
    out->address_len   = parsed.addr_len;
    out->port          = parsed.port;
    out->nusers        = parsed.nusers;
    out->name          = buf + parsed.name_off;
    out->name_len      = parsed.name_len;
    out->desc          = buf + parsed.desc_off;
    out->desc_len      = parsed.desc_len;
    out->tlv_count     = parsed.tlv_count;
    out->tlv_bytes     = buf + parsed.tlv_off;
    out->tlv_bytes_len = parsed.tlv_len;
    *consumed_out      = parsed.consumed;
    return TRUE;
}

gboolean
hx_tracker_v3_walk_tlvs (const guint8 *buf, gsize buf_len, guint16 count,
                         hx_tracker_v3_tlv_cb cb, gpointer user_data)
{
    /* The Rust parser surfaces one TLV at a time via an offset
     * iterator; the callback contract (return FALSE to stop early
     * without failure, plus the "no leftover bytes" final check)
     * stays here in C. */
    if (!buf && buf_len > 0) {
        return FALSE;
    }
    size_t off = 0;
    for (guint16 i = 0; i < count; i++) {
        struct gtkhx_proto_tracker_v3_tlv tlv;
        if (!gtkhx_proto_tracker_v3_parse_tlv_at (buf, buf_len, off, &tlv)) {
            return FALSE;
        }
        if (cb
            && !cb (tlv.id, (guint16) tlv.value_len, buf + tlv.value_off,
                    user_data)) {
            /* Caller asked us to stop early; unconsumed bytes past
             * the stop point are by definition expected. */
            return TRUE;
        }
        off = tlv.next_off;
    }
    /* All `count` entries walked. The public contract promises
     * FALSE when the supplied buf had leftover bytes past the
     * declared TLVs. */
    return off == buf_len;
}
