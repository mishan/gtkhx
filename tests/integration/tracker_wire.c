/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/* tests/integration/tracker_wire.c — see tracker_wire.h. */

#include "config.h"

#include <glib.h>
#include "hotline_proto.h" /* gtkhx_proto_tracker_* */
#include "tracker_wire.h"

gboolean
hx_tracker_reply_parse_header (const guint8 *buf, gsize len,
                               guint16 *nservers_out)
{
    return gtkhx_proto_parse_tracker_header (buf, len, nservers_out);
}

gboolean
hx_tracker_record_is_padding (const guint8 *buf, gsize len)
{
    return gtkhx_proto_tracker_record_is_padding (buf, len);
}

gboolean
hx_tracker_record_parse_fixed (const guint8 *buf, gsize len,
                               hx_tracker_record_fixed *out)
{
    if (!out) {
        return FALSE;
    }
    struct gtkhx_proto_tracker_record_fixed parsed;
    if (!gtkhx_proto_parse_tracker_record_fixed (buf, len, &parsed)) {
        return FALSE;
    }
    /* addr_be stores the wire bytes verbatim — same network-byte-
     * order convention the parser stores addresses in, so this is a
     * direct field assignment, not a byte-swap. */
    out->addr = parsed.addr_be;
    out->port = parsed.port;
    out->nusers = parsed.nusers;
    out->name_len = parsed.name_len;
    return TRUE;
}

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
    return gtkhx_proto_tracker_v3_parse_handshake_response (
        buf, len, version_out, features_out);
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
    out->addr_type = parsed.addr_type;
    out->address = buf + parsed.addr_off;
    out->address_len = parsed.addr_len;
    out->port = parsed.port;
    out->nusers = parsed.nusers;
    out->name = buf + parsed.name_off;
    out->name_len = parsed.name_len;
    out->desc = buf + parsed.desc_off;
    out->desc_len = parsed.desc_len;
    out->tlv_count = parsed.tlv_count;
    out->tlv_bytes = buf + parsed.tlv_off;
    out->tlv_bytes_len = parsed.tlv_len;
    *consumed_out = parsed.consumed;
    return TRUE;
}
