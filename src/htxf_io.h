/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * htxf_io.{c,h} — read/write wrappers for the HTXF file-transfer
 * subchannel that route through ChaCha20-Poly1305 AEAD when the
 * control channel negotiated it (HOPE-ChaCha20-Poly1305 Phase E).
 *
 * Wire shape, plaintext mode:
 *     g_input_stream_read(in, ...)   ≡  htxf_io_read(htxf, io, buf, len)
 *     g_output_stream_write_all(...) ≡  htxf_io_write(htxf, io, buf, len)
 *
 * `io` is the GIOStream of the GSocketConnection htxf_connect
 * returned. The wrappers extract its input / output streams
 * internally. Using GIOStream (rather than a raw socket fd) lets
 * the same code path absorb a future TLS wrap — replacing the
 * GSocketConnection's stream with a GTlsClientConnection wrapping
 * it (docs/tls-scoping.md Phase 2) needs no edits below this line.
 *
 * Wire shape, AEAD mode (Phase E2 — htxf_connect flips
 * htxf->aead_active = TRUE after deriving xfer_encode / xfer_decode
 * via cipher_aead_derive_transfer_keys):
 *
 *   write — Seal each call's plaintext as one length-prefixed
 *           framed ChaCha20-Poly1305 frame, then write the framed
 *           bytes to the socket. The 4-byte big-endian length
 *           prefix lets the peer know how much ciphertext to
 *           accumulate before calling Open.
 *
 *   read  — Maintain a plaintext accumulator. When the caller
 *           asks for N bytes, serve from the accumulator first;
 *           if it's empty, read enough ciphertext from the socket
 *           to assemble at least one frame, Open it, append the
 *           plaintext to the accumulator, then serve. Repeats
 *           until the caller's request is satisfied or the socket
 *           EOFs / errors.
 *
 * The wrappers preserve the existing read()/write() error
 * semantics — 0 / negative return values still indicate
 * end-of-stream / error — so xfers.c's `if (read(s,...) < 1)`
 * idioms keep working without per-call-site logic changes.
 *
 * Per-direction state (encode for outbound, decode for inbound)
 * plus the read accumulator live on struct htxf_conn so the
 * lifetime is one ChaCha20 context pair per transfer. Counters
 * start at 0 for every transfer (cipher_aead_derive_transfer_keys
 * zeros them) — different transfers within the same control
 * channel session never share a nonce.
 *
 * NOT covered here:
 *
 *   - The control channel itself (htlc_conn). That lives in
 *     cipher.c / network.c and was wired up by Phase D.
 *   - banner.c's HTXF worker (which has its own read_n / write_n
 *     and uses struct htxf_fetch, not struct htxf_conn). Phase
 *     E3 follow-up.
 */

#ifndef __htxf_io_h
#define __htxf_io_h

#include "config.h"
#include <gio/gio.h>
#include <glib.h>
#include <sys/types.h> /* ssize_t */

struct htxf_conn;

/* Per-htxf_conn AEAD I/O state. One instance per transfer; lives
 * inline on struct htxf_conn so the worker thread can drive it
 * without an extra heap allocation. Zero-initialised at
 * xfer_new time and reclaimed by htxf_io_release at xfer_delete
 * time. */
struct htxf_aead_io {
    /* Receive plaintext accumulator. Bytes successfully Open'd
	 * but not yet consumed by the caller. Grown as needed (one
	 * AEAD frame at a time, capped at CIPHER_AEAD_MAX_FRAME_SIZE
	 * payload size). NULL until first use. */
    guint8 *plain_buf;
    gsize plain_cap;
    gsize plain_len;
    gsize plain_pos;

    /* Receive ciphertext accumulator. Bytes read from the socket
	 * but not yet a complete frame. Grown as needed. NULL until
	 * first use. */
    guint8 *cipher_buf;
    gsize cipher_cap;
    gsize cipher_len;
};

/* Zero-init the I/O state. Idempotent on an already-zero struct
 * (xfer_new memsets the parent htxf_conn). Call sites that
 * memset() the parent struct don't strictly need this, but it
 * keeps the intent explicit. */
extern void htxf_io_init (struct htxf_conn *htxf);

/* Free the read accumulator buffers and zero the state. Called
 * from the xfer worker exit path; safe to call multiple times.
 * Does NOT touch the AEAD key material — that lives in
 * htxf->xfer_encode / xfer_decode, which are zeroed by their
 * own owner (struct htxf_conn) when the htxf_conn frees. */
extern void htxf_io_release (struct htxf_conn *htxf);

/* Read up to `len` bytes from `io` into `buf`.
 *
 * Plaintext path: g_input_stream_read on io's input stream.
 *
 * AEAD path: serves up to `len` bytes from the plaintext
 * accumulator, refilling from the input stream as needed.
 *
 * Returns bytes copied to buf (>0), 0 on EOF, -1 on error
 * (errno set to EIO; GError details surface through debug_log
 * under the xfer / xfer-aead categories). Close enough to read(2)
 * semantics that the historical `if (r < 1)` idioms in xfers.c
 * keep working without per-site logic changes. */
extern ssize_t htxf_io_read (struct htxf_conn *htxf, GIOStream *io,
                             void *buf, size_t len);

/* Write exactly `len` bytes from `buf` to `io`.
 *
 * Plaintext path: g_output_stream_write_all on io's output
 * stream (loops over partial writes internally — short writes
 * can't surface).
 *
 * AEAD path: Seal's the buffer as one frame, writes the framed
 * bytes through the output stream.
 *
 * Returns `len` on success (matches the caller's
 * `if (htxf_io_write (...) != n)` check) or -1 with errno = EIO
 * on error / oversized plaintext. */
extern ssize_t htxf_io_write (struct htxf_conn *htxf, GIOStream *io,
                              const void *buf, size_t len);

#endif /* __htxf_io_h */
