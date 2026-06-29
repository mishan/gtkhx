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
 *   - htxf_connect (network.c) opens the channel via hxnet_htxf_connect,
 *     which does the connect itself (optional SOCKS) — no fd hand-off —
 *     then takes the packed preamble and
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

#ifndef GTKHX_HTXF_IO_H
#define GTKHX_HTXF_IO_H

#include "config.h"
#include <glib.h>
#include <sys/types.h> /* ssize_t */

#include "cipher.h"

struct htxf_conn;

/* Opaque hxnet HTXF channel handle (Rust `HtxfConn`). The C side never
 * dereferences it — it's stored on htxf->hx and passed back to the
 * hxnet_htxf_* calls below. */
typedef struct HtxfConn HtxfConn;

/* Opaque handle to a HOPE control-channel's retained AEAD material
 * (Rust `HxnetHopeAead`). Obtained from
 * hxnet_connection_hope_aead_material (the control connection's retained
 * material, via hx_bridge_orchestrated_hope_aead) and passed to
 * hxnet_htxf_connect so the subchannel derives its per-transfer keys
 * in-process. The session key never crosses the FFI as bytes — only this
 * opaque token does. Free with hxnet_hope_aead_free. */
typedef struct HxnetHopeAead HxnetHopeAead;

/* Clone a HxnetHopeAead into a new, independently owned handle (NULL in
 * → NULL out). The copy's lifetime is decoupled from the source, so a
 * worker that may outlive the original handle (banner.c's HTXF fetch,
 * which can race a disconnect that frees htlc->hope_aead) can own its
 * own copy. Free with hxnet_hope_aead_free. Declared in
 * rust/crates/hxnet/src/ffi.rs. */
extern HxnetHopeAead *hxnet_hope_aead_clone (const HxnetHopeAead *h);

/* Free a HxnetHopeAead handle (NULL-safe). Declared in
 * rust/crates/hxnet/src/ffi.rs. */
extern void hxnet_hope_aead_free (HxnetHopeAead *h);

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
 * `preamble` is written raw before AEAD arms. A non-NULL `hope_aead`
 * arms per-transfer AEAD framing: the keys are derived in-process from
 * that control-channel material + `xfer_ref`, so the session key never
 * crosses the FFI. NULL = plaintext passthrough. Returns an owned
 * handle, or NULL on bad arguments / TLS rejection / IO error (the
 * adopted fd is closed on every failure path). */
extern HtxfConn *hxnet_htxf_open (int fd, int tls, const guint8 *host,
                                  size_t host_len, const guint8 *preamble,
                                  size_t preamble_len,
                                  const HxnetHopeAead *hope_aead,
                                  guint32 xfer_ref,
                                  hxnet_htxf_verify_cb_t verify_cert,
                                  void *user_data);

/* Connect an HTXF subchannel to host:port (optionally through a SOCKS
 * proxy) entirely in Rust, then open it — the production entry that
 * replaces the C-side GSocketClient connect + fd hand-off. `proxy_uri`
 * (length proxy_uri_len) is an optional "socks5://..." URI; NULL/0
 * connects direct, a malformed/unsupported URI fails the open. `host` is
 * required (connect target + TLS SNI / TOFU name). All other args match
 * hxnet_htxf_open. The connect runs on the tokio runtime (bounded by the
 * shared handshake timeout) and blocks the calling worker for the result.
 * Returns an owned handle, or NULL on a bad argument / connect / TLS /
 * TOFU failure. Defined in rust/crates/hxnet/src/htxf.rs. */
extern HtxfConn *hxnet_htxf_connect (const guint8 *host, size_t host_len,
                                     guint16 port, const guint8 *proxy_uri,
                                     size_t proxy_uri_len, int tls,
                                     const guint8 *preamble,
                                     size_t preamble_len,
                                     const HxnetHopeAead *hope_aead,
                                     guint32 xfer_ref,
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

/* ---- hxnet HTXF cancellation token --------------------------------
 * Cooperative-cancel foundation (Phase R3 X1). A token is created on
 * the main thread before the transfer worker starts, armed with the
 * channel's socket once it opens (worker), and aborted from the main
 * thread to shut that socket down and unblock a parked blocking
 * read/write. Reference-counted: hxnet_htxf_abort_new yields the C
 * side's ref (freed with hxnet_htxf_abort_free); hxnet_htxf_abort_arm
 * clones a ref into the channel handle (released when the channel
 * closes). Defined in rust/crates/hxnet/src/htxf.rs. */

/* Opaque Rust HtxfAbort cancellation token. */
typedef struct HtxfAbort HtxfAbort;

/* Create an unarmed token (never NULL). Free with
 * hxnet_htxf_abort_free exactly once. */
extern const HtxfAbort *hxnet_htxf_abort_new (void);

/* Arm `token` with `handle`'s socket and clone a ref into `handle` so
 * its read/write observe the aborted flag. Worker-thread call, once,
 * right after hxnet_htxf_connect. No-op on NULL args. */
extern void hxnet_htxf_abort_arm (HtxfConn *handle, const HtxfAbort *token);

/* Flip `token` to aborted and shut its socket down to unblock a parked
 * read/write. Main-thread call. NULL-safe. Does NOT free. */
extern void hxnet_htxf_abort (const HtxfAbort *token);

/* Drop the C side's ref to `token`. NULL-safe. */
extern void hxnet_htxf_abort_free (const HtxfAbort *token);

/* ---- C-side shim --------------------------------------------------- */

/* Allocate the cancellation token onto htxf->abort. Main-thread call
 * at transfer creation. No-op if a token is already present. */
extern void htxf_io_abort_init (struct htxf_conn *htxf);

/* Arm htxf->abort with htxf->hx's socket. Worker-thread call, once,
 * after the channel is open. No-op if either is NULL. */
extern void htxf_io_abort_arm (struct htxf_conn *htxf);

/* Trigger cancellation: unblock a parked htxf_io_read / _write by
 * shutting the subchannel socket down. Main-thread call. NULL-safe. */
extern void htxf_io_abort (struct htxf_conn *htxf);

/* Free htxf->abort and clear the slot. Called at htxf teardown. */
extern void htxf_io_abort_free (struct htxf_conn *htxf);

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

#endif /* GTKHX_HTXF_IO_H */
