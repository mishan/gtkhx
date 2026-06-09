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

struct gtkhx_proto_user_list_record {
    /* HX_NICK_COLOR_NONE (0xffffffff) when no Colored-Nicknames
     * trailer was present; otherwise the 0x00RRGGBB nick colour. */
    uint32_t nick_color;
    uint16_t uid;
    uint16_t icon;
    uint16_t color;
    /* 0/1 — set iff the Colored-Nicknames trailer was present in
     * this record (chunk had at least 8 + clamped_nlen + 4 bytes). */
    uint8_t got_nick_color;
    /* Bytes written to name_buf, excluding the trailing NUL. */
    uint16_t name_len;
};

/* Parse one HTLS_DATA_USER_LIST chunk's payload — the per-user
 * record body, not a whole frame. Writes the strip_ansi'd nickname
 * into name_buf (NUL-terminated, capped at name_cap-1) and fills
 * *out. Returns false on NULL out / NULL name_buf / zero name_cap /
 * a chunk shorter than the 8 fixed bytes (mirroring the C
 * extractor silently skipping malformed records); otherwise true.
 *
 * Caller iterates HTLS_DATA_USER_LIST chunks (with the dh_start /
 * dh_end macros in the rcv path) and invokes this for each chunk's
 * (data, len) pair. */
extern bool gtkhx_proto_parse_user_list_record (
    const uint8_t *data, size_t data_len,
    uint8_t *name_buf, size_t name_cap,
    struct gtkhx_proto_user_list_record *out);

/* ---- Account / user-info reply parsers (post-TASK payloads) ---- */

struct gtkhx_proto_user_info {
    /* Bytes written to name_buf / info_buf, excluding the trailing NUL. */
    uint16_t name_len;
    uint16_t info_len;
};

/* Parse the post-HTLC_HDR_USER_GETINFO TASK reply payload — the
 * server's response arrives inside an HTLS_HDR_TASK frame; this
 * parses the post-TASK body (the rcv_task_user_info body in C).
 * Writes strip_ansi'd NAME into name_buf
 * (NUL-terminated, capped at name_cap-1) and CR2LF + strip_ansi'd
 * USER_INFO body into info_buf (capped at info_cap-1). Returns false
 * on any NULL / zero-cap pointer; otherwise true. The caller's
 * `nlen && ilen` dispatch gate is preserved at the call site. */
extern bool gtkhx_proto_parse_user_info (const uint8_t *msg, size_t msglen,
                                         uint8_t *name_buf, size_t name_cap,
                                         uint8_t *info_buf, size_t info_cap,
                                         struct gtkhx_proto_user_info *out);

struct gtkhx_proto_account_read {
    /* Raw 8 bytes of the ACCESS chunk (memcpy'd verbatim). */
    uint8_t access[8];
    /* 0/1 — set iff the ACCESS chunk was present and at least 8 bytes.
     * The call site uses this as the dispatch gate (matches the C
     * extractor's accessbool). */
    uint8_t got_access;
    /* Bytes written to the three string buffers, excluding NUL. */
    uint16_t name_len;
    uint16_t login_len;
    uint16_t pass_len;
};

/* Parse the post-HTLC_HDR_ACCOUNT_READ TASK reply payload (the
 * rcv_task_user_open body). Writes NAME (raw, no strip_ansi),
 * XOR-0xff-decoded LOGIN, and XOR-0xff-decoded PASSWORD into their
 * respective buffers (NUL-terminated, capped at the matching _cap-1).
 * PASSWORD no-password convention: a single zero byte (or empty)
 * yields an empty pass buffer — matches the C extractor's
 * `plen > 1 && dh->data[0]` gate. ACCESS lands in out->access (8
 * bytes); out->got_access is the dispatch gate. Returns false on any
 * NULL / zero-cap pointer; otherwise true. */
extern bool gtkhx_proto_parse_account_read (const uint8_t *msg, size_t msglen,
                                            uint8_t *name_buf, size_t name_cap,
                                            uint8_t *login_buf, size_t login_cap,
                                            uint8_t *pass_buf, size_t pass_cap,
                                            struct gtkhx_proto_account_read *out);

/* ---- Xfer-reply parsers (post-TASK payloads on FILE_GET / FOLDER_GET /
 * FILE_GETINFO replies) ---- */

