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

/* ---- News parsers (1.0 flat news + 1.5 dirlist entries) ---- */

/* Extract the first HTLS_DATA_NEWS chunk's CR2LF + strip_ansi
 * sanitised text into *out (NUL-terminated, capped at cap-1). Returns
 * one of:
 *
 *   * a byte count (excluding the trailing NUL) on success;
 *   * SIZE_MAX when no NEWS chunk was present — *out is left
 *     untouched in that case (matches the "untouched" pattern
 *     test_news_file.c pins);
 *   * 0 when out == NULL or cap == 0 (no parse attempted; same
 *     defensive contract as gtkhx_proto_parse_task_error).
 *
 * Callers must distinguish "empty body" (return == 0 AND out != NULL
 * AND cap > 0) from "bad arguments" (return == 0 AND out == NULL or
 * cap == 0) on their own. The function never returns 0 for a present
 * NEWS chunk with a non-empty sanitised body. */
extern size_t gtkhx_proto_parse_news_file (const uint8_t *msg, size_t msglen,
                                           uint8_t *out, size_t cap);

/* Callback type for gtkhx_proto_walk_news_post. The bytes pointer is a
 * NUL-terminated, sanitised buffer valid only for the call's duration;
 * len is the byte count excluding the NUL. */
typedef void (*gtkhx_proto_news_post_cb) (void *user, const uint8_t *bytes,
                                          size_t len);

/* Walk every HTLS_DATA_NEWS chunk, invoking cb once per chunk with the
 * sanitised body. Returns the number of chunks emitted. NULL cb is
 * allowed (counts without dispatching). */
extern int32_t gtkhx_proto_walk_news_post (const uint8_t *msg, size_t msglen,
                                           gtkhx_proto_news_post_cb cb,
                                           void *user);

struct gtkhx_proto_news_dir_entry {
    int32_t kind;     /* 1 = folder, 2 = category */
    uint16_t name_len;
};

/* Parse HTLC_DATA_NEWSFOLDERITEM (0x0140) chunk body. */
extern bool
gtkhx_proto_parse_news_folderitem (const uint8_t *data, size_t dlen,
                                   uint8_t *name_buf, size_t name_cap,
                                   struct gtkhx_proto_news_dir_entry *out);

/* Parse HTLC_DATA_CATEGORYITEM (0x0143) chunk body. */
extern bool
gtkhx_proto_parse_news_categoryitem (const uint8_t *data, size_t dlen,
                                     uint8_t *name_buf, size_t name_cap,
                                     struct gtkhx_proto_news_dir_entry *out);

/* ---- HTLC_DATA_CATLIST (1.5 threaded news article listing) ----
 *
 * The C consumer (rcv.c::news_item_take_from_wire) steals the parsed
 * pstring pointers (subject / sender / mime_type) and frees them with
 * g_free later. Rather than allocate strings on the Rust side and
 * worry about mixing allocators, the FFI exposes an opaque handle plus
 * view-struct accessors: the C shim copies each pstring into a
 * g_strndup'd buffer itself, so g_malloc/g_free pair correctly on the
 * C side and Rust owns its parse tree end-to-end.
 *
 * Lifecycle:
 *   1. gtkhx_proto_parse_catlist returns an opaque handle (or NULL).
 *   2. _post_count + _post_get + _part_get expose the parse tree via
 *      borrowed-pointer view structs.
 *   3. gtkhx_proto_catlist_free releases the tree. The byte pointers
 *      in any view structs become invalid at that point. */

struct gtkhx_proto_catlist; /* opaque handle */

struct gtkhx_proto_catlist_post_view {
    uint32_t postid;
    uint32_t parentid;
    uint32_t date_seconds;
    uint16_t date_base_year;
    uint16_t date_pad;
    uint16_t partcount;
    uint16_t size_total;
    /* NULL when the wire pstring was zero-length (matches the C
     * hx_newscat's "NULL when empty" convention). */
    const uint8_t *subject_ptr;
    size_t subject_len;
    const uint8_t *sender_ptr;
    size_t sender_len;
};

struct gtkhx_proto_catlist_part_view {
    /* NULL when the mime pstring was zero-length. */
    const uint8_t *mime_type_ptr;
    size_t mime_type_len;
    uint16_t size;
};

/* Parse the first HTLC_DATA_CATLIST chunk. Returns NULL when no
 * CATLIST chunk is present or the body is malformed (the FALSE return
 * from hx_newscat_parse). The caller owns the returned handle and
 * must free it with gtkhx_proto_catlist_free. */
extern struct gtkhx_proto_catlist *
gtkhx_proto_parse_catlist (const uint8_t *msg, size_t msglen);

/* Free a catlist handle. NULL is a no-op. */
extern void gtkhx_proto_catlist_free (struct gtkhx_proto_catlist *cl);

/* Number of posts in *cl. 0 on NULL. */
extern uint32_t
gtkhx_proto_catlist_post_count (const struct gtkhx_proto_catlist *cl);

