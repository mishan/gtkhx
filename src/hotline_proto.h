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
 * FFI prototypes for the Phase R2 `hxproto` Rust crate
 * (the hxproto crate in hx-libs). These are hand-declared rather than
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

#ifndef GTKHX_HOTLINE_PROTO_H
#define GTKHX_HOTLINE_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* True if the transaction header's task-error bit is set
 * (g_ntohl(flag) & 1). Buffer is htlc->in.buf / htlc->in.pos. */
extern bool gtkhx_proto_header_in_error (const uint8_t *buf, size_t len);

/* Extract the transaction id into *out_trans. Returns true on success,
 * false (leaving *out_trans untouched) on a short buffer. */
extern bool gtkhx_proto_header_trans (const uint8_t *buf, size_t len,
                                      uint32_t *out_trans);

/* ---- LOGIN task-reply parser ----
 *
 * Every field of the LOGIN reply is independently optional on the wire (a
 * 1.0/1.2 server sends almost none of them). gtkhx_proto_parse_login walks
 * the chunks, fills *out with whatever scalars were present, writes the
 * sanitised server name into a caller buffer, and returns a bitmask of the
 * fields it saw. Read each *out field only when its HX_LOGIN_SEEN_* bit is
 * set. */

enum {
    HX_LOGIN_SEEN_UID = 1u << 0,
    HX_LOGIN_SEEN_VERSION = 1u << 1,
    HX_LOGIN_SEEN_SERVERNAME = 1u << 2,
    HX_LOGIN_SEEN_CAPS = 1u << 3,
    HX_LOGIN_SEEN_MEDIA_MAX_BYTES = 1u << 4,
    HX_LOGIN_SEEN_MEDIA_MAX_DIMENSION = 1u << 5,
    HX_LOGIN_SEEN_MEDIA_MAX_PIXELS = 1u << 6,
    HX_LOGIN_SEEN_MEDIA_CHUNK_SIZE = 1u << 7,
    HX_LOGIN_SEEN_MEDIA_MAX_FRAMES = 1u << 8,
    HX_LOGIN_SEEN_MEDIA_MAX_DURATION_MS = 1u << 9,
    HX_LOGIN_SEEN_HISTORY_MAX_MSGS = 1u << 10,
    HX_LOGIN_SEEN_HISTORY_MAX_DAYS = 1u << 11,
    HX_LOGIN_SEEN_VIDEO_CAMERA_LIMITS = 1u << 12,
    HX_LOGIN_SEEN_VIDEO_SCREEN_LIMITS = 1u << 13,
};

/* One kind's DATA_VIDEO_LIMITS (mirror of LoginVideoLimits). */
struct gtkhx_proto_login_video_limits {
    uint16_t max_width;
    uint16_t max_height;
    uint16_t max_fps;
    uint16_t max_per_room;
    uint32_t max_bitrate;
};

struct gtkhx_proto_login {
    uint64_t caps; /* decoded capabilities bitmap */
    uint32_t media_max_bytes;
    uint32_t media_max_dimension;
    uint32_t media_max_pixels;
    uint32_t media_chunk_size;
    uint32_t media_max_frames;
    uint32_t media_max_duration_ms;
    uint32_t history_max_msgs;
    uint32_t history_max_days;
    uint16_t uid;
    uint16_t version;
    /* Camera then screen; each valid when its
     * HX_LOGIN_SEEN_VIDEO_*_LIMITS bit is set. */
    struct gtkhx_proto_login_video_limits video_limits[2];
};

/* Parse the LOGIN task reply. Fills *out and writes the CR2LF'd +
 * strip_ansi'd server name into servername (capacity servername_cap,
 * NUL-terminated, capped at servername_cap-1). Returns the HX_LOGIN_SEEN_*
 * bitmask; each *out field is valid only when its bit is set. Returns 0 on
 * NULL out. A NULL / zero-capacity servername is tolerated (name skipped). */
extern uint32_t gtkhx_proto_parse_login (const uint8_t *msg, size_t msglen,
                                         uint8_t *servername,
                                         size_t servername_cap,
                                         struct gtkhx_proto_login *out);

/* ---- Chat-history extension (HTLS_DATA_HISTORY_ENTRY) ---- */