struct gtkhx_proto_file_get_reply {
    uint32_t ref_;
    uint32_t size;
    uint64_t size64;
    uint32_t queue;
    /* 0/1 — set iff the XFERSIZE64 companion chunk was present and
     * carried at least 8 bytes (the parser reads the first 8 BE
     * bytes; trailing bytes are ignored, matching the C `_len >= 8`
     * gate). Callers prefer size64 when set. */
    uint8_t size64_seen;
};

/* Parse the FILE_GET reply scalars (HTXF_REF + HTXF_SIZE + optional
 * XFERSIZE64 + optional QUEUE). Missing chunks default to zero; the
 * caller applies the C extractor's `(!size && !size64_seen) || !ref`
 * dispatch gate. Returns false on NULL out; otherwise true. */
extern bool gtkhx_proto_parse_file_get_reply (
    const uint8_t *msg, size_t msglen,
    struct gtkhx_proto_file_get_reply *out);

struct gtkhx_proto_folder_get_reply {
    uint32_t ref_;
    uint32_t size;
    uint64_t size64;
    uint32_t queue;
    uint32_t nfiles;
    uint8_t size64_seen;
};

/* Parse the FOLDER_GET reply scalars. Same contract as
 * gtkhx_proto_parse_file_get_reply with the addition of FILE_NFILES. */
extern bool gtkhx_proto_parse_folder_get_reply (
    const uint8_t *msg, size_t msglen,
    struct gtkhx_proto_folder_get_reply *out);

struct gtkhx_proto_file_put_reply {
    uint32_t ref_;
    uint32_t queue;
    /* Fork resume offsets parsed from the optional RFLT payload.
     * Zero when no RFLT was sent or it was shorter than 66 bytes
     * (the C extractor's gate; RFLT carries data_pos at +46 and
     * rsrc_pos at +62, both BE u32). */
    uint32_t data_pos;
    uint32_t rsrc_pos;
};

/* Parse the FILE_PUT reply scalars. Returns false on NULL out;
 * otherwise true (missing chunks default to zero — caller applies
 * the `!ref` dispatch gate). */
extern bool gtkhx_proto_parse_file_put_reply (
    const uint8_t *msg, size_t msglen,
    struct gtkhx_proto_file_put_reply *out);

struct gtkhx_proto_folder_put_reply {
    uint32_t ref_;
    uint32_t queue;
};

/* Parse the FOLDER_PUT reply scalars (strict subset of
 * gtkhx_proto_parse_file_put_reply — no RFLT; per-file resume
 * happens inside folder_put_thread, not at the task boundary). */
extern bool gtkhx_proto_parse_folder_put_reply (
    const uint8_t *msg, size_t msglen,
    struct gtkhx_proto_folder_put_reply *out);

struct gtkhx_proto_banner_get_reply {
    uint32_t ref_;
    uint32_t size;
};

/* Parse the DOWNLOAD_BANNER reply scalars. Just the transfer
 * reference + total byte count for the HTXF subchannel fetch
 * banner.c spins up. */
extern bool gtkhx_proto_parse_banner_get_reply (
    const uint8_t *msg, size_t msglen,
    struct gtkhx_proto_banner_get_reply *out);

struct gtkhx_proto_news_thread_reply {
    uint32_t thread_id;
    /* Bytes written to text_buf, excluding the trailing NUL. */
    uint16_t text_len;
    /* 0/1 — set iff a NEWSDATA chunk was present and no TASK_ERROR
     * short-circuited the walk. Caller bails when zero. Empty
     * NEWSDATA still counts as has_text=1 (text_len may be 0). */
    uint8_t has_text;
    /* 0/1 — set iff a TASK_ERROR chunk was seen mid-walk. */
    uint8_t has_task_error;
};

/* Parse the post-HTLC_HDR_GETTHREAD TASK reply payload — the
 * server's response arrives inside an HTLS_HDR_TASK frame; this
 * parses the post-TASK body (the rcv_task_news_post body in C).
 * Writes the CR2LF + strip_ansi'd NEWSDATA body into text_buf
 * (NUL-terminated, capped at text_cap-1) and fills *out. Returns
 * false on NULL out, NULL text_buf, or zero text_cap; otherwise true.
 * The out->has_text gate filters out the missing-body and TASK_ERROR
 * paths the way the C extractor does. */