/* Fill *view with a borrowed snapshot of post idx. Returns false on
 * NULL / out-of-range idx. */
extern bool
gtkhx_proto_catlist_post_get (const struct gtkhx_proto_catlist *cl, uint32_t idx,
                              struct gtkhx_proto_catlist_post_view *view);

/* Fill *view with a borrowed snapshot of part (post_idx, part_idx). */
extern bool
gtkhx_proto_catlist_part_get (const struct gtkhx_proto_catlist *cl,
                              uint32_t post_idx, uint16_t part_idx,
                              struct gtkhx_proto_catlist_part_view *view);

/* ---- SEND-path builders (HTLC_HDR_CHAT / _MSG / _MSG_BROADCAST) ----
 *
 * Each builder fills a caller-provided struct hx_chunk[] (and a
 * uint8_t scratch[] buffer for the integer chunks) and returns the
 * number of chunks populated, or 0 on validation failure. Matches the
 * pre-existing hx_agreement_agree_build_chunks API in
 * src/agreement_packet.{c,h}, so production callers hand the chunks
 * array to hlwrite_chunks() for actual wire encoding — cipher,
 * compression, and fd dispatch all stay in C until Phase R3.
 *
 * Both the chunks buffer and the scratch buffer must outlive the
 * eventual hlwrite_chunks() call: the chunk data pointers reference
 * into scratch (for integer fields) and into the caller's body
 * buffer (for variable-length payloads).
 *
 * Text encoding (UTF-8 vs Mac Roman per CAP_TEXT_ENCODING) is the
 * caller's responsibility — gtkhx_text_for_wire is C-side and keeps
 * the Rust crate free of iconv. */

/* Forward decl matches what agreement_packet.h does — only the
 * struct's tag is referenced in the prototypes below (no field
 * access). Callers that need to stack-allocate `struct hx_chunk
 * chunks[N]` must #include "proto_helpers.h" directly. */
struct hx_chunk;

/* HTLC_HDR_CHAT: STYLE (u16) + CHAT body + CHAT_ID (u32, only when
 * cid != 0). Requires chunks_cap >= 3 and scratch_cap >= 6. Returns
 * 2 (no cid) or 3 (with cid) on success, or 0 on validation failure. */
extern int32_t gtkhx_proto_build_chat_chunks (uint32_t cid, uint16_t style,
                                              const uint8_t *body_ptr,
                                              size_t body_len,
                                              struct hx_chunk *chunks,
                                              size_t chunks_cap,
                                              uint8_t *scratch,
                                              size_t scratch_cap);

/* HTLC_HDR_MSG: UID (u16) + MSG body. Requires chunks_cap >= 2 and
 * scratch_cap >= 2. Returns 2 on success, 0 on validation failure. */
extern int32_t gtkhx_proto_build_msg_chunks (uint16_t uid,
                                             const uint8_t *body_ptr,
                                             size_t body_len,
                                             struct hx_chunk *chunks,
                                             size_t chunks_cap,
                                             uint8_t *scratch,
                                             size_t scratch_cap);

/* HTLC_HDR_MSG_BROADCAST: just MSG body. No scratch needed. Requires
 * chunks_cap >= 1. Returns 1 on success, 0 on validation failure. */
extern int32_t gtkhx_proto_build_broadcast_chunks (const uint8_t *body_ptr,
                                                   size_t body_len,
                                                   struct hx_chunk *chunks,
                                                   size_t chunks_cap);

/* HTLC_HDR_CHAT_CREATE: UID. chunks_cap >= 1, scratch_cap >= 2. */
extern int32_t gtkhx_proto_build_chat_create_chunks (uint16_t uid,
                                                     struct hx_chunk *chunks,
                                                     size_t chunks_cap,
                                                     uint8_t *scratch,
                                                     size_t scratch_cap);

/* HTLC_HDR_CHAT_INVITE: CHAT_ID + UID. chunks_cap >= 2,
 * scratch_cap >= 6. */
extern int32_t gtkhx_proto_build_chat_invite_chunks (uint32_t cid, uint16_t uid,
                                                     struct hx_chunk *chunks,
                                                     size_t chunks_cap,
                                                     uint8_t *scratch,
                                                     size_t scratch_cap);

/* HTLC_HDR_CHAT_JOIN: single CHAT_ID. chunks_cap >= 1,
 * scratch_cap >= 4. */
extern int32_t gtkhx_proto_build_chat_join_chunks (uint32_t cid,
                                                   struct hx_chunk *chunks,
                                                   size_t chunks_cap,
                                                   uint8_t *scratch,
                                                   size_t scratch_cap);

/* HTLC_HDR_CHAT_PART: single CHAT_ID. chunks_cap >= 1,
 * scratch_cap >= 4. */