struct gtkhx_proto_history_entry {
    uint64_t message_id;
    /* i64 on the wire (Unix epoch UTC). Two's-complement preserved;
     * negative values are legal pre-1970 timestamps. */
    int64_t timestamp;
    uint16_t flags; /* HX_HISTORY_FLAG_* */
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
 * HistoryEntryOut in gtkhx_proto_ffi::ffi has to
 * match exactly). Layout: u64 (8) + i64 (8) + 6×u16 (flags +
 * icon_id + nick_off + nick_len + msg_off + msg_len = 12) =
 * 28 bytes of data + 4 bytes of trailing alignment-to-8 padding
 * = 32 bytes. */
_Static_assert (sizeof (struct gtkhx_proto_history_entry) == 32,
                "gtkhx_proto_history_entry size drifted from Rust ABI mirror");

/* Parse one HTLS_DATA_HISTORY_ENTRY chunk body (chat-history
 * extension). Returns false on NULL out or a malformed packed
 * record (buffer < 24 bytes, declared nick_len or msg_len runs
 * past the buffer); otherwise true. Mini-TLV sub-fields after the
 * message body are walked past silently — v1 defines no sub-types
 * and a malformed sub-field stops the walk but the entry is still
 * returned. */
extern bool
gtkhx_proto_parse_history_entry (const uint8_t *data, size_t len,
                                 struct gtkhx_proto_history_entry *out);

/* ---- Misc smaller parsers ---- */

/* Extract a HTLS_DATA_TASKERROR chunk's CR2LF + strip_ansi sanitised
 * text into *out (NUL-terminated, capped at cap-1). Returns the byte
 * count (excluding the NUL), or SIZE_MAX when no TASK_ERROR chunk was
 * present. Returns 0 on NULL out or zero cap. */
extern size_t gtkhx_proto_parse_task_error (const uint8_t *msg, size_t msglen,
                                            uint8_t *out, size_t cap);

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

/* HTLS_HDR_AGREEMENT result codes matching hx_agreement_result. */
#define GTKHX_PROTO_AGREEMENT_OK 0u
#define GTKHX_PROTO_AGREEMENT_NONE 1u
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

/* ---- SEND-path builders (HTLC_HDR_CHAT / _MSG / _MSG_BROADCAST) ----
 *
 * Each builder fills a caller-provided struct hx_chunk[] (and a
 * uint8_t scratch[] buffer for the integer chunks) and returns the
 * number of chunks populated, or 0 on validation failure. Production
 * callers hand the chunks array to hlwrite_chunks() for actual wire
 * encoding — cipher, compression, and fd dispatch all stay in C.
 *
 * Both the chunks buffer and the scratch buffer must outlive the
 * eventual hlwrite_chunks() call: the chunk data pointers reference
 * into scratch (for integer fields) and into the caller's body
 * buffer (for variable-length payloads).
 *
 * Text encoding (UTF-8 vs Mac Roman per CAP_TEXT_ENCODING) is the
 * caller's responsibility — gtkhx_text_for_wire is C-side and keeps
 * the Rust crate free of iconv. */

/* Only the struct's tag is referenced in the prototypes below (no
 * field access). Callers that need to stack-allocate `struct hx_chunk
 * chunks[N]` must #include "proto_helpers.h" directly. */
struct hx_chunk;

/* HTLC_HDR_CHAT: STYLE (u16) + CHAT body + CHAT_ID (u32, only when
 * cid != 0). Requires chunks_cap >= 3 and scratch_cap >= 6. Returns
 * 2 (no cid) or 3 (with cid) on success, or 0 on validation failure. */
extern int32_t
gtkhx_proto_build_chat_chunks (uint32_t cid, uint16_t style,
                               const uint8_t *body_ptr, size_t body_len,
                               struct hx_chunk *chunks, size_t chunks_cap,
                               uint8_t *scratch, size_t scratch_cap);

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
#define HX_AGREEMENT_AGREE_MAX_CHUNKS 3
#define HX_AGREEMENT_AGREE_SCRATCH_SIZE 16
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
extern int32_t gtkhx_proto_build_user_change_chunks (
    uint16_t icon, const uint8_t *name_ptr, size_t name_len,
    uint8_t has_nick_color, uint32_t nick_color, struct hx_chunk *chunks,
    size_t chunks_cap, uint8_t *scratch, size_t scratch_cap);

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
     * into a network-byte-order guint32 (same network-byte-order storage
     * convention). */
    uint32_t addr_be;
    uint16_t port;   /* host byte order */
    uint16_t nusers; /* host byte order */
    uint8_t name_len;
};