extern bool gtkhx_proto_parse_news_thread_reply (
    const uint8_t *msg, size_t msglen,
    uint8_t *text_buf, size_t text_cap,
    struct gtkhx_proto_news_thread_reply *out);

/* ---- Chat-history extension (HTLS_DATA_HISTORY_ENTRY) ---- */

struct gtkhx_proto_history_entry {
    uint64_t message_id;
    /* i64 on the wire (Unix epoch UTC). Two's-complement preserved;
     * negative values are legal pre-1970 timestamps. */
    int64_t timestamp;
    uint16_t flags;     /* HX_HISTORY_FLAG_* */
    uint16_t icon_id;
    /* nick / message land in (offset, length) pairs into the
     * caller's `data` buffer — the call site allocates owned
     * copies by length (g_malloc + memcpy + trailing NUL), NOT
     * via g_strndup: payloads can contain embedded NULs (the
     * server has no obligation to scrub them) and g_strndup
     * would stop at the first one, leaving the allocation
     * shorter than the recorded *_len. The wire bytes are NOT
     * NUL-terminated; server has already transcoded to the
     * negotiated encoding. */
    uint16_t nick_off;
    uint16_t nick_len;
    uint16_t msg_off;
    uint16_t msg_len;
};

/* Pin the C-ABI mirror size so any padding / alignment drift across
 * compilers or targets is caught at build time rather than turning
 * into memory corruption on the Rust side (the #[repr(C)] mirror
 * HistoryEntryOut in rust/crates/hotline-proto/src/ffi.rs has to
 * match exactly). Layout: u64 (8) + i64 (8) + 6×u16 (flags +
 * icon_id + nick_off + nick_len + msg_off + msg_len = 12) =
 * 28 bytes of data + 4 bytes of trailing alignment-to-8 padding
 * = 32 bytes. Same discipline as chacha_aead_state in
 * src/cipher.h. */
_Static_assert (sizeof (struct gtkhx_proto_history_entry) == 32,
                "gtkhx_proto_history_entry size drifted from Rust ABI mirror");

/* Parse one HTLS_DATA_HISTORY_ENTRY chunk body (chat-history
 * extension). Returns false on NULL out or a malformed packed
 * record (buffer < 24 bytes, declared nick_len or msg_len runs
 * past the buffer); otherwise true. Mini-TLV sub-fields after the
 * message body are walked past silently — v1 defines no sub-types
 * and a malformed sub-field stops the walk but the entry is still
 * returned. */
extern bool gtkhx_proto_parse_history_entry (const uint8_t *data, size_t len,
                                              struct gtkhx_proto_history_entry *out);

/* ---- HTLS_DATA_FILE_LIST entry walker ---- */

struct gtkhx_proto_file_list_entry {
    uint32_t ftype;    /* FourCC, e.g. "fldr" / "TEXT" / "JPEG" */
    uint32_t fcreator; /* FourCC */
    uint32_t fsize;    /* bytes (or item count for folders) */
    uint32_t fnlen;
    /* Offset of filename bytes within the caller's `data` buffer
     * (relative to `data`, not relative to the chunk start). */
    size_t name_off;
    size_t name_len;
    /* Where the next chunk begins; pass back as the next call's
     * `off`. Only meaningful when the parse returns true — a false
     * return means either end-of-buffer OR a malformed chunk, and
     * the caller can't tell which from next_off alone (it isn't
     * written on the failure path). Iteration just stops at the
     * first false return; callers that need to distinguish a
     * clean end-of-buffer from a corrupt entry must inspect the
     * remaining `len - off` bytes themselves. */
    size_t next_off;
};

/* Parse one packed HTLS_DATA_FILE_LIST entry starting at
 * data[off]. Caller iterates: off = 0; while
 * (gtkhx_proto_parse_file_list_entry (data, len, off, &out)) {
 *     use out; off = out.next_off; }
 *
 * Returns true on success with *out filled; false at end-of-buffer
 * or on a malformed chunk (< 24 bytes remaining, declared chunk
 * length runs past the buffer, fnlen runs past the chunk). */
extern bool gtkhx_proto_parse_file_list_entry (const uint8_t *data, size_t len,
                                                size_t off,
                                                struct gtkhx_proto_file_list_entry *out);