extern int32_t gtkhx_proto_build_chat_part_chunks (uint32_t cid,
                                                   struct hx_chunk *chunks,
                                                   size_t chunks_cap,
                                                   uint8_t *scratch,
                                                   size_t scratch_cap);

/* HTLC_HDR_CHAT_DECLINE: single CHAT_ID. chunks_cap >= 1,
 * scratch_cap >= 4. */
extern int32_t gtkhx_proto_build_chat_decline_chunks (uint32_t cid,
                                                      struct hx_chunk *chunks,
                                                      size_t chunks_cap,
                                                      uint8_t *scratch,
                                                      size_t scratch_cap);

/* HTLC_HDR_CHAT_SUBJECT: CHAT_ID + subject body. chunks_cap >= 2,
 * scratch_cap >= 4. */
extern int32_t gtkhx_proto_build_chat_subject_chunks (
    uint32_t cid, const uint8_t *subject_ptr, size_t subject_len,
    struct hx_chunk *chunks, size_t chunks_cap, uint8_t *scratch,
    size_t scratch_cap);

/* HTLC_HDR_AGREEMENTAGREE: ICON + NAME + OPTIONS (all three mandatory —
 * Mobius panics without OPTIONS). chunks_cap >= 3, scratch_cap >= 4. */
extern int32_t gtkhx_proto_build_agreement_agree_chunks (
    uint16_t icon, const uint8_t *name_ptr, size_t name_len, uint16_t options,
    struct hx_chunk *chunks, size_t chunks_cap, uint8_t *scratch,
    size_t scratch_cap);

/* HTLC_HDR_USER_CHANGE: ICON + NAME + optional COLOR (Colored-
 * Nicknames extension). chunks_cap >= 3, scratch_cap >= 6.
 * has_nick_color is a 0/1 flag — when non-zero, emit DATA_COLOR with
 * the BE u32 nick_color (0x00RRGGBB); when zero, omit the chunk.
 * Returns 2 (no color) or 3 (with color) on success, or 0 on
 * validation failure (NULL pointer, short buffer, name_len > u16
 * max). */
extern int32_t
gtkhx_proto_build_user_change_chunks (uint16_t icon,
                                      const uint8_t *name_ptr, size_t name_len,
                                      uint8_t has_nick_color,
                                      uint32_t nick_color,
                                      struct hx_chunk *chunks,
                                      size_t chunks_cap,
                                      uint8_t *scratch, size_t scratch_cap);

/* HTLC_HDR_USER_KICK: optional BAN + UID. When ban != 0, emits BAN
 * first, then UID; when ban == 0, emits just UID. chunks_cap >= 2,
 * scratch_cap >= 4. Returns 1 (no ban) or 2 (with ban) on success,
 * or 0 on validation failure (NULL pointer or short buffer). */
extern int32_t gtkhx_proto_build_user_kick_chunks (uint16_t uid, uint16_t ban,
                                                   struct hx_chunk *chunks,
                                                   size_t chunks_cap,
                                                   uint8_t *scratch,
                                                   size_t scratch_cap);

/* HTLC_HDR_USER_GETINFO: single UID. chunks_cap >= 1,
 * scratch_cap >= 2. Returns 1 on success, or 0 on validation failure
 * (NULL pointer or short buffer). */
extern int32_t gtkhx_proto_build_user_getinfo_chunks (uint16_t uid,
                                                      struct hx_chunk *chunks,
                                                      size_t chunks_cap,
                                                      uint8_t *scratch,
                                                      size_t scratch_cap);

/* HTLC_HDR_ACCOUNT_READ: single LOGIN chunk. No scratch needed.
 * chunks_cap >= 1. Returns 1 on success. */
extern int32_t gtkhx_proto_build_account_read_chunks (const uint8_t *login_ptr,
                                                      size_t login_len,
                                                      struct hx_chunk *chunks,
                                                      size_t chunks_cap);

/* HTLC_HDR_ACCOUNT_DELETE: single LOGIN chunk (same shape as READ). */
extern int32_t gtkhx_proto_build_account_delete_chunks (const uint8_t *login_ptr,
                                                        size_t login_len,
                                                        struct hx_chunk *chunks,
                                                        size_t chunks_cap);

/* HTLC_HDR_ACCOUNT_MODIFY: LOGIN + PASSWORD + NAME + ACCESS (8 raw
 * bytes — the hl_access_bits bitmap). chunks_cap >= 4, scratch_cap >= 8.
 * access_ptr must point at exactly 8 bytes (NULL is rejected). Returns
 * 4 on success. */
extern int32_t gtkhx_proto_build_account_modify_chunks (
    const uint8_t *login_ptr, size_t login_len,
    const uint8_t *password_ptr, size_t password_len,
    const uint8_t *name_ptr, size_t name_len,
    const uint8_t *access_ptr,
    struct hx_chunk *chunks, size_t chunks_cap,
    uint8_t *scratch, size_t scratch_cap);

#endif /* _HOTLINE_PROTO_H */
