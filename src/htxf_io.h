/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * htxf_io.{c,h} — thin C shim over hxnet's Rust HTXF (file-transfer)
 * subchannel transport (rust/crates/hxnet/src/htxf.rs).
 *
 * Since the HTXF→Rust H2 re-wire the byte pump, the AEAD framing
 * (ChaCha20-Poly1305 length-prefixed frames), the optional rustls TLS
 * wrap, and the socket fd all live in the hxnet crate. struct htxf_conn
 * carries the opaque channel handle in `htxf->hx`; this shim:
 *
 *   - htxf_connect (network.c) opens the channel via hxnet_htxf_open,
 *     handing over a connected blocking fd, the packed preamble, and
 *     (when the control channel negotiated CIPHER_MODE_AEAD) the
 *     per-transfer ChaCha20 keys derived into htxf->xfer_encode /
 *     xfer_decode.
 *   - the xfers.c / banner.c workers stream bytes through
 *     htxf_io_read / htxf_io_write, which forward to hxnet_htxf_read /
 *     hxnet_htxf_write and translate the Rust `-1` error into the
 *     errno-set `< 1` idiom the worker loops already use.
 *   - htxf_io_release closes the channel at worker teardown.
 *
 * The shim exists (rather than calling hxnet_htxf_* directly from the
 * workers) to keep errno semantics and the handle cast in one place.
 */

#ifndef __htxf_io_h
#define __htxf_io_h

#include "config.h"
#include <glib.h>
#include <sys/types.h> /* ssize_t */

#include "cipher_aead.h" /* chacha_aead_state */

struct htxf_conn;

/* Opaque hxnet HTXF channel handle (Rust `HtxfConn`). The C side never
 * dereferences it — it's stored on htxf->hx and passed back to the
 * hxnet_htxf_* calls below. */
typedef struct HtxfConn HtxfConn;

/* ---- hxnet HTXF subchannel FFI ------------------------------------
 * Defined in rust/crates/hxnet/src/htxf.rs. Declared here so both this
 * shim (read/write/timeout/close) and network.c::htxf_connect (open)
 * see one prototype. */

/* TOFU verify-cert callback for a TLS subchannel whose peer cert did
 * not chain to a public root: receives the "sha256:<hex>" leaf
 * fingerprint, returns non-zero to accept the connection. */
typedef int (*hxnet_htxf_verify_cb_t) (const guint8 *fp, gsize fp_len,
                                       void *user_data);

/* Open an HTXF subchannel over an already-connected, blocking `fd`
 * (which this call adopts — the C side must not close it). `tls != 0`
 * TLS-handshakes the fd with rustls (host = SNI / TOFU name);
 * `preamble` is written raw before AEAD arms; non-NULL
 * `aead_encode` / `aead_decode` arm per-transfer AEAD framing. Returns
 * an owned handle, or NULL on bad arguments / TLS rejection / IO error
 * (the adopted fd is closed on every failure path). */
extern HtxfConn *hxnet_htxf_open (int fd, int tls, const guint8 *host,
                                  size_t host_len, const guint8 *preamble,
                                  size_t preamble_len,
                                  const chacha_aead_state *aead_encode,
                                  const chacha_aead_state *aead_decode,
                                  hxnet_htxf_verify_cb_t verify_cert,
                                  void *user_data);

/* Blocking read of up to `len` bytes (`0` = clean EOF, `-1` on error). */
extern ssize_t hxnet_htxf_read (HtxfConn *handle, guint8 *buf, size_t len);

/* Blocking write of `len` bytes — one AEAD frame when armed (returns
 * `len` on success, `-1` on error). */
extern ssize_t hxnet_htxf_write (HtxfConn *handle, const guint8 *buf,
                                 size_t len);

/* Arm (ms > 0) or clear (ms == 0) a per-read timeout on the underlying
 * socket. Returns 0 / -1. */
extern int hxnet_htxf_set_read_timeout (HtxfConn *handle, guint32 timeout_ms);

/* Close the channel and free the handle (drops the socket / TLS
 * session). Safe with NULL. */
extern void hxnet_htxf_close (HtxfConn *handle);

/* ---- C-side shim --------------------------------------------------- */

/* Zero the handle slot. struct htxf_conn is memset by every caller
 * before use, so this is mostly explicit-intent; safe to call before
 * htxf_connect opens the channel. */
extern void htxf_io_init (struct htxf_conn *htxf);

/* Close the hxnet channel and clear htxf->hx. Idempotent — safe to
 * call on a never-opened htxf and to call more than once. */
extern void htxf_io_release (struct htxf_conn *htxf);

/* Read up to `len` bytes into `buf`. Returns bytes read (>0), 0 on
 * clean EOF, -1 on error. On error errno is EINVAL when the channel
 * isn't open (htxf / htxf->hx NULL — a caller bug) and EIO for a
 * transport / framing error from hxnet. Close enough to read(2)
 * semantics that the historical `if (r < 1)` idioms keep working
 * without per-site logic changes. */
extern ssize_t htxf_io_read (struct htxf_conn *htxf, void *buf, size_t len);

/* Write exactly `len` bytes from `buf` (one AEAD frame when armed).
 * Returns `len` on success (matches `if (htxf_io_write (...) != n)`
 * checks), -1 on error. On error errno is EINVAL when the channel
 * isn't open (htxf / htxf->hx NULL) and EIO for a transport / seal
 * error from hxnet. */
extern ssize_t htxf_io_write (struct htxf_conn *htxf, const void *buf,
                              size_t len);

/* Arm/clear a per-read timeout (ms; 0 = block indefinitely). Used by
 * the folder-drain path to slurp whatever the server has in flight and
 * then give up. Returns 0 on success, -1 on error (errno EINVAL when
 * the channel isn't open, EIO on a hxnet setsockopt failure). */
extern int htxf_io_set_read_timeout (struct htxf_conn *htxf,
                                     guint32 timeout_ms);

#endif /* __htxf_io_h */