struct gtkhx_proto_file_getinfo {
    uint8_t icon[4];
    uint8_t date_create[8];
    uint8_t date_modify[8];
    uint32_t size;
    uint64_t size64;
    uint8_t size64_seen;
    uint8_t got_icon;
    /* Bytes written to the four string buffers (excluding trailing NUL). */
    uint16_t name_len;
    uint16_t type_len;
    uint16_t creator_len;
    uint16_t comment_len;
};

/* Parse the FILE_GETINFO reply (the file-info dialog payload).
 * Writes strip_ansi'd FILE_NAME into name_buf, FILE_TYPE / FILE_CREATOR
 * (4-byte HFS codes, typically) into their buffers, and CR2LF +
 * strip_ansi'd FILE_COMMENT into comment_buf — all NUL-terminated and
 * capped at the matching _cap-1. FILE_ICON / FILE_DATE_CREATE /
 * FILE_DATE_MODIFY land in fixed-size byte arrays in *out. Returns
 * false on any NULL / zero-cap pointer; otherwise true. */
extern bool gtkhx_proto_parse_file_getinfo (
    const uint8_t *msg, size_t msglen,
    uint8_t *name_buf, size_t name_cap,
    uint8_t *type_buf, size_t type_cap,
    uint8_t *creator_buf, size_t creator_cap,
    uint8_t *comment_buf, size_t comment_cap,
    struct gtkhx_proto_file_getinfo *out);

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

/* HTLC_HDR_NEWS_POST (1.0 flat news): single body chunk. No scratch.
 * chunks_cap >= 1. Returns 1 on success, or 0 on validation failure
 * (NULL pointer, short buffer, body_len > u16 max). */
extern int32_t gtkhx_proto_build_news_post_chunks (const uint8_t *body_ptr,
                                                   size_t body_len,
                                                   struct hx_chunk *chunks,
                                                   size_t chunks_cap);

/* HTLC_HDR_NEWSCATLIST: single HTLC_DATA_NEWSPATH chunk. chunks_cap >= 1.
 * Returns 1 on success, or 0 on validation failure. */
extern int32_t gtkhx_proto_build_news_catlist_chunks (const uint8_t *path_ptr,
                                                      size_t path_len,
                                                      struct hx_chunk *chunks,
                                                      size_t chunks_cap);

/* HTLC_HDR_NEWSDIRLIST: same shape as NEWSCATLIST. */
extern int32_t gtkhx_proto_build_news_dirlist_chunks (const uint8_t *path_ptr,
                                                      size_t path_len,
                                                      struct hx_chunk *chunks,
                                                      size_t chunks_cap);

/* HTLC_HDR_DELNEWSDIRCAT: same shape as NEWSCATLIST (path tells the
 * server whether to delete a folder or a category). */
extern int32_t gtkhx_proto_build_news_delete_chunks (const uint8_t *path_ptr,
                                                     size_t path_len,
                                                     struct hx_chunk *chunks,
                                                     size_t chunks_cap);

/* HTLC_HDR_MAKENEWSDIR: same shape as NEWSCATLIST (the last path
 * component is the new directory name). */
extern int32_t gtkhx_proto_build_news_mkdir_chunks (const uint8_t *path_ptr,
                                                    size_t path_len,
                                                    struct hx_chunk *chunks,
                                                    size_t chunks_cap);

/* HTLC_HDR_DELETETHREAD: NEWSPATH + THREADID (u32). chunks_cap >= 2,
 * scratch_cap >= 4. Returns 2 on success, 0 on validation failure. */
extern int32_t gtkhx_proto_build_news_delete_thread_chunks (
    const uint8_t *path_ptr, size_t path_len, uint32_t threadid,
    struct hx_chunk *chunks, size_t chunks_cap,
    uint8_t *scratch, size_t scratch_cap);

/* HTLC_HDR_GETTHREAD: NEWSPATH + THREADID + NEWSTYPE.
 * chunks_cap >= 3, scratch_cap >= 4. Returns 3 on success, 0 on
 * validation failure. */
extern int32_t gtkhx_proto_build_news_getthread_chunks (
    const uint8_t *path_ptr, size_t path_len, uint32_t threadid,
    const uint8_t *mime_type_ptr, size_t mime_type_len,
    struct hx_chunk *chunks, size_t chunks_cap,
    uint8_t *scratch, size_t scratch_cap);