/* Pin the C-ABI mirror size so any padding drift across compilers
 * or targets surfaces at build time rather than memory corruption
 * on the Rust side. Layout: u32 + 2×u16 + u8 = 9 bytes of data +
 * 3 bytes of trailing alignment-to-4 padding = 12 bytes. */
_Static_assert (
    sizeof (struct gtkhx_proto_tracker_record_fixed) == 12,
    "gtkhx_proto_tracker_record_fixed size drifted from Rust ABI mirror");

/* Parse the 11-byte fixed prefix of a HTRK server record. Returns
 * false on NULL out or a buffer shorter than 11; otherwise true.
 * Bytes 8 and 9 are spec-reserved and not surfaced; byte 10 is
 * name_len (returned in *out). */
extern bool gtkhx_proto_parse_tracker_record_fixed (
    const uint8_t *buf, size_t len,
    struct gtkhx_proto_tracker_record_fixed *out);

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
extern bool
gtkhx_proto_tracker_v3_parse_handshake_response (const uint8_t *buf, size_t len,
                                                 uint16_t *version_out,
                                                 uint16_t *features_out);

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
    const uint8_t *buf, size_t len, uint16_t *response_type_out,
    uint32_t *total_size_out, uint16_t *total_servers_out,
    uint16_t *record_count_out);

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
extern bool
gtkhx_proto_tracker_v3_parse_record (const uint8_t *buf, size_t len, size_t off,
                                     struct gtkhx_proto_tracker_v3_record *out);

/* ---- HTLS_DATA_CAPABILITIES decode ---- */

/* Decode an HTLS_DATA_CAPABILITIES payload (1..8 bytes, big-endian,
 * MSB-first) into a u64. Payloads longer than 8 bytes are truncated at
 * the first 8; NULL `bytes` or zero `len` returns 0 (matching the bare-
 * advertisement convention pre-spec servers use). */
extern uint64_t gtkhx_proto_capabilities_decode (const uint8_t *bytes,
                                                 size_t len);

/* ---- Transaction header decode ----
 *
 * Companion to gtkhx_proto_header_trans / _header_in_error: the full
 * 6-output decode the C entry point hl_hdr_decode dispatches on. */

struct gtkhx_proto_header_decoded {
    uint32_t type_;
    uint32_t trans;
    uint32_t flag;
    /* Raw on-wire `len` field — passes through verbatim so production
     * logging can show the server's claim even when pathological. */
    uint32_t wire_len;
    /* Body bytes after `hc`, clamped at max_packet_len - sizeof(hc).
     * Wire `len` counts (body + hc=2); subtract 2 to get the body. */
    uint32_t body_len;
    uint16_t hc;
};

/* Pin the C-ABI mirror size so any padding / alignment drift across
 * compilers or targets surfaces at build time rather than reading
 * garbage on the Rust side. Layout: 5×u32 (20) + u16 (2) + 2 bytes
 * trailing alignment-to-4 padding = 24 bytes total. The Rust side
 * has a matching `const _: () = assert!(size_of == 24, ...)`. */
_Static_assert (sizeof (struct gtkhx_proto_header_decoded) == 24,
                "gtkhx_proto_header_decoded size drifted from Rust ABI mirror");

/* Decode the 22-byte transaction header. Fills *out with type / trans /
 * flag / hc / wire_len / body_len; returns false on NULL out or a
 * buffer shorter than the header. max_packet_len is the protocol-layer
 * packet ceiling (production passes MAX_HOTLINE_PACKET_LEN from
 * compat.h) and clamps body_len without touching wire_len. */
extern bool gtkhx_proto_decode_header (const uint8_t *buf, size_t len,
                                       uint32_t max_packet_len,
                                       struct gtkhx_proto_header_decoded *out);

