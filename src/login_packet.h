/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * login_packet.{c,h} — shared HTLC_HDR_LOGIN packet builder used by
 * both the production code path (src/network.c::send_login) and the
 * integration test harness (tests/integration/integration_harness.c).
 *
 * BEFORE this module existed, the production binary had ~100 LOC of
 * inline chunk-assembly inside send_login, and the harness had
 * ~70 LOC of similar-but-not-identical chunk-assembly inside
 * integration_login_guest{,_caps}. The two would drift — for example,
 * the harness sends HTLC_DATA_NAME inline (a shortcut for tests),
 * which production deliberately delays until USER_CHANGE /
 * AGREEMENTAGREE — and any future LOGIN-shape change (chat-history
 * enablement, large-file caps, HOPE algorithm advertisement) had to
 * be applied twice.
 *
 * This module owns the LOGIN shape. Callers fill in an
 * `hx_login_request` describing what they want sent;
 * hx_login_build_chunks turns it into a chunk array + scratch
 * buffer. The caller then hands the array to hlpack_chunks (or the
 * trace-aware hlwrite_chunks in network.c) for the actual wire
 * encoding.
 *
 * Two modes are supported:
 *
 *   HX_LOGIN_MODE_LEGACY — the cleartext LOGIN sent on both HOPE
 *     Step 2 (after the algorithm-negotiation reply lands) and the
 *     non-HOPE bookmark. Chunks emitted:
 *
 *         HTLC_DATA_ICON          (always)
 *         HTLC_DATA_LOGIN         (XOR-encoded login_name)
 *         HTLC_DATA_PASSWORD      (XOR-encoded password — if set)
 *         HTLC_DATA_NAME          (display_name — if set; test only)
 *         HTLC_DATA_CLIENTVERSION (if client_version != 0)
 *         HTLC_DATA_CAPABILITIES  (if send_caps)
 *
 *   HX_LOGIN_MODE_HOPE_STEP1 — the algorithm-negotiation packet for
 *     HOPE-Secure-Login. Chunks emitted:
 *
 *         HTLC_DATA_LOGIN          (single 0 byte)
 *         HTLC_DATA_PASSWORD       (single 0 byte)
 *         HTLC_DATA_MAC_ALG        (preference list — see hope_macalgs)
 *         HTLC_DATA_HOPE_APP_ID    (4-char OSType)
 *         HTLC_DATA_HOPE_APP_STRING
 *         HTLC_DATA_CIPHER_ALG     (if cipheralg != NULL/"")
 *         HTLC_DATA_COMPRESS_ALG   (if compressalg != NULL/"")
 *         HTLC_DATA_SESSIONKEY     (empty — server fills in the reply)
 *
 *     Caller-supplied algorithm lists are 1-entry; multi-entry MAC
 *     preference list is hardcoded inside the builder per the spec
 *     (HMAC-SHA256, HMAC-SHA1, HMAC-MD5).
 */

#ifndef HX_LOGIN_PACKET_H
#define HX_LOGIN_PACKET_H 1

#include "config.h"
#include <glib.h>
#include <stdint.h>
#include <stddef.h>

struct htlc_conn;
struct hx_chunk;

typedef enum {
    HX_LOGIN_MODE_LEGACY = 0,
    HX_LOGIN_MODE_HOPE_STEP1 = 1
} hx_login_mode;

typedef struct {
    hx_login_mode mode;

    /* --- Legacy mode fields ------------------------------------- */

    /* HTLC_DATA_ICON value. Always sent in legacy mode. */
    guint16 icon;

    /* HTLC_DATA_LOGIN. NULL or "" means: emit the chunk with zero
     * length (the "anonymous" form some servers expect for guest).
     * Non-NULL is XOR-encoded inside hx_login_build_chunks. */
    const char *login_name;

    /* HTLC_DATA_PASSWORD. NULL or "" means: no PASSWORD chunk.
     * Non-NULL is XOR-encoded inside hx_login_build_chunks. */
    const char *password;

    /* HTLC_DATA_NAME. NULL or "" means: no NAME chunk (production
     * default — production sends nick later via AGREEMENTAGREE /
     * USER_CHANGE; the test harness uses this for inline shortcut). */
    const char *display_name;

    /* HTLC_DATA_CLIENTVERSION. 0 means: don't emit the chunk. */
    guint16 client_version;

    /* HTLC_DATA_CAPABILITIES. send_caps gates emission separately
     * from the value because caps=0 is a meaningful advertisement
     * ("I support the spec but advertise no optional bits"). */
    guint16 caps;
    int send_caps;

    /* --- HOPE Step 1 mode fields -------------------------------- */

    /* HTLC_DATA_HOPE_APP_ID. NULL = "GTKx" (the production default
     * — distinguishes GtkHx from other hx-family clients in server
     * logs). 4 characters expected; not NUL-terminated on the wire. */
    const char *hope_app_id;

    /* HTLC_DATA_HOPE_APP_STRING. NULL = empty. Free-form name +
     * version string. */
    const char *hope_app_string;

    /* HTLC_DATA_CIPHER_ALG. NULL or "" = don't advertise. Single-
     * entry list emitted (the production default — multi-entry
     * lists are a Phase 5+ extension). */
    const char *cipheralg;

    /* HTLC_DATA_COMPRESS_ALG. NULL or "" = don't advertise. Single
     * entry, same shape as cipheralg. */
    const char *compressalg;
} hx_login_request;

/* Scratch buffer the caller must provide for hx_login_build_chunks
 * to lay out the network-order ints and algorithm lists. 256 bytes
 * covers any reasonable LOGIN request (the MAC preference list,
 * three entries deep, is the biggest single field at ~36 bytes). */
#define HX_LOGIN_SCRATCH_SIZE 256

/* Maximum number of chunks any LOGIN request can produce. */
#define HX_LOGIN_MAX_CHUNKS 12

/*
 * Translate a logical LOGIN request into an hx_chunk array suitable
 * for hlpack_chunks. The caller supplies the chunks array and a
 * scratch buffer (the data pointers inside chunks[] point into the
 * scratch; both must outlive the eventual hlpack_chunks call).
 *
 * Returns the number of chunks filled (always <= HX_LOGIN_MAX_CHUNKS).
 *
 * Truncates over-long login / password to 64 bytes — matches the
 * legacy inline behaviour in network.c::send_login.
 */
extern int hx_login_build_chunks (const hx_login_request *req,
                                  struct hx_chunk *chunks, int chunks_cap,
                                  guint8 *scratch, size_t scratch_cap);

#endif /* HX_LOGIN_PACKET_H */