/* HTLC_HDR_MAKECATEGORY: NEWSPATH + CATEGORY name. chunks_cap >= 2.
 * No scratch. Returns 2 on success, 0 on validation failure. */
extern int32_t gtkhx_proto_build_news_mkcat_chunks (
    const uint8_t *path_ptr, size_t path_len,
    const uint8_t *name_ptr, size_t name_len,
    struct hx_chunk *chunks, size_t chunks_cap);

/* HTLC_HDR_POSTTHREAD: 6 chunks in wire order — NEWSPATH +
 * PARENTTHREAD (u32) + NEWSTYPE + NEWSSUBJECT + NEWSDATA + THREADID
 * (u32). chunks_cap >= 6, scratch_cap >= 8. Returns 6 on success, 0
 * on validation failure. */
extern int32_t gtkhx_proto_build_news_post_thread_chunks (
    const uint8_t *path_ptr, size_t path_len, uint32_t parent_thread,
    const uint8_t *mime_type_ptr, size_t mime_type_len,
    const uint8_t *subject_ptr, size_t subject_len,
    const uint8_t *text_ptr, size_t text_len, uint32_t thread_id,
    struct hx_chunk *chunks, size_t chunks_cap,
    uint8_t *scratch, size_t scratch_cap);

/* HTLC_HDR_FILE_MKDIR: single HTLC_DATA_DIR chunk. chunks_cap >= 1.
 * No scratch needed. Returns 1 on success, 0 on validation failure. */
extern int32_t gtkhx_proto_build_file_mkdir_chunks (const uint8_t *dir_ptr,
                                                    size_t dir_len,
                                                    struct hx_chunk *chunks,
                                                    size_t chunks_cap);

/* HTLC_HDR_FILE_LIST: single HTLC_DATA_DIR chunk. Wire-identical to
 * FILE_MKDIR. chunks_cap >= 1. Returns 1 on success, 0 on validation
 * failure. */
extern int32_t gtkhx_proto_build_file_list_chunks (const uint8_t *dir_ptr,
                                                   size_t dir_len,
                                                   struct hx_chunk *chunks,
                                                   size_t chunks_cap);

/* HTLC_HDR_FILE_DELETE: FILE_NAME + optional DIR. has_dir is a 0/1
 * flag — when non-zero, emit DIR with the given bytes; when zero,
 * omit (dir_ptr / dir_len are ignored). chunks_cap >= 2.
 * Returns 1 (no dir) or 2 (with dir) on success, 0 on validation
 * failure (NULL pointer, short buffer, oversize field). */
extern int32_t gtkhx_proto_build_file_delete_chunks (const uint8_t *name_ptr,
                                                     size_t name_len,
                                                     uint8_t has_dir,
                                                     const uint8_t *dir_ptr,
                                                     size_t dir_len,
                                                     struct hx_chunk *chunks,
                                                     size_t chunks_cap);

/* HTLC_HDR_FILE_GETINFO: same shape as FILE_DELETE. */
extern int32_t gtkhx_proto_build_file_getinfo_chunks (const uint8_t *name_ptr,
                                                      size_t name_len,
                                                      uint8_t has_dir,
                                                      const uint8_t *dir_ptr,
                                                      size_t dir_len,
                                                      struct hx_chunk *chunks,
                                                      size_t chunks_cap);

/* HTLC_HDR_FILE_GETFOLDER: same shape as FILE_DELETE. */
extern int32_t gtkhx_proto_build_file_getfolder_chunks (const uint8_t *name_ptr,
                                                        size_t name_len,
                                                        uint8_t has_dir,
                                                        const uint8_t *dir_ptr,
                                                        size_t dir_len,
                                                        struct hx_chunk *chunks,
                                                        size_t chunks_cap);

/* HTLC_HDR_FILE_SETINFO: FILE_NAME + FILE_RENAME + optional
 * FILE_COMMENT + optional DIR. has_comment / has_dir are 0/1 flags;
 * when zero the corresponding payload pointer/len are ignored.
 * chunks_cap >= 4 (the shim always slices to the full setinfo size).
 * Returns 2..=4 on success, 0 on validation failure (NULL pointer,
 * short buffer, oversize field). */
extern int32_t gtkhx_proto_build_file_setinfo_chunks (
    const uint8_t *name_ptr, size_t name_len,
    const uint8_t *rename_ptr, size_t rename_len,
    uint8_t has_comment, const uint8_t *comment_ptr, size_t comment_len,
    uint8_t has_dir, const uint8_t *dir_ptr, size_t dir_len,
    struct hx_chunk *chunks, size_t chunks_cap);

