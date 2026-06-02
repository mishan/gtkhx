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
 * FFI prototypes for the Phase R2 `hotline-proto` Rust crate
 * (rust/crates/hotline-proto). These are hand-declared rather than
 * cbindgen-generated — the same discipline the Phase R1 crypto crates
 * use: a signature mismatch surfaces as an undefined symbol at link
 * time, which is enough for this small, opaque-pointer-free surface.
 *
 * The crate replaces the byte-twiddling in rcv.c / commands.c one
 * opcode at a time. The C side keeps the dispatch table and the
 * GtkhxSession signal emit; only the parse/serialize step moves to
 * Rust. This foundation header covers the two proof-of-concept
 * opcodes: HTLS_HDR_USER_SELFINFO and the HTLS_HDR_TASK header fields.
 */

#ifndef _HOTLINE_PROTO_H
#define _HOTLINE_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* True if the transaction header's task-error bit is set
 * (ntohl(flag) & 1). Buffer is htlc->in.buf / htlc->in.pos. */
extern bool gtkhx_proto_header_in_error (const uint8_t *buf, size_t len);

/* Extract the transaction id into *out_trans. Returns true on success,
 * false (leaving *out_trans untouched) on a short buffer. */
extern bool gtkhx_proto_header_trans (const uint8_t *buf, size_t len,
                                      uint32_t *out_trans);

/* C-ABI mirror of the Rust SelfInfo. cached_name_ptr borrows into the
 * caller's buffer and is valid only for the duration of the parse call;
 * use it immediately and do not retain it. */
struct gtkhx_proto_selfinfo {
    /* Raw 8 wire bytes (big-endian), memcpy'd straight into the
     * guint64 htlc->access — matches the original
     * memcpy(&htlc->access, dh->data, 8) byte-for-byte. */
    uint8_t access[8];
    uint32_t nick_color;
    uint16_t uid;
    uint16_t icon;
    const uint8_t *cached_name_ptr;
    size_t cached_name_len;
};

/* Parse HTLS_HDR_USER_SELFINFO. Fills *out and returns the `seen`
 * bitmask (HX_SELFINFO_* flags). Returns 0 on NULL out. */
extern uint32_t gtkhx_proto_parse_selfinfo (const uint8_t *buf, size_t len,
                                            struct gtkhx_proto_selfinfo *out);

#endif /* _HOTLINE_PROTO_H */