/* ---- HTXF subframe header pack ---- */

/* Pack the 16-byte HTXF subframe header into `out[0..16)`. Wire layout
 * (big-endian throughout): magic ("HTXF") | ref | payload-len |
 * (type<<16)|flags. The trailing word is read as `type` by classic Mac-
 * native servers (high u16) and as `flags` by cap-aware peers (low u16);
 * both interpretations share the same wire bytes. Returns false (writes
 * nothing) when any of the following holds:
 *   - out == NULL
 *   - out_cap < 16        (no room for the header)
 *   - out_cap > SSIZE_MAX (Rust slice ceiling — protects the FFI from
 *                          UB when a buggy caller passes a garbage size)
 * Otherwise writes 16 bytes and returns true. */
extern bool gtkhx_proto_htxf_hdr_pack (uint8_t *out, size_t out_cap,
                                       uint32_t ref_id, uint32_t payload_len,
                                       uint16_t type_code, uint16_t flags);

/* ---- Full message packer (header + chunks → wire bytes) ----
 *
 * Pure serialization of a Hotline transaction message. The connection-
 * side concerns (qbuf growth on htlc->out, htlc->trans++ side effect)
 * stay in C in proto_helpers.c::hlpack_chunks; this Rust function does
 * the byte-twiddling.
 *
 * struct hx_chunk is the chunk-array element type already shared with
 * proto_helpers.h — its layout is { guint16 type; guint16 len;
 * const void *data; }, mirrored byte-for-byte by hxproto::build's
 * #[repr(C)] HxChunk (the Rust mirror calls the first field `tag` because
 * `type` is a Rust keyword, but the ABI is identical). */

struct hx_chunk; /* forward decl — defined in proto_helpers.h */

/* Total bytes a packed message with `chunks_len` chunks will occupy.
 * Caller uses this to size the destination buffer. Returns 0 on any of:
 *   - chunks == NULL && chunks_len != 0 (caller-side bug: fail closed
 *     rather than silently treat as empty and under-size the buffer)
 *   - chunks_len > MAX_PACK_CHUNKS (currently 64 — well above what any
 *     in-tree builder produces; surfaces a wildly pathological hc as a
 *     hard error)
 *   - chunks_len overflows the slice-byte limit (defensive)
 *
 * chunks_len == 0 always returns the header-only size (22), regardless
 * of whether chunks is NULL or a valid pointer. */
extern size_t gtkhx_proto_pack_message_size (const struct hx_chunk *chunks,
                                             size_t chunks_len);

/* Serialize `(type, trans, flag, chunks[0..chunks_len))` as a Hotline
 * transaction into `out[0..out_cap)`. Returns the number of bytes
 * written, or 0 on any of:
 *   - out == NULL or out_cap == 0
 *   - out_cap > SSIZE_MAX (Rust slice ceiling)
 *   - out_cap < pack_message_size (would truncate)
 *   - chunks == NULL && chunks_len != 0 (caller bug — fail closed)
 *   - chunks_len > MAX_PACK_CHUNKS (currently 64)
 *   - any chunk with len > 0 && data == NULL (caller bug — empty chunks
 *     must have len == 0 to skip the data deref)
 *
 * chunks_len == 0 packs a header-only message (22 bytes) regardless of
 * whether chunks is NULL or a valid pointer.
 *
 * Zero is unambiguous: the smallest legal packet is 22 bytes (a
 * header-only message), so a successful pack never returns 0. */
extern size_t gtkhx_proto_pack_message (uint8_t *out, size_t out_cap,
                                        uint32_t type, uint32_t trans,
                                        uint32_t flag,
                                        const struct hx_chunk *chunks,
                                        size_t chunks_len);

/* Pack a 22-byte Hotline transaction header into `dst` (the receive-side
 * counterpart to gtkhx_proto_pack_message). The hxnet actor already parsed the
 * header; the bridge calls this to reconstruct the byte-exact header into
 * htlc->in so the body handlers can decode it back out. `body_len` is the body
 * byte count after the header (excluding hc); the wire len/len2 fields encode
 * body_len + sizeof(hc). `dst` must be non-NULL and hold >= 22 writable bytes.
 * Passing NULL is a caller bug — the header simply won't be produced (the Rust
 * side returns early rather than crash), which surfaces downstream as a
 * malformed frame; do not rely on NULL as a "skip" path. */
