/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * src/network_decode.h — receive-side qbuf-to-qbuf decoder split
 * out of network.c so the Tier 2 test suite can drive it without
 * dragging in the rest of network.c (async connect, GSocketClient,
 * pthread plumbing, tracker fetch).
 *
 * Two entry points:
 *
 *   hx_aead_pump_frames — pumps complete length-prefixed ChaCha20-
 *                         Poly1305 frames out of htlc->read_in into
 *                         htlc->aead_plain. Only used in
 *                         CIPHER_MODE_AEAD.
 *   hx_decode           — top-level decoder dispatch: AEAD frame
 *                         pump, legacy stream-cipher decode, or
 *                         plaintext passthrough. Drains read_in /
 *                         aead_plain into htlc->in based on how
 *                         many bytes the rcv loop is waiting for
 *                         (htlc->in.len).
 *
 * Error handling is unchanged from the in-line versions: on a
 * frame-size-out-of-range or Poly1305 tag-mismatch, the AEAD pump
 * calls hx_htlc_close (forward-declared via network.h) to tear
 * the connection down, which zeros htlc->fd; both functions then
 * bail. The test suite stubs hx_htlc_close + hx_printf_prefix so
 * the same call-paths work without dragging in GTK or GtkhxSession.
 */

#ifndef GTKHX_NETWORK_DECODE_H
#define GTKHX_NETWORK_DECODE_H

#include <glib.h>
#include "protocol.h"

/* Pump complete AEAD frames from htlc->read_in into htlc->aead_plain.
 * Returns the new total length of aead_plain. On a malformed frame
 * (size out of range, Poly1305 tag mismatch) calls hx_htlc_close and
 * returns 0; callers should check htlc->fd before consuming bytes. */
extern u_int32_t hx_aead_pump_frames (struct htlc_conn *htlc);

/* Top-level receive decoder. Drains as many bytes as the rcv loop
 * is currently waiting for (htlc->in.len) into htlc->in.buf, going
 * through cipher_decode + compress_decode (stream-cipher path) or
 * the AEAD frame pump (CHACHA20 path) as appropriate.
 *
 * Returns nonzero when htlc->in is full (rcv loop should dispatch),
 * zero when more bytes are needed (rcv loop should wait for the
 * next read).
 *
 * On AEAD failure htlc->fd is zeroed via hx_htlc_close and the
 * function returns 0; the outer loop in htlc_read sees no progress
 * and the fd-cleared state stops the next iteration. */
extern unsigned int hx_decode (struct htlc_conn *htlc);

#endif /* GTKHX_NETWORK_DECODE_H */
