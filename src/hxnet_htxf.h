/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * hxnet_htxf.h — C declarations for hxnet's Rust HTXF (file-transfer)
 * subchannel FFI (rust/crates/hxnet/src/htxf.rs).
 *
 * The whole HTXF transport — the byte pump, the AEAD framing
 * (ChaCha20-Poly1305 length-prefixed frames), the optional rustls TLS wrap,
 * the socket fd, the cancellation token, and the handshake preamble packer —
 * lives in the hxnet crate. struct htxf_conn carries the opaque channel handle
 * in `htxf->hx` and the cancellation token in `htxf->abort`; the C drivers
 * (network.c::htxf_connect, the xfers.c workers, banner.c) call the
 * hxnet_htxf_* functions below directly. (The old htxf_io.c C shim was retired
 * in S1.2: its read/write/close/abort wrappers collapsed into these calls +
 * hxnet_htxf_read_full, and the preamble packer moved to hxnet in S1.1.)
 */

#ifndef GTKHX_HXNET_HTXF_H
#define GTKHX_HXNET_HTXF_H

#include "config.h"
#include <glib.h>
#include <sys/types.h> /* ssize_t */

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
 * All defined in rust/crates/hxnet/src/htxf.rs. */

/* TOFU verify-cert callback for a TLS subchannel whose peer cert did
 * not chain to a public root: receives the "sha256:<hex>" leaf
 * fingerprint, returns non-zero to accept the connection. */
typedef int (*hxnet_htxf_verify_cb_t) (const guint8 *fp, gsize fp_len,
                                       void *user_data);

/* Connect an HTXF subchannel to host:port (optionally through a SOCKS
 * proxy) entirely in Rust, then open it — no OS socket fd crosses the
 * FFI. `proxy_uri` (length proxy_uri_len) is an optional "socks5://..."
 * URI; NULL/0 connects direct, a malformed/unsupported URI fails the
 * open. `host` is required (connect target + TLS SNI / TOFU name).
 * `tls != 0` TLS-handshakes with rustls; `preamble` is written raw
 * before AEAD arms; a non-NULL `hope_aead` arms per-transfer AEAD
 * framing, with the keys derived in-process from that control-channel
 * material + `xfer_ref` (the session key never crosses the FFI), while
 * NULL selects plaintext passthrough. The connect runs on the tokio
 * runtime (bounded by the shared handshake timeout) and blocks the
 * calling worker for the result. Returns an owned handle, or NULL on a
 * bad argument / connect / TLS / TOFU failure. */
extern HtxfConn *hxnet_htxf_connect (const guint8 *host, size_t host_len,
                                     guint16 port, const guint8 *proxy_uri,
                                     size_t proxy_uri_len, int tls,
                                     const guint8 *preamble,
                                     size_t preamble_len,
                                     const HxnetHopeAead *hope_aead,
                                     guint32 xfer_ref,
                                     hxnet_htxf_verify_cb_t verify_cert,
                                     void *user_data);

/* Pack the HTXF subchannel handshake preamble into buf[..cap] (S1.1, was
 * hx_htxf_subchannel_pack_preamble in the retired htxf_subchannel.c). Returns
 * the bytes written (16, or 24 for the size64 large-file variant), or 0 on a
 * NULL/too-small buffer or a >4 GiB size in the legacy 16-byte form. */
extern size_t hxnet_htxf_pack_preamble (guint8 *buf, size_t cap, guint32 ref,
                                        guint64 total_size, guint16 type,
                                        guint16 flags, int size64);

/* Blocking read of up to `len` bytes (`0` = clean EOF, `-1` on error). */
extern ssize_t hxnet_htxf_read (HtxfConn *handle, guint8 *buf, size_t len);

/* Read exactly `len` bytes into `buf`, looping over hxnet_htxf_read (a short
 * read / clean EOF before `len` is an error). Returns `len` on success, `-1`
 * on error / truncation. `len == 0` is a no-op success. */
extern ssize_t hxnet_htxf_read_full (HtxfConn *handle, guint8 *buf, size_t len);

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
 * Cooperative-cancel foundation (Phase R3 X1). A token is created by
 * hx_htxf_new (S0.3), armed with the channel's socket once it opens
 * (worker, hxnet_htxf_abort_arm), and aborted from the main thread
 * (hxnet_htxf_abort) to shut that socket down and unblock a parked
 * blocking read/write. Reference-counted: hxnet_htxf_abort_new yields the
 * C side's ref (freed with hxnet_htxf_abort_free); hxnet_htxf_abort_arm
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

#endif /* GTKHX_HXNET_HTXF_H */
