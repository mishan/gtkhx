/*
 * hxnet_bridge.h — translation layer between hxnet's callback FFI
 * and gtkhx's existing rcv state machine. Phases R3.3.e-4a /
 * R3.3.e-4b.
 *
 * hxnet emits plaintext Hotline frames as HxnetFrame structs
 * delivered to a C callback on the GLib main thread. The
 * existing rcv path in src/rcv.c reads from htlc->in (a 22-byte
 * header buffer; resized for the body after hx_rcv_hdr decodes
 * the type) and runs body handlers via htlc->rcv. These helpers
 * marshal an HxnetFrame into the rcv state machine without
 * involving hx_decode (the cipher / compression layers are
 * already handled by hxnet's transform stack — the bytes
 * arriving here are plaintext).
 *
 * The bridge is intentionally side-effect-free outside of htlc:
 * it touches htlc->in, htlc->rcv, and the body handlers; it
 * does NOT touch any GIOStream / GPollable state directly.
 *
 * R3.3.e-4a (shipped): the dispatch translation (pack_header /
 * dispatch_frame / dispatch_shutdown).
 *
 * R3.3.e-4b (this PR): install / send / uninstall helpers that
 * wrap hxnet's callback FFI, with the C-side on_event /
 * on_shutdown trampolines that call back into dispatch_*.
 * Production glue (the env-var gating in network.c) is
 * R3.3.e-4c.
 */

#ifndef _HXNET_BRIDGE_H
#define _HXNET_BRIDGE_H

#include <glib.h>
#include <stdint.h>

#include "hx.h"
#include "protocol.h"

G_BEGIN_DECLS

/*
 * Pack a Hotline 22-byte message header into `dst`.
 *
 *   dst        : caller-supplied buffer of at least SIZEOF_HL_HDR
 *                bytes. Must be writable.
 *   type/trans/flag/hc : host-order fields from a decoded
 *                HxnetFrame.
 *   body_len   : body byte count (no hc adjustment — this is the
 *                hxnet "body" length, matching
 *                hotline_proto::parse::HeaderDecoded.body_len).
 *
 * The wire `len` field is encoded as `body_len + sizeof(hc)` so
 * production's hl_hdr_decode reports the same body_len back via
 * its `body_len_out` parameter. `len2` is set to zero (legacy
 * unused field).
 *
 * Visibility: declared here for testability — tests/unit/
 * test_hxnet_bridge.c exercises the round-trip against
 * hl_hdr_decode without standing up a struct htlc_conn.
 */
extern void hx_bridge_pack_header (guint8 *dst, guint32 type, guint32 trans,
                                   guint32 flag, guint16 hc, guint32 body_len);

/*
 * Run a plaintext Hotline frame through the rcv state machine.
 *
 * Stages the packed 22-byte header + body bytes into
 * `htlc->in`, calls `hx_rcv_hdr` (which decodes the header and
 * sets `htlc->rcv` to the appropriate body handler), then
 * dispatches the body handler if one was selected and a body is
 * present. Leaves `htlc->in` in the "expecting next header"
 * state when the dispatch returns (matches what
 * control_on_readable's reset-leg leaves behind today, so a
 * follow-up call works cleanly).
 *
 * `body` may be NULL when `body_len == 0`; otherwise it must
 * point at `body_len` readable bytes. The bytes are copied out
 * of `body` before the body handler runs, so the caller may
 * free `body` immediately on return.
 */
extern void hx_bridge_dispatch_frame (struct htlc_conn *htlc, guint32 type,
                                      guint32 trans, guint32 flag, guint16 hc,
                                      const guint8 *body, guint32 body_len);

/*
 * Translate an hxnet shutdown reason into hx_htlc_close's
 * `expected` flag and tear the connection down.
 *
 *   HXNET_SHUTDOWN_EOF (0)             → expected = 1 (clean
 *                                        peer-side close)
 *   HXNET_SHUTDOWN_HANDLE_DROPPED (3)  → expected = 1 (we
 *                                        initiated the close)
 *   HXNET_SHUTDOWN_STREAM_ERROR (1)    → expected = 0
 *   HXNET_SHUTDOWN_FRAME_TOO_LARGE (2) → expected = 0
 *
 * Unknown reason codes are conservatively treated as
 * unexpected so the user sees the error dialog rather than a
 * silent disconnect.
 */
extern void hx_bridge_dispatch_shutdown (struct htlc_conn *htlc, int reason);

/*
 * Lifecycle helpers wrapping hxnet's callback FFI (R3.3.e-4b).
 *
 * The bridge owns one hxnet connection handle at a time — a
 * single global, since gtkhx is single-connection today.
 * Production wiring (R3.3.e-4c) calls these from network.c's
 * send_login / hlwrite / hx_htlc_close sites; the standalone
 * Tier 1 test exercises the install / send / uninstall
 * lifecycle on a TCP loopback pair end-to-end.
 */

/*
 * Adopt `fd` as the underlying TCP socket and spawn an hxnet
 * Connection actor on it with a passthrough transform stack
 * (cipher=NONE, compression=NONE). The actor's `on_event`
 * callback routes through hx_bridge_dispatch_frame on the
 * supplied htlc; `on_shutdown` routes through
 * hx_bridge_dispatch_shutdown.
 *
 * Ownership: hxnet takes the fd. The C side must NOT
 * `close(fd)` after a successful return — that's
 * double-close UB.
 *
 * Returns TRUE on success. On failure, logs via `g_critical`,
 * leaves the bridge uninstalled, and the fd is closed by
 * hxnet's spawn path (so the caller still doesn't close it).
 *
 * Precondition: no prior install is live (call
 * hx_bridge_uninstall first if you're recycling).
 */