extern void gtkhx_proto_pack_header (uint8_t *dst, uint32_t type,
                                     uint32_t trans, uint32_t flag, uint16_t hc,
                                     uint32_t body_len);

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

/* ---- Emoji shortcodes (phase E2/E3) ---- */

/* Rewrite emoji clusters in `src[0..len)` to `:shortcode:` text (ASCII,
 * e.g. 😂 → ":joy:"), writing UTF-8 into `dst`. Used by the legacy
 * (non-UTF-8) send path before Mac Roman conversion so emoji survive as
 * readable text instead of the '?' g_convert fallback.
 *
 * Returns the FULL number of bytes the output requires (snprintf-style),
 * which is NOT capped at `cap`. When `dst` is a usable buffer (see below),
 * a return value <= cap means the whole output was written; > cap means it
 * was truncated at a UTF-8 char boundary and the caller should re-allocate
 * to at least the returned size and call again. `:shortcode:` expansion is
 * unbounded per input byte (up to ~7×), which is why this reports a
 * required size rather than over-allocating like the 3× Mac Roman path
 * above.
 *
 * No-op-ish returns (nothing written, required length still returned —
 * so a <= cap return does NOT imply bytes were written in these cases):
 *   - `dst == NULL` or `cap == 0`, OR
 *   - `cap > isize::MAX` — the Rust shim refuses to build an oversize
 *     slice and treats `dst` as zero-capacity. Callers must size buffers
 *     well below this (chat text is microscopic by comparison).
 * `src == NULL` is treated as empty input regardless of `len`, and a
 * `len > isize::MAX` is likewise treated as empty. No trailing NUL is
 * appended. */
extern size_t gtkhx_proto_emoji_to_shortcodes (const uint8_t *src, size_t len,
                                               uint8_t *dst, size_t cap);

/* Prefix query for the emoji typeahead popup (phase E5). `prefix[0..len)`
 * is the partial shortcode name (no colons) the user has typed after an
 * opening colon, e.g. "jo". Writes up to `max` matches into `dst` as a run
 * of "name\temoji\n" records and returns the number of records written.
 * Records are whole-only — one that wouldn't fit is dropped and ends the
 * run — so size `dst` generously (max * 128 comfortably holds the longest
 * name plus any emoji). Ranking: exact match first, then shortest name,
 * then alphabetical. `dst == NULL`, `cap == 0`, `cap > isize::MAX`, or a
 * non-UTF-8 prefix all yield 0. No trailing NUL. */
extern size_t gtkhx_proto_shortcode_matches (const uint8_t *prefix, size_t len,
                                             uint8_t *dst, size_t cap,
                                             size_t max);

/* ---- Voice-chat extension (Phase 8.A) -----------------------------
 *
 * Builders for HTLC_HDR_VOICE_* and parsers for HTLS_HDR_VOICE_* /
 * VOICE_ROOM_STATUS / the JOIN reply, all defined in
 * hxproto::voice. The C-side wrapper sits
 * in src/voice.{h,c}; rcv.c dispatches the 600-606 family through
 * the parsers below.
 *
 * See docs/voice.md: the builder FFI shims are slated
 * for retirement once Phase 8.C lands the `hxvoice-runtime` crate
 * (the runtime can call the Rust builders directly). They stay for
 * Phase 8.A because there's no runtime crate to lean on yet. */

/* Build chunks for HTLC_HDR_VOICE_JOIN (600): one CHAT_ID chunk.
 * chunks_cap >= 1, scratch_cap >= 4. Returns chunk count (1) or 0. */
extern int32_t gtkhx_proto_build_voice_join_chunks (uint32_t cid,
                                                    struct hx_chunk *chunks,
                                                    size_t chunks_cap,
                                                    uint8_t *scratch,
                                                    size_t scratch_cap);

