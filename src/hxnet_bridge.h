/*
 * hxnet_bridge.h — translation layer between hxnet's callback FFI
 * and gtkhx's existing rcv state machine. Phases R3.3.e-4a /
 * R3.3.e-4b.
 *
 * hxnet emits plaintext Hotline frames as HxnetFrame structs
 * delivered to a C callback on the GLib main thread. The
 * existing rcv path in src/rcv.c reads from htlc->in (a 22-byte
 * header buffer; resized for the body after hx_rcv_hdr decodes
 * the type) and runs body handlers via the rcv dispatch. These helpers
 * marshal an HxnetFrame into the rcv state machine without
 * involving hx_decode (the cipher / compression layers are
 * already handled by hxnet's transform stack — the bytes
 * arriving here are plaintext).
 *
 * The bridge is intentionally side-effect-free outside of htlc:
 * it touches htlc->in and the body handlers; it
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

#ifndef GTKHX_HXNET_BRIDGE_H
#define GTKHX_HXNET_BRIDGE_H

#include <glib.h>
#include <stdint.h>

#include "hx.h"
#include "protocol.h"

G_BEGIN_DECLS

/*
 * Run a plaintext Hotline frame through the rcv state machine.
 *
 * Stages the packed 22-byte header + body bytes into
 * `htlc->in`, calls `hx_rcv_hdr` (which decodes the header and
 * selects the appropriate body handler), then
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
 * Phase G (hxnet-owns-the-whole-lifecycle): open a plaintext
 * Hotline connection with hxnet driving the entire pre-frame
 * lifecycle (DNS + TCP + magic + LOGIN + LOGIN-reply), then
 * install the resulting handle as the live bridge.
 *
 * hxnet owns the socket from byte zero (it calls
 * hxnet_connection_open_plaintext), so the C side never has a real
 * fd. The
 * orchestrator replays the LOGIN reply back as a synthetic frame
 * (Option B in docs/rust/networking.md) so the C-side rcv
 * dispatch (rcv_task_login) runs unchanged.
 *
 * The bridge's own event / shutdown / state callbacks are wired
 * in; state transitions are mapped onto GtkhxConnectionState and
 * emitted on the default GtkhxSession (same coarse sequence the
 * legacy GIOStream connect path emits: CONNECTING → TCP_CONNECTED
 * → HANDSHAKE_DONE).
 *
 * `host` is a NUL-terminated server name / IP; `login` / `pass` /
 * `name` are NUL-terminated (NULL treated as empty). `trans` is the
 * transaction id the orchestrator stamps on the LOGIN frame — the
 * caller pins it and registers a matching login task so the
 * replayed reply dispatches correctly.
 *
 * Returns TRUE on a successful spawn (the handle is now the live
 * bridge), FALSE on failure (open_plaintext logged its own
 * g_critical; the bridge is left uninstalled).
 *
 * Precondition: no prior install is live. Plaintext only — TLS /
 * HOPE-secure logins still go through the legacy connect path.
 */
extern gboolean hx_bridge_install_orchestrated_plaintext (
    struct htlc_conn *htlc, const char *host, guint16 port, const char *login,
    const char *pass, const char *name, guint16 icon, guint16 version,
    guint16 caps, guint32 trans);

/*
 * HOPE sibling of hx_bridge_install_orchestrated_plaintext: hxnet
 * drives the full HOPE-Secure-Login handshake (magic + step1 + step2
 * + cipher transition) and the encrypted post-login stream, then the
 * handle becomes the live bridge. `cipher_alg` is the wire cipher
 * label to advertise ("BLOWFISH" / "CHACHA20-POLY1305"); HOPE
 * requires a non-empty cipher. `trans` is the step-1 transaction id
 * (the step-2 reply, which gets replayed, carries `trans + 1` — the
 * caller registers its login task under that value).
 */
extern gboolean hx_bridge_install_orchestrated_hope (
    struct htlc_conn *htlc, const char *host, guint16 port, const char *login,
    const char *pass, const char *name, guint16 icon, guint16 version,
    guint16 caps, guint32 trans, const char *cipher_alg);

/*
 * TLS sibling of hx_bridge_install_orchestrated_plaintext: plaintext
 * Hotline over TLS-from-byte-zero (Mobius / Janus separate-port
 * model). hxnet does the TLS handshake then the plaintext lifecycle
 * over the encrypted stream. The replayed reply is the LOGIN reply
 * (trans = `trans`), same as the non-TLS plaintext path.
 *
 * Cert trust is WebPKI-first: rustls validates the server cert against
 * the native trust roots, and a CA-valid cert is accepted silently.
 * Only when WebPKI validation fails does the bridge's verify_cert
 * callback (hx_tls_orchestrator_verify_cert) run the known-hosts TOFU
 * decision post-handshake, before LOGIN — so the callback is NOT
 * guaranteed to run on every connection.
 */
extern gboolean hx_bridge_install_orchestrated_plaintext_tls (
    struct htlc_conn *htlc, const char *host, guint16 port, const char *login,
    const char *pass, const char *name, guint16 icon, guint16 version,
    guint16 caps, guint32 trans);

/*
 * TRUE when `htlc` has a transport installed.
 *
 * Per-connection, because every caller is asking about a specific
 * connection: "may I send on this one", "is this one's transport still up".
 * It used to take no argument and answer for the process, which at one
 * connection was the same question and at two would not be.
 */
extern gboolean hx_bridge_is_installed (const struct htlc_conn *htlc);

/*
 * Opaque HOPE AEAD material handle for the installed orchestrated
 * connection `htlc`, or NULL (no transport installed on it, or no
 * ChaCha20-Poly1305 negotiated). Caller owns it and frees with hxnet_hope_aead_free.
 * HxnetHopeAead is declared in htxf_io.h. See the definition in
 * hxnet_bridge.c for the lifecycle contract (call after login).
 */
struct HxnetHopeAead;
extern struct HxnetHopeAead *
hx_bridge_orchestrated_hope_aead (const struct htlc_conn *htlc);

/*
 * Ask GProxyResolver whether (host, port) is reached through a SOCKS
 * proxy; returns the proxy URI (caller g_free's) or NULL for direct.
 * Shared by the control-channel install paths and the HTXF subchannel
 * connect (network.c / banner.c) so every connect honours the same proxy
 * config. Only SOCKS results are returned; non-SOCKS (e.g. http://) are
 * warned + skipped. See the definition in hxnet_bridge.c for the
 * lookup-URI scheme + the synchronous-lookup note.
 */
extern char *hx_bridge_lookup_socks_proxy (const char *host, guint16 port);

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
extern int hx_bridge_send_frame (struct htlc_conn *htlc, const guint8 *data,
                                 guint32 len);

/*
 * Tear down `htlc`'s hxnet handle. Drops the ConnectionHandle, which
 * signals the actor to flush pending writes and exit; the wrapped
 * TcpStream's Drop closes the fd. Safe to call multiple times — second +
 * subsequent calls are silent no-ops. Other connections are untouched.
 */
extern void hx_bridge_uninstall (struct htlc_conn *htlc);

G_END_DECLS

#endif /* GTKHX_HXNET_BRIDGE_H */
