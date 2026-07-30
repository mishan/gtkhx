/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * src/tracker_v3.h — pure encoders and parsers for the Hotline
 * tracker v3 protocol.
 *
 * v1 parsers live in tracker_parser.{c,h}. They stay where they are
 * because the v1 wire shape isn't going away — any tracker still
 * running v1 has to keep working. v3 is wholly additive: a separate
 * module so the existing v1 tests / state machine don't grow new
 * preprocessor branches, and so Tier 2 tests can pin the v3 wire
 * format without dragging in GIO / GSocketClient.
 *
 * What's in scope here:
 *
 *   - 8-byte v3 handshake encoder + handshake-response decoder
 *     (covers both v1/v2 backcompat at 6 bytes and v3 at 6+2).
 *   - 4-byte minimum listing-request encoder (request_type +
 *     field_count, no TLVs). The Phase C SEARCH_TEXT / pagination
 *     encoders bolt on top.
 *   - 10-byte listing-response header parser.
 *   - Single-record parser: takes a whole record's worth of bytes
 *     (variable size), populates a record struct, reports how many
 *     bytes were consumed. The state machine in network.c reads the
 *     entire response payload in one go (size known from the
 *     response header) and then walks the buffer with this parser.
 *   - TLV walker — invokes a callback per (id, len, value).
 *
 * What's NOT in scope:
 *
 *   - Server-side registration, HMAC-signed datagrams, nonces,
 *     REG_TOKEN, federation, content-index injection. We're a
 *     client; we never send registrations.
 *   - Network I/O. Every function in this header operates on
 *     already-buffered bytes.
 *   - String encoding. v3 is UTF-8 mandatory; we don't transcode.
 *     The v1 path's MacRoman → UTF-8 step lives in
 *     hx_tracker_server_new_v1 (src/tracker_event.c) — the v3
 *     constructor in that same file runs g_utf8_make_valid as a
 *     defence-in-depth pass but doesn't transcode.
 */

#ifndef GTKHX_TRACKER_V3_H
#define GTKHX_TRACKER_V3_H

#include <stdint.h>
#include <glib.h>

G_BEGIN_DECLS

/* ---- Handshake -------------------------------------------------- */

/* Build the 8-byte client-side v3 handshake into `out` (must be at
 * least HTRK_V3_HANDSHAKE_LEN = 8 bytes). `features` is the bitmask
 * of HTRK_V3_FEAT_* bits the client is offering — typical:
 * HTRK_V3_FEAT_IPV6 plus HTRK_V3_FEAT_QUERY when we'll send a
 * SEARCH_TEXT in the listing request. Returns FALSE on NULL or
 * insufficient buffer. */
extern gboolean hx_tracker_v3_pack_handshake (guint8 *out, gsize out_len,
                                              guint16 features);

/* Parse the tracker's response to our handshake. The state machine
 * reads 6 bytes first; if `version` comes back as HTRK_VERSION_V3 it
 * then reads the trailing 2 bytes and calls us again with len == 8.
 *
 *   len == 6 → version is read, *features_out is left zero.
 *   len == 8 → version + features both populated.
 *
 * Validates the "HTRK" magic prefix. Returns FALSE on bad magic,
 * NULL pointers, or wrong length. */
extern gboolean hx_tracker_v3_parse_handshake_response (const guint8 *buf,
                                                        gsize len,
                                                        guint16 *version_out,
                                                        guint16 *features_out);

/* ---- Listing request ------------------------------------------- */

/* Build a no-TLV listing request — the minimum 4 bytes the spec
 * requires (request_type=0x0001 + field_count=0). `out` must be at
 * least 4 bytes; `*out_len` receives the byte count actually
 * written. Returns FALSE on NULL or insufficient buffer.
 *
 * Phase C will add a richer builder that takes SEARCH_TEXT +
 * pagination as parameters and writes TLV fields. */
extern gboolean hx_tracker_v3_pack_listing_request_simple (guint8 *out,
                                                           gsize out_len,
                                                           gsize *out_written);

/* ---- Listing response ------------------------------------------ */

/* Parse the 10-byte listing-response header. Returns FALSE if `len
 * < 10`, the response_type isn't HTRK_V3_RESP_LIST, or any pointer
 * is NULL.
 *
 *   *response_type_out  → u16 BE from offset [0..1]
 *   *total_size_out     → u32 BE from [2..5]  (records-blob bytes)
 *   *total_servers_out  → u16 BE from [6..7]  (full match count)
 *   *record_count_out   → u16 BE from [8..9]  (records in this msg) */
extern gboolean hx_tracker_v3_parse_response_header (const guint8 *buf,
                                                     gsize len,
                                                     guint16 *response_type_out,
                                                     guint32 *total_size_out,
                                                     guint16 *total_servers_out,
                                                     guint16 *record_count_out);

/* ---- Server record --------------------------------------------- */

/* Parsed view of a single v3 server record. All string pointers
 * (`address`, `name`, `desc`, `tlv_bytes`) point INTO the source
 * buffer the parser was given — they're not owned by the record.
 * Caller must copy what it needs before the source buffer goes
 * away. `address_len` / `name_len` / `desc_len` / `tlv_bytes_len`
 * give the borrowed-slice lengths; the bytes are NOT NUL-terminated
 * in the source.
 *
 * For IPv4 (addr_type == 0x04): address is the raw 4-byte network-
 * order IPv4 address bytes; address_len == 4.
 * For IPv6 (0x06): 16 bytes of IPv6 address.
 * For hostname (0x48): UTF-8 hostname bytes (no length prefix).
 */
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
    const guint8
        *tlv_bytes; /* raw TLV blob — walk with hx_tracker_v3_walk_tlvs */
    gsize tlv_bytes_len;
} hx_tracker_v3_record;

/* Parse a single record starting at `buf`. On success, `*record_out`
 * is populated with borrowed pointers into `buf` and `*consumed_out`
 * is set to the byte count consumed (so the caller can advance past
 * this record into the next). Returns FALSE on truncation, an
 * unknown address-type byte, or any size that would over-read the
 * buffer. The record_out fields are undefined on failure.
 *
 * The parser tolerates unknown TLV IDs in the trailer — it just
 * walks past them. That's the forward-compat rule from the spec. */
extern gboolean hx_tracker_v3_parse_record (const guint8 *buf, gsize buf_len,
                                            hx_tracker_v3_record *record_out,
                                            gsize *consumed_out);

/* ---- TLV walker ------------------------------------------------ */

/* Callback fired once per TLV. `value` points into the source
 * buffer; `value_len` is the size from the TLV's length field
 * (caller must not read past it). Return TRUE to keep walking,
 * FALSE to stop early. */
typedef gboolean (*hx_tracker_v3_tlv_cb) (guint16 id, guint16 value_len,
                                          const guint8 *value,
                                          gpointer user_data);

/* Walk a TLV blob of `count` entries. Returns TRUE iff the full
 * blob parsed cleanly (each entry's length field stayed in-bounds,
 * we hit exactly `count` entries, no leftover bytes). Returns FALSE
 * on truncation. Stops early — and returns TRUE — if the callback
 * returns FALSE. */
extern gboolean hx_tracker_v3_walk_tlvs (const guint8 *buf, gsize buf_len,
                                         guint16 count, hx_tracker_v3_tlv_cb cb,
                                         gpointer user_data);

G_END_DECLS

#endif /* GTKHX_TRACKER_V3_H */