/* Build chunks for HTLC_HDR_VOICE_LEAVE (601): one CHAT_ID chunk. */
extern int32_t gtkhx_proto_build_voice_leave_chunks (uint32_t cid,
                                                     struct hx_chunk *chunks,
                                                     size_t chunks_cap,
                                                     uint8_t *scratch,
                                                     size_t scratch_cap);

/* Build chunks for HTLC_HDR_VOICE_SDP_ANSWER (603): CHAT_ID + VOICE_SDP.
 * Empty sdp rejected. chunks_cap >= 2, scratch_cap >= 4. */
extern int32_t
gtkhx_proto_build_voice_answer_chunks (uint32_t cid, const uint8_t *sdp_ptr,
                                       size_t sdp_len, struct hx_chunk *chunks,
                                       size_t chunks_cap, uint8_t *scratch,
                                       size_t scratch_cap);

/* Build chunks for HTLC_HDR_VOICE_ICE (604): CHAT_ID + VOICE_ICE.
 * ice_len 0 with NULL/non-NULL ice_ptr is the end-of-candidates marker. */
extern int32_t
gtkhx_proto_build_voice_ice_chunks (uint32_t cid, const uint8_t *ice_ptr,
                                    size_t ice_len, struct hx_chunk *chunks,
                                    size_t chunks_cap, uint8_t *scratch,
                                    size_t scratch_cap);

/* Build chunks for HTLC_HDR_VOICE_MUTE (606): CHAT_ID + VOICE_MUTED (u16).
 * chunks_cap >= 2, scratch_cap >= 6. The C caller normalises `muted` to
 * 0/1 before the call. */
extern int32_t
gtkhx_proto_build_voice_mute_chunks (uint32_t cid, uint16_t muted,
                                     struct hx_chunk *chunks, size_t chunks_cap,
                                     uint8_t *scratch, size_t scratch_cap);

/* C-ABI mirror of crate::voice::Participant. */
struct gtkhx_proto_voice_participant {
    uint16_t user_id;
    uint16_t flags;    /* bit 0 = muted, bits 1-15 reserved */
    uint16_t codec_id; /* 0 = PCMU; others reserved */
};
_Static_assert (sizeof (struct gtkhx_proto_voice_participant) == 6,
                "voice participant ABI is 6 bytes");

/* Walk the packed DATA_VOICE_PARTICIPANTS blob into a caller buffer.
 * Returns the number of entries written (capped at `cap`). Returns
 * 0 on NULL out or NULL blob with nonzero blob_len. */
extern size_t
gtkhx_proto_parse_voice_participants (const uint8_t *blob_ptr, size_t blob_len,
                                      struct gtkhx_proto_voice_participant *out,
                                      size_t cap);

/* Mid-label parse return codes. */
#define GTKHX_PROTO_VOICE_MID_INVALID 0
#define GTKHX_PROTO_VOICE_MID_SEND 1
#define GTKHX_PROTO_VOICE_MID_USER 2
#define GTKHX_PROTO_VOICE_MID_CAM_SEND 3
#define GTKHX_PROTO_VOICE_MID_SCR_SEND 4
#define GTKHX_PROTO_VOICE_MID_CAM_USER 5
#define GTKHX_PROTO_VOICE_MID_SCR_USER 6

/* Parse an SDP a=mid: label. Returns one of the GTKHX_PROTO_VOICE_MID_*
 * constants. For the USER, CAM_USER and SCR_USER variants, *out_uid is
 * set to the parsed uid; for the rest, *out_uid is left untouched. */
extern uint32_t gtkhx_proto_parse_voice_mid_label (const uint8_t *label_ptr,
                                                   size_t label_len,
                                                   uint16_t *out_uid);

/* C-ABI summary of an SDP offer/answer. Scalars only; the mid / bundle
 * lists stay Rust-side until the runtime crate consumes them. */
struct gtkhx_proto_voice_sdp_summary {
    uint32_t mid_count;
    uint32_t unknown_mid_count;
    uint32_t bundle_count;
    bool has_disabled_slot;
    bool has_pcmu;
};

/* Returns true on success; false (leaves *out untouched) only on NULL
 * out or NULL sdp_ptr with nonzero sdp_len. */
extern bool
gtkhx_proto_parse_voice_sdp_summary (const uint8_t *sdp_ptr, size_t sdp_len,
                                     struct gtkhx_proto_voice_sdp_summary *out);

