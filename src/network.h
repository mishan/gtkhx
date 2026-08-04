#ifndef HX_NETWORK_H
#define HX_NETWORK_H

#include <gio/gio.h>

extern void hx_htlc_close (struct htlc_conn *htlc, int expected);

/* Orchestrator (hxnet) TOFU certificate verify. Called from the
 * hxnet bridge's verify_cert callback after the Rust TLS handshake,
 * with the peer leaf cert's "sha256:<hex>" fingerprint (computed in
 * Rust to match hx_tls_trust_fingerprint). Looks it up in the
 * known-hosts store and accepts / prompts / pins exactly like the
 * legacy GTlsConnection accept-certificate path. Returns TRUE to
 * accept, FALSE to reject. */
extern gboolean hx_tls_orchestrator_verify_cert (struct htlc_conn *htlc,
                                                 const char *fingerprint);

/* host:port-keyed variant of the TOFU verify, for the HTXF subchannel
 * workers (banner.c) that snapshot the endpoint rather than hold an
 * htlc. Same known-hosts decision, keyed on the subchannel's own
 * host:port — the endpoint the pre-rewire GTlsConnection handler used.
 * Returns TRUE to accept, FALSE to reject. */
extern gboolean hx_tls_verify_subchannel_cert (const char *host, guint16 port,
                                               const char *fingerprint);

/* Phase G: register the orchestrator's "login" protocol task. Called
 * from the hxnet bridge's LOGIN_SENDING state callback so the login
 * task appears at the same point the legacy connect path registers it
 * (magic done, credentials going out) rather than up front. */
extern void hx_orchestrator_register_login_task (struct htlc_conn *htlc);

/* `secure` is the legacy hxd "secure server" password flag (not
 * transport security — it just selects an alternate password
 * encoding). `tls` is the transport-security flag: non-zero
 * wraps the control-channel socket in GTlsClientConnection from
 * byte zero (see docs/tls.md "The separate-port model"). The default port
 * for TLS is 5600 by Mobius/Janus convention; the caller is
 * responsible for handing that in via `port` — hx_connect
 * doesn't auto-translate. The Connect dialog (Phase 4) flips
 * the port when the TLS toggle changes; bookmarks carry their
 * own port + tls flag.
 *
 * Cert trust policy (Phase 3): the accept-certificate signal
 * handler computes a SHA-256 fingerprint over the cert DER,
 * looks it up in $CONFIG/known_hosts (SSH known_hosts shape),
 * and either silently accepts (TRUSTED) or prompts the user via
 * an AdwAlertDialog (UNKNOWN / MISMATCH) before pinning. The
 * GTKHX_KNOWN_HOSTS env var overrides the path for tests.
 * GTKHX_TLS_AUTO_ACCEPT=1 bypasses the dialog and auto-pins —
 * intended for the Tier 3 headless test harness; production
 * users always see the prompt.
 *
 * Env-var override: setting GTKHX_TLS=1 forces tls=1 regardless
 * of the parameter. Mostly a leftover from before Phase 4 wired
 * the Connect-dialog toggle; kept for power-user scripting. */
extern void hx_connect (struct htlc_conn *htlc, const char *serverstr,
                        guint16 port, const char *login, const char *pass,
                        char secure, char tls);

extern void kill_threads (void);

/* Open the HTXF subchannel for `htxf`: look up the SOCKS proxy for the
 * target, then hand the host:port (+ proxy, the packed 16/24-byte
 * preamble, the per-transfer AEAD keys when the control channel
 * negotiated CIPHER_MODE_AEAD, and the TLS flag) to hxnet_htxf_connect.
 * hxnet does the whole connect itself — DNS + IPv4/IPv6 fallback +
 * optional SOCKS tunnel — and owns the socket, the optional rustls wrap,
 * and the AEAD framing thereafter; the handle is stored on htxf->hx.
 * Returns TRUE on success, FALSE on connect / handshake / open failure.
 *
 * Worker threads then stream bytes through htxf_io_read /
 * htxf_io_write and close the channel via htxf_io_release. */
extern gboolean htxf_connect (struct htxf_conn *htxf);

/* The control-channel send primitive. Packs the (type, flag, chunk-array)
 * message and hands the bytes to hxnet, tearing the connection down on a send
 * failure. Defined in Rust (hxtask::send); the wire format is built natively by
 * hotline-proto. The old variadic hlwrite front door was dead in production and
 * has been retired — build a struct hx_chunk[] and call this directly. */
struct hx_chunk;
extern void hlwrite_chunks (struct htlc_conn *htlc, guint32 type, guint32 flag,
                            const struct hx_chunk *chunks, int hc);

extern void hl_code (void *__dst, const void *__src, size_t len);

/* PING keepalive — start the periodic timer once login has
 * succeeded and stop it when the connection ends. ping_stop is also
 * called from inside hx_htlc_close, so callers never need to pair
 * a stop with a start. */
extern void ping_start (struct htlc_conn *htlc);
extern void ping_stop (struct htlc_conn *htlc);

/* send HTLC_HDR_AGREEMENTAGREE carrying NAME + ICON. The
 * legacy Hotline two-stage login flow defers identity disclosure
 * to this message; see the comment block above login_dispatch in
 * network.c. Triggered by the Agree button on the agreement window
 * (gtkhx.c::concurrence). */
extern void hx_send_agreement_agree (struct htlc_conn *htlc);

#endif /* HX_NETWORK_H */
