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

/* ---- Chat-family parsers (HTLS_HDR_CHAT / _SUBJECT / _INVITE) ----
 *
 * Each takes the message buffer (msg/msglen = htlc->in.buf/in.pos) plus a
 * caller-owned text buffer (buf/bufcap) the sanitised text is written into
 * (NUL-terminated, capped at bufcap-1). Scalar fields land in *out. They
 * return false on a NULL out, NULL buf, or zero bufcap; otherwise true,
 * mirroring the C extractors that always succeed on a well-formed frame. */

struct gtkhx_proto_chat {
    uint32_t cid;
    uint16_t uid;
    /* Offset into buf where the display text starts (0, or 1 when a
     * leading LF was stripped from the common "\nUser: msg" framing). */
    uint16_t text_off;
    /* Display-text length: strlen(buf + text_off). */
    uint16_t text_len;
};

/* Parse HTLS_HDR_CHAT. Writes the full sanitised line (CR2LF + strip_ansi)
 * into buf; out->text_off/text_len describe the display slice. */
extern bool gtkhx_proto_parse_chat (const uint8_t *msg, size_t msglen,
                                    uint8_t *buf, size_t bufcap,
                                    struct gtkhx_proto_chat *out);

struct gtkhx_proto_chat_subject {
    uint32_t cid;
    uint16_t subject_len;
};

/* Parse HTLS_HDR_CHAT_SUBJECT. Writes the subject into buf (no CR2LF /
 * strip_ansi — subjects carry no line endings). */
extern bool gtkhx_proto_parse_chat_subject (const uint8_t *msg, size_t msglen,
                                            uint8_t *buf, size_t bufcap,
                                            struct gtkhx_proto_chat_subject *out);

struct gtkhx_proto_chat_invite {
    uint32_t cid;
    uint16_t uid;
    uint16_t name_len;
};

/* Parse HTLS_HDR_CHAT_INVITE. Writes the strip_ansi'd inviter name into
 * buf. */
extern bool gtkhx_proto_parse_chat_invite (const uint8_t *msg, size_t msglen,
                                           uint8_t *buf, size_t bufcap,
                                           struct gtkhx_proto_chat_invite *out);

/* ---- User-list parsers (HTLS_HDR_USER_PART / _USER_CHANGE) ---- */

struct gtkhx_proto_user_part {
    uint32_t cid;
    uint16_t uid;
};

/* Parse HTLS_HDR_USER_PART. Both fields default to 0 when missing.
 * Returns false on NULL out; otherwise true. */
extern bool gtkhx_proto_parse_user_part (const uint8_t *msg, size_t msglen,
                                         struct gtkhx_proto_user_part *out);

struct gtkhx_proto_user_change {
    uint32_t cid;
    /* HX_NICK_COLOR_NONE (0xffffffff) when no COLOR chunk was present. */
    uint32_t nick_color;
    uint16_t uid;
    uint16_t icon;
    uint16_t color;
    /* 0/1 booleans, set when the corresponding chunk was present. */
    uint8_t got_color;
    uint8_t got_nick_color;
    /* Bytes of nickname written to buf (excluding the trailing NUL),
     * capped at bufcap-1. May differ from strlen(buf) if the wire
     * payload contained an interior NUL. */
    uint16_t name_len;
};

/* Parse HTLS_HDR_USER_CHANGE. Writes the strip_ansi'd nickname into buf
 * (NUL-terminated, capped at bufcap-1) and fills *out. Returns false on
 * NULL out, NULL buf, or zero bufcap; otherwise true. */
extern bool gtkhx_proto_parse_user_change (const uint8_t *msg, size_t msglen,
                                           uint8_t *buf, size_t bufcap,
                                           struct gtkhx_proto_user_change *out);

/* ---- Misc smaller parsers ---- */

/* Extract a HTLS_DATA_TASKERROR chunk's CR2LF + strip_ansi sanitised
 * text into *out (NUL-terminated, capped at cap-1). Returns the byte
 * count (excluding the NUL), or SIZE_MAX when no TASK_ERROR chunk was
 * present. Returns 0 on NULL out or zero cap. */
extern size_t gtkhx_proto_parse_task_error (const uint8_t *msg, size_t msglen,
                                            uint8_t *out, size_t cap);

struct gtkhx_proto_msg {
    uint16_t uid;
    uint16_t name_len;
    uint16_t msg_len;
};

/* Parse HTLS_HDR_MSG (and MSG_BROADCAST / POLITEQUIT, which share the
 * same shape). Writes the strip_ansi'd name into name_buf and the CR2LF
 * + strip_ansi'd body into msg_buf (both NUL-terminated, capped at
 * cap-1). Returns false on any NULL / zero-cap pointer; otherwise true. */
extern bool gtkhx_proto_parse_msg (const uint8_t *msg, size_t msglen,
                                   uint8_t *name_buf, size_t name_cap,
                                   uint8_t *msg_buf, size_t msg_cap,
                                   struct gtkhx_proto_msg *out);

struct gtkhx_proto_banner {
    uint8_t type_code[4];
    uint16_t url_len;
    uint8_t got_type;
    uint8_t has_url;
};

/* Parse HTLS_HDR_BANNER. The banner type is gated at exactly 4 bytes.
 * URL (if present) is written into url_buf (NUL-terminated, capped at
 * url_cap-1). Returns got_type — true iff the BANNER_TYPE chunk was
 * well-formed (the C extractor's contract). */
extern bool gtkhx_proto_parse_banner (const uint8_t *msg, size_t msglen,
                                      uint8_t *url_buf, size_t url_cap,
                                      struct gtkhx_proto_banner *out);

struct gtkhx_proto_xfer_queue {
    uint32_t htxf_ref;
    uint32_t queueid;
};

/* Parse HTLS_HDR_QUEUE. Both fields default to 0; queueid == 0 means
 * "ready, you can start the transfer". Returns false on NULL out;
 * otherwise true. */
extern bool gtkhx_proto_parse_xfer_queue (const uint8_t *msg, size_t msglen,
                                          struct gtkhx_proto_xfer_queue *out);

/* HTLS_HDR_AGREEMENT result codes matching hx_agreement_result. */
#define GTKHX_PROTO_AGREEMENT_OK      0u
#define GTKHX_PROTO_AGREEMENT_NONE    1u
#define GTKHX_PROTO_AGREEMENT_MISSING 2u

/* Parse HTLS_HDR_AGREEMENT. Returns one of GTKHX_PROTO_AGREEMENT_*.
 *
 * Output-buffer contract (matches hx_agreement_extract, which
 * tests/proto/test_agreement.c pins via "untouched" sentinel strings):
 *
 *   * OK: when out != NULL && cap > 0, writes the CR2LF + strip_ansi
 *     sanitised body into out (NUL-terminated, capped at cap-1) and,
 *     if out_len != NULL, stores the byte count (excluding the NUL)
 *     into *out_len. When out is NULL or cap is 0, the result code is
 *     still OK but neither out nor *out_len is touched.
 *   * NONE / MISSING: out and *out_len are both left untouched
 *     regardless of NULL-ness. Callers that initialised out to a
 *     known sentinel before the call can rely on it surviving. */
extern uint32_t gtkhx_proto_parse_agreement (const uint8_t *msg, size_t msglen,
                                             uint8_t *out, size_t cap,
                                             size_t *out_len);

#endif /* _HOTLINE_PROTO_H */
