/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/tracker_wire.h — a small C convenience layer over
 * the tracker listing codec, for the tests that speak the listing
 * protocol to a live tracker themselves (test_tracker_v1, _v3, _v3_tls).
 *
 * The codec is hxproto's (hx-libs), reached through gtkhx-proto-ffi's
 * gtkhx_proto_tracker_* C ABI. The client itself fetches listings
 * through hxnet, in Rust, and never needed this layer; it lived in src/
 * until nothing there called it. The functions return borrowed pointers
 * into the caller's buffer where the ABI returns offsets.
 */

#ifndef GTKHX_TESTS_TRACKER_WIRE_H
#define GTKHX_TESTS_TRACKER_WIRE_H

#include <glib.h>

G_BEGIN_DECLS

/* ---- v1 ---------------------------------------------------------- */

typedef struct {
    guint32 addr;   /* IPv4 address, network byte order */
    guint16 port;   /* host byte order */
    guint16 nusers; /* host byte order */
    guint8 name_len;
} hx_tracker_record_fixed;

/* The 14-byte reply header; *nservers_out from offset [10..11]. */
gboolean hx_tracker_reply_parse_header (const guint8 *buf, gsize len,
                                        guint16 *nservers_out);

/* A record whose first byte is 0 is a padding slot, not a server. */
gboolean hx_tracker_record_is_padding (const guint8 *buf, gsize len);

/* The 11-byte fixed prefix of a record. */
gboolean hx_tracker_record_parse_fixed (const guint8 *buf, gsize len,
                                        hx_tracker_record_fixed *out);

/* ---- v3 ---------------------------------------------------------- */

typedef struct {
    guint8 addr_type;
    const guint8 *address;
    gsize address_len;
    guint16 port;
    guint16 nusers;
    const guint8 *name;
    gsize name_len;
    const guint8 *desc;
    gsize desc_len;
    guint16 tlv_count;
    const guint8 *tlv_bytes;
    gsize tlv_bytes_len;
} hx_tracker_v3_record;

gboolean hx_tracker_v3_pack_handshake (guint8 *out, gsize out_len,
                                       guint16 features);
gboolean hx_tracker_v3_parse_handshake_response (const guint8 *buf, gsize len,
                                                 guint16 *version_out,
                                                 guint16 *features_out);
gboolean hx_tracker_v3_pack_listing_request_simple (guint8 *out, gsize out_len,
                                                    gsize *out_written);
gboolean hx_tracker_v3_parse_response_header (const guint8 *buf, gsize len,
                                              guint16 *response_type_out,
                                              guint32 *total_size_out,
                                              guint16 *total_servers_out,
                                              guint16 *record_count_out);
/* One record at `buf`; *consumed_out is the bytes it took. */
gboolean hx_tracker_v3_parse_record (const guint8 *buf, gsize buf_len,
                                     hx_tracker_v3_record *out,
                                     gsize *consumed_out);

G_END_DECLS

#endif /* GTKHX_TESTS_TRACKER_WIRE_H */