/* C-ABI view of a parsed ICE candidate; strings borrow into the
 * opaque handle and stay valid until gtkhx_proto_voice_ice_free is
 * called on the handle. */
struct gtkhx_proto_voice_ice_candidate {
    const uint8_t *candidate_ptr;
    size_t candidate_len;
    const uint8_t *sdp_mid_ptr;
    size_t sdp_mid_len;
    const uint8_t *username_fragment_ptr;
    size_t username_fragment_len;
    uint32_t sdp_mline_index;
    bool sdp_mline_index_present;
    bool is_end_of_candidates;
};

/* Opaque handle, owned by Rust. */
struct gtkhx_proto_voice_ice_handle;

/* Parse the inner JSON from a DATA_VOICE_ICE chunk. Returns an opaque
 * handle on success, NULL on parse failure or NULL inputs. On success
 * *out (if non-NULL) is populated with borrowed pointers into the
 * handle's owned strings. Caller frees via gtkhx_proto_voice_ice_free. */
extern struct gtkhx_proto_voice_ice_handle *
gtkhx_proto_parse_voice_ice_json (const uint8_t *json_ptr, size_t json_len,
                                  struct gtkhx_proto_voice_ice_candidate *out);

extern void gtkhx_proto_voice_ice_free (struct gtkhx_proto_voice_ice_handle *h);

/* Build the outgoing JSON for an ICE candidate into out_buf.
 *
 * Per fogWraith Capabilities-Voice.md §"ICE Candidate Format",
 * `candidate` and `sdpMid` are required on every payload:
 *   - candidate_ptr MUST be non-NULL. candidate_len may be 0 (the
 *     spec's end-of-candidates shorthand emits the key with an
 *     empty-string value).
 *   - sdp_mid_ptr MUST be non-NULL. sdp_mid_len may be 0.
 *
 * The optional fields preserve the NULL-means-key-absent shape:
 *   - sdp_mline_index is emitted only when sdp_mline_index_present.
 *   - username_fragment_ptr == NULL with username_fragment_len == 0
 *     omits the key. Non-NULL with any length emits the value.
 *
 * Returns the number of bytes written, or 0 on failure (NULL
 * required pointer, undersized buffer, NULL out_buf). No trailing
 * NUL is appended. The returned length may be up to and including
 * out_cap on success — callers that want to NUL-terminate the
 * payload must allocate out_cap + 1 bytes and pass out_cap as the
 * cap, then write out_buf[returned] = '\0' from the spare slot. */
extern size_t gtkhx_proto_build_voice_ice_json (
    const uint8_t *candidate_ptr, size_t candidate_len,
    const uint8_t *sdp_mid_ptr, size_t sdp_mid_len, uint32_t sdp_mline_index,
    bool sdp_mline_index_present, const uint8_t *username_fragment_ptr,
    size_t username_fragment_len, uint8_t *out_buf, size_t out_cap);

/* Scalar fields parsed from a voice reply / notification body. The
 * variable-length payloads (SDP / ICE / codec / participants) are
 * fetched separately via gtkhx_proto_voice_reply_field; the scalars
 * tell the caller which fields are present and how long each payload
 * is so it can size its buffer. */
struct gtkhx_proto_voice_reply {
    uint32_t cid;
    uint16_t muted;
    bool muted_present;
    bool sdp_present;
    bool ice_present;
    bool codec_present;
    bool participants_present;
    uint32_t sdp_len;
    uint32_t ice_len;
    uint32_t codec_len;
    uint32_t participants_len;
};

extern bool gtkhx_proto_parse_voice_reply (const uint8_t *buf, size_t len,
                                           struct gtkhx_proto_voice_reply *out);

/* Video reply / Video Status (611) body. The slices borrow `buf`; an
 * absent field has a NULL pointer, an absent cid or kind is 0. */
struct gtkhx_proto_video_reply {
    uint32_t cid;
    uint16_t kind;
    const uint8_t *codec_ptr;
    size_t codec_len;
    const uint8_t *publishers_ptr;
    size_t publishers_len;
};
extern bool gtkhx_proto_parse_video_reply (const uint8_t *buf, size_t len,
                                           struct gtkhx_proto_video_reply *out);

