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

#include <string.h>
#include <glib.h>
#include "protocol.h"          /* strip_ansi */
#include "compat.h"            /* CR2LF */
#include "tracker_parser.h"

gboolean
hx_tracker_reply_parse_header (const guint8 *buf, gsize len,
                               guint16 *nservers_out)
{
    if (!buf || len < 14 || !nservers_out) {
        return FALSE;
    }
    /* nservers lives at offset [10..11], big-endian. memcpy into
     * an aligned local before ntohs — the production callback used
     * to type-pun the byte pointer directly, which is undefined
     * behaviour under strict aliasing AND triggers SIGBUS on
     * alignment-strict targets (ARMv6, SPARC). memcpy through a
     * guint8 view is the portable byte-swap idiom and GCC / clang
     * both fold it to a single mov + bswap. */
    guint16 nservers_be;
    memcpy (&nservers_be, &buf[10], sizeof (nservers_be));
    *nservers_out = ntohs (nservers_be);
    return TRUE;
}

gboolean
hx_tracker_record_is_padding (const guint8 *buf, gsize len)
{
    if (!buf || len < 1) {
        return FALSE;
    }
    /* First byte is the high octet of the IPv4 address. The
     * tracker pads its reply with all-zero records; production
     * skips a record without advancing the nservers counter
     * when this byte is zero. */
    return buf[0] == 0;
}

gboolean
hx_tracker_record_parse_fixed (const guint8 *buf, gsize len,
                               hx_tracker_record_fixed *out)
{
    if (!buf || len < 11 || !out) {
        return FALSE;
    }
    /* memcpy through aligned locals before ntohs — production used
     * to type-pun the byte pointer directly, which is UB under
     * strict aliasing and an alignment fault on ARMv6 / SPARC.
     * Same single-mov + bswap codegen on x86. addr is left in
     * network byte order (in_addr's storage convention) so
     * callers that hand it to gtkhx_session_emit_tracker_server
     * _create / inet_ntoa don't have to byte-swap. */
    guint32 addr_be;
    guint16 port_be;
    guint16 nusers_be;
    memcpy (&addr_be,   &buf[0], sizeof (addr_be));
    memcpy (&port_be,   &buf[4], sizeof (port_be));
    memcpy (&nusers_be, &buf[6], sizeof (nusers_be));
    out->addr.s_addr = addr_be;
    out->port        = ntohs (port_be);
    out->nusers      = ntohs (nusers_be);
    /* Bytes [8..9] are reserved per the HTRK spec — production
     * has never read them and neither do we. */
    out->name_len    = buf[10];
    return TRUE;
}

void
hx_tracker_normalize_text (char *buf, gsize len)
{
    if (!buf || len == 0) {
        return;
    }
    CR2LF (buf, (int) len);
    strip_ansi (buf, (int) len);
}