/* HTLC_HDR_FILE_MOVE: FILE_NAME + DIR + DIR_RENAME. chunks_cap >= 3.
 * Returns 3 on success, 0 on validation failure. */
extern int32_t gtkhx_proto_build_file_move_chunks (
    const uint8_t *name_ptr, size_t name_len,
    const uint8_t *dir_ptr, size_t dir_len,
    const uint8_t *dir_rename_ptr, size_t dir_rename_len,
    struct hx_chunk *chunks, size_t chunks_cap);

/* HTLC_HDR_FILE_SYMLINK: FILE_NAME + DIR + DIR_RENAME + FILE_RENAME.
 * chunks_cap >= 4. Returns 4 on success, 0 on validation failure. */
extern int32_t gtkhx_proto_build_file_symlink_chunks (
    const uint8_t *name_ptr, size_t name_len,
    const uint8_t *dir_ptr, size_t dir_len,
    const uint8_t *dir_rename_ptr, size_t dir_rename_len,
    const uint8_t *rename_ptr, size_t rename_len,
    struct hx_chunk *chunks, size_t chunks_cap);

/* HTLC_HDR_FILE_PUTFOLDER: FILE_NAME + optional DIR + HTXF_SIZE (u32
 * BE, host order in) + FILE_NFILES (u32 BE, host order in).
 * chunks_cap >= 4, scratch_cap >= 8. Returns 3 (no dir) or 4 (with
 * dir) on success, 0 on validation failure. */
extern int32_t gtkhx_proto_build_file_putfolder_chunks (
    const uint8_t *name_ptr, size_t name_len,
    uint8_t has_dir, const uint8_t *dir_ptr, size_t dir_len,
    uint32_t size, uint32_t nfiles,
    struct hx_chunk *chunks, size_t chunks_cap,
    uint8_t *scratch, size_t scratch_cap);

/* HTLC_HDR_FILE_GET: FILE_NAME + optional DIR + optional RFLT (74
 * bytes — fixed-size resume payload built by the C caller).
 * has_dir / has_rflt are 0/1 flags; rflt_ptr must point to exactly
 * 74 bytes when has_rflt is set. chunks_cap >= 3. Returns 1..=3 on
 * success, 0 on validation failure. */
extern int32_t gtkhx_proto_build_file_get_chunks (
    const uint8_t *name_ptr, size_t name_len,
    uint8_t has_dir, const uint8_t *dir_ptr, size_t dir_len,
    uint8_t has_rflt, const uint8_t *rflt_ptr,
    struct hx_chunk *chunks, size_t chunks_cap);

/* HTLC_HDR_FILE_PUT: FILE_NAME + optional DIR + optional FILE_PREVIEW
 * (2 bytes "\0\1", set when overwriting an existing remote file) +
 * HTXF_SIZE (u32 BE, host order in) + optional XFERSIZE64 (u64 BE,
 * host order in; large-files mode). has_dir / has_preview /
 * has_size64 are 0/1 flags. chunks_cap >= 5, scratch_cap >= 12 (u32
 * at +0, u64 at +4). Returns 2..=5 on success, 0 on validation
 * failure. */
extern int32_t gtkhx_proto_build_file_put_chunks (
    const uint8_t *name_ptr, size_t name_len,
    uint8_t has_dir, const uint8_t *dir_ptr, size_t dir_len,
    uint8_t has_preview, uint32_t size,
    uint8_t has_size64, uint64_t size64,
    struct hx_chunk *chunks, size_t chunks_cap,
    uint8_t *scratch, size_t scratch_cap);

/* ---- HTRK (Hotline tracker, v1) reply parsers ---- */

/* Parse the 14-byte HTRK reply header. Writes nservers (host byte
 * order) into *out_nservers. Returns false on NULL out_nservers
 * or a buffer shorter than 14; otherwise true. */
extern bool gtkhx_proto_parse_tracker_header (const uint8_t *buf, size_t len,
                                               uint16_t *out_nservers);

/* True iff buf[0] == 0 — the HTRK padding-slot marker the async
 * fetch state machine skips without advancing the record counter.
 * False on empty input. */