/* Per-field accessor for the variable-length payloads. `field`:
 *   0 = SDP, 1 = ICE, 2 = codec name, 3 = participants blob.
 * On success writes *out_ptr / *out_len pointing into `buf`; the
 * pointer stays valid for the lifetime of `buf`. Returns false if
 * the field is absent or `field` is invalid. */
extern bool gtkhx_proto_voice_reply_field (const uint8_t *buf, size_t len,
                                           uint32_t field,
                                           const uint8_t **out_ptr,
                                           size_t *out_len);

/* ---- GIF-icons extension (fogWraith GIF-Icons.md) ---- */

/* True if buf begins with a GIF87a/GIF89a signature. */
extern bool gtkhx_proto_gif_icon_is_gif (const uint8_t *buf, size_t len);

/* Parse an ICON_CHANGE (1864) broadcast: UID only. Returns true and
 * writes *out_uid when present, false otherwise. */
extern bool gtkhx_proto_parse_icon_change (const uint8_t *buf, size_t len,
                                           uint16_t *out_uid);

/*
 * Receive-dispatch routing. hx_dispatch_frame calls hx_recv_route(type) to map
 * a server frame's opcode to a handler category, then switches on the result to
 * pick the body handler. This enum mirrors hxproto's dispatch::HandlerKind
 * (hxproto::dispatch) — the values must stay in lockstep
 * with the Rust discriminants. The composite-TASK mask (folding a now-fixed
 * Heidrun quirk where the TASK reply echoed the request opcode in the low u16,
 * kept defensively for older deployments) is applied inside hx_recv_route, so
 * the C side no longer special-cases it.
 */
typedef enum {
    HX_RECV_CHAT = 0,
    HX_RECV_MSG = 1,
    HX_RECV_USER_CHANGE = 2,
    HX_RECV_USER_PART = 3,
    HX_RECV_NEWS_POST = 4,
    HX_RECV_TASK = 5,
    HX_RECV_CHAT_SUBJECT = 6,
    HX_RECV_CHAT_INVITE = 7,
    HX_RECV_USER_SELFINFO = 8,
    HX_RECV_AGREEMENT = 9,
    HX_RECV_BANNER = 10,
    HX_RECV_POLITEQUIT = 11,
    HX_RECV_XFER_QUEUE = 12,
    HX_RECV_VOICE_SDP_OFFER = 13,
    HX_RECV_VOICE_ICE = 14,
    HX_RECV_VOICE_ROOM_STATUS = 15,
    HX_RECV_ICON_CHANGE = 16,
    HX_RECV_UNKNOWN = 17,
    HX_RECV_VIDEO_STATUS = 18,
} hx_recv_handler_kind;

extern hx_recv_handler_kind hx_recv_route (guint32 opcode);

/* Decoded 8-byte Hotline wire timestamp (mirror of #[repr(C)] GtkhxProtoHlDate
 * in hxproto's ffi.rs). `kind` is 0 = Mac 1904 epoch, 1 = modern. The
 * caller resolves this to an absolute instant + display string (local-tz
 * calendar math, e.g. GDateTime) — the decode is protocol, the format is view. */
struct gtkhx_proto_hl_date {
    uint8_t kind;
    uint16_t year;
    uint32_t secs;
};

/* Seconds between the Mac 1904 epoch and the Unix epoch — for a `kind==0`
 * result, the absolute Unix time is `secs - GTKHX_PROTO_MAC_TO_UNIX_EPOCH_OFFSET`.
 * (Mirrors hl_date::MAC_TO_UNIX_EPOCH_OFFSET.) */
#define GTKHX_PROTO_MAC_TO_UNIX_EPOCH_OFFSET 2082844800u

/* Decode an 8-byte wire timestamp into *out. FALSE (out untouched) for the
 * no-timestamp sentinel (secs==0), an out-of-range modern year, or a short
 * buffer. */
extern bool gtkhx_proto_hl_date_decode (const uint8_t *bytes, size_t len,
                                        struct gtkhx_proto_hl_date *out);

#endif /* GTKHX_HOTLINE_PROTO_H */
