/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * src/tracker_parser.h — pure parsers for the Hotline tracker (HTRK)
 * reply wire format. Carved out of network.c's async tracker fetch
 * state machine so tests/proto/test_tracker_parser.c can pin the
 * byte layouts without dragging in GIO / GSocketClient.
 *
 * HTRK reply layout (after the client sends HTRK_MAGIC and gets a
 * matching server reply):
 *
 *   14-byte response header
 *     [0..9]   opaque (msg type + protocol ver + msg-id; unused
 *              by the client)
 *     [10..11] u16 BE — number of server records to follow
 *     [12..13] opaque
 *
 *   Per server record (variable length):
 *     [0..3]   u32 BE — IPv4 address (network byte order; IP=0 in
 *              byte 0 marks a padding/empty slot, NOT a record)
 *     [4..5]   u16 BE — TCP port
 *     [6..7]   u16 BE — number of users currently on this server
 *     [8..9]   reserved (skipped)
 *     [10]     u8     — server name length N
 *     [11..11+N-1]    — server name (CR / ANSI possible; normalize)
 *     [11+N]   u8     — server description length M
 *     [12+N..11+N+M]  — description (CR / ANSI possible; normalize)
 *
 * The async fetch in network.c reads the fixed portions in
 * chunked g_input_stream_read_all_async calls (14 / 8 / 3 / 1+name
 * / 1 / desc). These parsers operate on the already-buffered
 * bytes; the state machine just calls them after each read
 * completes.
 */

#ifndef GTKHX_TRACKER_PARSER_H
#define GTKHX_TRACKER_PARSER_H

#include <stdint.h>
#include <glib.h>

/* Parsed fixed portion of a single server record. The variable
 * name + description come on separate reads. */
typedef struct {
    guint32        addr;    /* IPv4 address, network byte order */
    guint16        port;    /* TCP port, host byte order */
    guint16        nusers;  /* user count, host byte order */
    guint8         name_len;
} hx_tracker_record_fixed;

/* Parse the 14-byte HTRK reply header. Returns TRUE iff `len >=
 * 14`, populates *nservers_out (host byte order) from offset
 * [10..11]. The remaining bytes are opaque to the client and
 * unchecked here. */
extern gboolean hx_tracker_reply_parse_header (const guint8 *buf, gsize len,
                                               guint16 *nservers_out);

/* TRUE iff `buf` (length `len`, must be >= 1) starts a padding
 * slot — first byte = 0 means the leading octet of the IPv4
 * address is 0, which isn't a legal Hotline server IP. The async
 * state machine skips these without advancing the record
 * counter. Returns FALSE on len==0 (defensive — caller should
 * not have invoked us). */
extern gboolean hx_tracker_record_is_padding (const guint8 *buf, gsize len);

/* Parse the 11-byte fixed prefix of a server record into `out`.
 * Returns TRUE iff `len >= 11`. Layout: addr[0..3] / port[4..5]
 * / nusers[6..7] / reserved[8..9] / name_len[10]. */
extern gboolean
hx_tracker_record_parse_fixed (const guint8 *buf, gsize len,
                               hx_tracker_record_fixed *out);

/* Normalize a server name or description in place: CR (0x0D)
 * bytes become LF (0x0A); low control bytes that strip_ansi
 * recognises (ESC and the other C0 codes in its range) are
 * remapped to printable ASCII via `(c & 127) | 64`, NOT
 * removed — the buffer length stays the same, the bytes
 * just stop being control characters. Matches what the async
 * fetch does on each name/desc once it arrives, kept here as
 * a single helper so the test pins the contract. `buf` need
 * not be NUL-terminated. */
extern void hx_tracker_normalize_text (char *buf, gsize len);

#endif /* GTKHX_TRACKER_PARSER_H */
