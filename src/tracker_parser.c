/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * src/tracker_parser.c — pure HTRK reply parsers, carved out of
 * network.c's async tracker fetch state machine. See header for
 * the wire layout. Behaviour is byte-for-byte identical to the
 * inline parses in on_tracker_header_read +
 * on_server_{hdr,rest,name,desc}_read.
 */

#include "config.h"

#include <glib.h>
#include "hotline_proto.h"     /* gtkhx_proto_parse_tracker_* */
#include "tracker_parser.h"

/* HTRK reply parsing moved to the Rust hotline-proto
 * crate. The public C surface in tracker_parser.h is preserved
 * unchanged — production callers (the async fetch state machine
 * in network.c) don't know the bodies delegate now. */

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

void
hx_tracker_normalize_text (char *buf, gsize len)
{
    gtkhx_proto_tracker_normalize_text ((uint8_t *) buf, len);
}