extern gboolean hx_bridge_install_passthrough (struct htlc_conn *htlc,
                                               int fd);

/*
 * Adopt `fd` and spawn an hxnet Connection actor with a
 * transform stack reconstructed from `htlc`'s post-HOPE cipher
 * state. Phase R3.3.e-4d (HOPE-over-hxnet).
 *
 * Reads the negotiated cipher / compression choice from:
 *
 *   htlc->cipher_encode_type    — CIPHER_NONE / CIPHER_BLOWFISH /
 *                                 CIPHER_CHACHA20_POLY1305
 *   htlc->cipher_decode_type
 *   htlc->cipher_encode_state   — union cipher_state
 *   htlc->cipher_decode_state
 *   htlc->cipher_encode_key     — Blowfish only (the symmetric
 *                                 key used to construct
 *                                 BlowfishOfb64State)
 *   htlc->cipher_encode_keylen
 *   (decode counterparts likewise)
 *
 * For Blowfish: extracts the live OFB ivec via
 * `gtkhx_blowfish_ofb64_save_state` so the negotiated stream
 * position survives the handoff, and reuses the symmetric key
 * stored on htlc. For ChaCha20-Poly1305: copies the
 * chacha_aead_state's key / counter / dir directly — they map
 * 1:1 to hxcrypto-aead's `AeadState`.
 *
 * Compression is wired through the same way (HOPE-negotiated
 * GZIP / LZ4 / ZSTD become the matching adapter in the
 * transform stack).
 *
 * Ownership: same as `hx_bridge_install_passthrough` — hxnet
 * adopts `fd`; the C side must not close() it after success.
 *
 * Returns TRUE on success. On failure (cipher state not
 * extractable, hxnet spawn refused), logs via g_critical,
 * leaves the bridge uninstalled, hxnet closes the fd.
 *
 * Precondition: no prior install is live; `htlc->fd > 0`;
 * negotiated cipher state has been fully initialised by
 * cipher_*_init.
 *
 * R3.3.e-4d (this PR) ships the bridge-side helper only. The
 * matching production switch in rcv.c (move install from
 * send_login to post-HOPE; drop the cipher-active tear-down)
 * is the follow-up R3.3.e-4d-cont commit.
 */
extern gboolean hx_bridge_install_with_hope_state (struct htlc_conn *htlc,
                                                   int fd);

/*
 * TLS-aware install. Same role as `hx_bridge_install_with_hope_state`
 * but for connections that use TLS as the transport instead of the
 * legacy HOPE-negotiated cipher / compression stack. TLS provides
 * confidentiality + integrity already; HOPE on top is double-
 * encryption and the scoping doc forbids it. `htlc->host` /
 * `htlc->serverport` are read for the SNI hostname + port that
 * surface to the trust callback.
 *
 * The fd hand-over and install-defer semantics match
 * `hx_bridge_install_with_hope_state`. The TLS handshake itself
 * runs asynchronously on the tokio runtime after this function
 * returns — the call is non-blocking. Handshake failures surface
 * via the shutdown callback as `StreamError("TLS handshake
 * failed: ...")` before any frames flow.
 */
extern gboolean hx_bridge_install_tls (struct htlc_conn *htlc, int fd);

/*
 * TRUE when an hxnet connection is currently installed.
 * Production code uses this as the gate between the new
 * (hxnet) and legacy (GIOStream) read / write paths.
 */
extern gboolean hx_bridge_is_installed (void);

/*
 * Sentinel returned by [`hx_bridge_send_frame`] when no hxnet
 * connection is currently installed. Sits outside the
 * HXNET_SEND_* range (0, -1, -2, -3) so callers can
 * distinguish "bridge not installed" from kernel backpressure
 * or channel closure. -100 is well clear of any legitimate
 * hxnet FFI return code.
 */
#define HX_BRIDGE_SEND_NOT_INSTALLED (-100)

/*
 * Enqueue `len` bytes of `data` for transmission via hxnet's
 * `hxnet_connection_send_frame`. The bytes are typically a
 * fully packed Hotline frame (header + body) from
 * htlc->out — production's hlwrite path packs into a scratch
 * buffer and calls this when an hxnet connection is live.
 *
 * Returns:
 *   0                              — success
 *   HXNET_SEND_FULL (-1)           — channel full, retry later
 *   HXNET_SEND_CLOSED (-2)         — actor exited
 *   HXNET_SEND_INVALID (-3)        — invalid args
 *   HX_BRIDGE_SEND_NOT_INSTALLED   — bridge not installed
 *
 * All non-zero returns are logged via `g_critical`. See the
 * HXNET_SEND_* constants in rust/crates/hxnet/src/ffi.rs for
 * the FFI-level reasons.
 */
extern int hx_bridge_send_frame (const guint8 *data, guint32 len);

/*
 * Tear down the installed hxnet handle. Drops the
 * ConnectionHandle, which signals the actor to flush pending
 * writes and exit; the wrapped TcpStream's Drop closes the
 * fd. Safe to call multiple times — second + subsequent calls
 * are silent no-ops.
 */
extern void hx_bridge_uninstall (void);

G_END_DECLS

#endif /* _HXNET_BRIDGE_H */
