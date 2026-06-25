/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * htxf_subchannel.{c,h} — shared HTXF (file-transfer subchannel)
 * handshake primitives, used by every code path that opens a fresh
 * HTXF TCP connection on top of a control-channel session.
 *
 * Three callers live in production / harness today:
 *
 *   src/network.c::htxf_connect             — generic transfers
 *                                             (file get/put, folder
 *                                             get/put, news file)
 *   src/banner.c::banner_htxf_worker_thread — banner image fetch
 *   tests/integration/test_hope_chacha20_banner.c
 *                                           — Tier 3 coverage
 *
 * Before this module, all three open-coded the same sequence:
 *
 *   1. pack 16-byte HTXF preamble (ref, total_size, type, flags)
 *   2. (network.c only, when CAP_LARGE_FILES + >4GiB) append 8 bytes
 *      of big-endian total_size for the 24-byte variant
 *   3. write that buffer plaintext to the freshly-connected socket
 *      — the server matches this subchannel against the queued
 *      transfer by `ref`, so AEAD-framing the preamble makes the
 *      server read garbage and slam the connection shut
 *   4. if the parent control channel negotiated CIPHER_MODE_AEAD,
 *      derive the per-transfer ChaCha20-Poly1305 key pair off the
 *      control session_key + ref, init the htxf_io receive buffer,
 *      flip aead_active = TRUE so subsequent htxf_io_read /
 *      htxf_io_write framings kick in
 *
 * The bug that motivated this extraction (claude/hope-chacha20-
 * banner-htxf-fix, May 2026) was that banner.c had step 3 inside
 * the AEAD-framing path — the test was constructed in lockstep
 * with the broken production code, so it asserted the bug as
 * correct behaviour. Once both walked through one shared module,
 * fixing the framing in one place fixed it everywhere and added a
 * structural prevent-recurrence guarantee.
 *
 * The writing of bytes is intentionally NOT shared. The three
 * callers use three different write primitives:
 *
 *   network.c   — raw write(2) (POSIX, blocking)
 *   banner.c    — local write_n() short-write loop
 *   tests/...   — integration_send()
 *
 * Sharing a write-primitive would force a callback-style interface
 * that pulls protocol-level identity (errno semantics, GError
 * wrappers, fd lifetimes) into the wrong layer. Instead this
 * module hands the caller a packed buffer; the caller writes it
 * however it wants. Same shape as login_packet / agreement_packet
 * / chat_history — packing + framing decisions in a shared
 * builder, wire I/O at the caller.
 */

#ifndef HX_HTXF_SUBCHANNEL_H
#define HX_HTXF_SUBCHANNEL_H 1

#include "config.h"
#include <glib.h>
#include <stdint.h>
#include <stddef.h>

struct htxf_conn;

/* Maximum bytes the preamble builder writes. 16 for the legacy
 * variant + 8 for the optional SIZE64 trailer. */
#define HX_HTXF_PREAMBLE_MAX_BYTES 24

/*
 * Pack the HTXF subchannel preamble into a caller-provided buffer.
 *
 *   buf, cap       — caller's scratch. Required size depends on
 *                    `size64`: 16 bytes (SIZEOF_HTXF_HDR) for the
 *                    legacy variant, 24 bytes (SIZEOF_HTXF_HDR + 8)
 *                    when `size64` is TRUE. Sizing the buffer to
 *                    HX_HTXF_PREAMBLE_MAX_BYTES always works and
 *                    keeps callers from having to branch on the
 *                    variant. cap is checked against the actual
 *                    required size, so a too-small buffer fails
 *                    closed (returns 0) instead of stomping.
 *   ref            — 32-bit HTXF reference, returned by the server
 *                    in the prior control-channel TASK reply
 *                    (DATA_HTXF_REF).
 *   total_size     — full body size in bytes. With size64=FALSE
 *                    this MUST fit in a guint32; the function
 *                    fails closed if it doesn't.
 *   type           — HTXF_TYPE_{FILE, FOLDER, BANNER}.
 *   flags          — extra HTXF_FLAG_* bits the caller wants set
 *                    OUTSIDE the LARGE_FILE / SIZE64 pair (rare —
 *                    today always 0). The builder ORs in
 *                    HTXF_FLAG_LARGE_FILE | HTXF_FLAG_SIZE64
 *                    itself when size64=TRUE.
 *   size64         — TRUE to emit the 24-byte handshake variant
 *                    (16-byte header with the legacy 32-bit length
 *                    zeroed + flags bits set, followed by 8-byte
 *                    big-endian total_size). Caller decides based
 *                    on CAP_LARGE_FILES negotiation + whether
 *                    total_size actually exceeds 0xFFFFFFFF.
 *
 * Returns the number of bytes written (16 or 24), or 0 on
 * validation failure (NULL buf, cap too small, size64=FALSE with
 * total_size > 0xFFFFFFFF).
 */
extern size_t hx_htxf_subchannel_pack_preamble (
    guint8 *buf, size_t cap,
    guint32 ref, guint64 total_size,
    guint16 type, guint16 flags,
    gboolean size64);

#endif /* HX_HTXF_SUBCHANNEL_H */