extern bool gtkhx_proto_tracker_record_is_padding (const uint8_t *buf,
                                                    size_t len);

struct gtkhx_proto_tracker_record_fixed {
    /* 4 IPv4 address bytes verbatim from the wire. memcpy straight
     * into struct in_addr's s_addr (same network-byte-order storage
     * convention). */
    uint32_t addr_be;
    uint16_t port;       /* host byte order */
    uint16_t nusers;     /* host byte order */
    uint8_t name_len;
};

/* Pin the C-ABI mirror size so any padding drift across compilers
 * or targets surfaces at build time rather than memory corruption
 * on the Rust side. Layout: u32 + 2×u16 + u8 = 9 bytes of data +
 * 3 bytes of trailing alignment-to-4 padding = 12 bytes. */
_Static_assert (sizeof (struct gtkhx_proto_tracker_record_fixed) == 12,
                "gtkhx_proto_tracker_record_fixed size drifted from Rust ABI mirror");

/* Parse the 11-byte fixed prefix of a HTRK server record. Returns
 * false on NULL out or a buffer shorter than 11; otherwise true.
 * Bytes 8 and 9 are spec-reserved and not surfaced; byte 10 is
 * name_len (returned in *out). */
extern bool gtkhx_proto_parse_tracker_record_fixed (
    const uint8_t *buf, size_t len,
    struct gtkhx_proto_tracker_record_fixed *out);

/* Normalize a server name or description in place: CR (0x0D) → LF
 * (0x0A); strip_ansi folds C0 control bytes (ESC etc. in the
 * 14..30 band, minus the {15, 22} exception set) to printable
 * ASCII via (c & 127) | 64 — buffer length unchanged. No-op on
 * NULL buf or zero len. */
extern void gtkhx_proto_tracker_normalize_text (uint8_t *buf, size_t len);

/* ---- HTRK v3 (newer tracker protocol) ---- */

/* 8-byte client-side handshake builder. Writes "HTRK" + version
 * (0x0003 BE) + features (BE). Returns false on NULL out or
 * out_len < 8; otherwise true. */
extern bool gtkhx_proto_tracker_v3_pack_handshake (uint8_t *out, size_t out_len,
                                                    uint16_t features);

/* Parse the tracker's handshake response. State machine reads 6
 * bytes first; if version comes back as v3 it reads the trailing
 * 2 and calls us again with len == 8. Returns false on NULL out
 * pointers, wrong length (must be 6 or 8), or bad magic. The
 * 6-byte form leaves *features_out = 0. */
extern bool gtkhx_proto_tracker_v3_parse_handshake_response (
    const uint8_t *buf, size_t len,
    uint16_t *version_out, uint16_t *features_out);

/* Build the 4-byte minimum listing-request body (request_type =
 * 0x0001 + field_count = 0). Writes byte count actually written
 * (always 4 on success) into *out_written. Returns false on NULL
 * pointers or out_len < 4. */
extern bool gtkhx_proto_tracker_v3_pack_listing_request_simple (
    uint8_t *out, size_t out_len, size_t *out_written);

/* Parse the 10-byte listing-response header. Returns false on
 * NULL out pointers, short buffer, or a response_type that isn't
 * HTRK_V3_RESP_LIST (0x0001). */
extern bool gtkhx_proto_tracker_v3_parse_response_header (
    const uint8_t *buf, size_t len,
    uint16_t *response_type_out, uint32_t *total_size_out,
    uint16_t *total_servers_out, uint16_t *record_count_out);

struct gtkhx_proto_tracker_v3_record {
    /* Offsets into the caller's `buf` argument. Lengths give the
     * slice extents. Caller dereferences as `buf + off` for each
     * of address / name / desc / tlv_bytes. */
    size_t addr_off;
    size_t addr_len;
    size_t name_off;
    size_t name_len;
    size_t desc_off;
    size_t desc_len;
    size_t tlv_off;
    size_t tlv_len;
    /* Bytes this record occupied — advance off by this for the
     * next record. */
    size_t consumed;
    uint16_t port;
    uint16_t nusers;
    uint16_t tlv_count;
    uint8_t addr_type;
};

/* Parse one tracker v3 server record at buf[off..]. Returns false
 * on truncation, an unknown addr_type, or any declared length that
 * overruns the buffer. */
extern bool gtkhx_proto_tracker_v3_parse_record (
    const uint8_t *buf, size_t len, size_t off,
    struct gtkhx_proto_tracker_v3_record *out);

struct gtkhx_proto_tracker_v3_tlv {
    size_t value_off;
    size_t value_len;
    size_t next_off;
    uint16_t id;
};

/* Parse the next TLV at buf[off..]. Returns false on a short
 * buffer (< 4 bytes for the id+len header) or when the declared
 * value_len runs past the buffer. The hx_tracker_v3_walk_tlvs C
 * wrapper iterates this and fires its callback per entry. */
extern bool gtkhx_proto_tracker_v3_parse_tlv_at (
    const uint8_t *buf, size_t len, size_t off,
    struct gtkhx_proto_tracker_v3_tlv *out);

/* ---- HTRK v3 meta TLV typed readers ----
 *
 * Wire-format-strict fail-closed scalar extractors for the
 * per-record TLV trailer. "Wrong size" — anything other than the
 * exact spec-mandated width — returns the supplied default rather
 * than silently decoding partial bytes. Strings stay in C
 * (g_utf8_make_valid + g_strndup); these cover only the numeric /
 * bool / enum-clamp half. */

extern uint8_t gtkhx_proto_tracker_v3_meta_read_u8 (
    const uint8_t *value, size_t value_len, uint8_t default_);
extern uint16_t gtkhx_proto_tracker_v3_meta_read_u16 (
    const uint8_t *value, size_t value_len, uint16_t default_);
extern int16_t gtkhx_proto_tracker_v3_meta_read_i16 (
    const uint8_t *value, size_t value_len, int16_t default_);
extern uint32_t gtkhx_proto_tracker_v3_meta_read_u32 (
    const uint8_t *value, size_t value_len, uint32_t default_);
extern bool gtkhx_proto_tracker_v3_meta_read_bool (
    const uint8_t *value, size_t value_len);

/* Closed-vocab enum clamps. raw values inside the spec-defined
 * range pass through; out-of-range values reset to 0 (GENERAL /
 * UNSPECIFIED). Spec rule for forward-compat. */
extern uint8_t gtkhx_proto_tracker_v3_meta_clamp_maturity (uint8_t raw);
extern uint8_t gtkhx_proto_tracker_v3_meta_clamp_listing_category (uint8_t raw);

/* ---- Text encoding: Mac Roman -> UTF-8 ---- */

/* Decode `src[0..len)` wire bytes into UTF-8 in `dst`, writing into the
 * half-open range `dst[0..returned)`. Returns the number of bytes
 * written (always <= cap). Mirrors src/text_util.c::gtkhx_text_to_utf8's
 * decode rule: valid UTF-8 passes through verbatim (including any
 * embedded NULs); non-UTF-8 input is decoded byte-by-byte through the
 * glibc MACINTOSH table.
 *
 * `src` and `dst` are independent buffers — the decode is src → dst,
 * not in place. They must not overlap.
 *
 * Worst-case Mac Roman → UTF-8 expansion is 3×. With cap >= len * 3 the
 * whole decoded output fits. With a smaller cap the result is truncated
 * at the last UTF-8 character boundary that still fits (never writes a
 * partial multi-byte sequence).
 *
 * No-op returns (returns 0 without writing):
 *   - dst == NULL, OR
 *   - cap == 0, OR
 *   - cap > isize::MAX (would violate Rust's slice ceiling), OR
 *   - src == NULL — treated as empty input regardless of `len`, so
 *     even a non-zero `len` is safe with a NULL pointer. (Same goes
 *     for `len > isize::MAX`: the input is treated as empty.)
 *
 * No trailing NUL is appended; `dst[returned]` is untouched. Decoded
 * output may legitimately contain embedded NULs (when the input was
 * already valid UTF-8 with NULs), so this FFI does not own NUL
 * accounting.
 *
 * For a NUL-terminated C string, allocate `len * 3 + 1` bytes,
 * pass `cap = len * 3` to reserve the trailing byte as the NUL slot,
 * then write `'\0'` to `dst[returned]` after the call. With
 * cap = len * 3, returned is at most len * 3, so dst[returned] is
 * always in bounds. */
extern size_t gtkhx_proto_text_to_utf8 (const uint8_t *src, size_t len,
                                        uint8_t *dst, size_t cap);

#endif /* _HOTLINE_PROTO_H */
