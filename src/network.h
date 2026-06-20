#ifndef HX_NETWORK_H
#define HX_NETWORK_H

#include <gio/gio.h>

/* we only want pthread.h if we're not in debug mode */
#ifndef DEBUG
#include <pthread.h>

extern pthread_t conn_tid;
#endif

extern char *server_addr;
extern guint16 server_port;
extern struct log *server_log;

extern int connected;

extern int fd_closeonexec (int fd, int on);
extern int fd_lock_write (int fd);

extern void hx_htlc_close (struct htlc_conn *htlc, int expected);

/* `secure` is the legacy hxd "secure server" password flag (not
 * transport security — it just selects an alternate password
 * encoding). `tls` is the transport-security flag: non-zero
 * wraps the control-channel socket in GTlsClientConnection from
 * byte zero (see docs/tls-scoping.md Phase 1). The default port
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

/* Open the HTXF subchannel for `htxf` and send the 16/24-byte
 * plaintext preamble. Returns a connected GSocketConnection that
 * the caller owns (g_object_unref drops both the GIO machinery
 * and the underlying socket). On failure returns NULL.
 *
 * Worker threads cast the returned conn to GIOStream and feed it
 * to htxf_io_read / htxf_io_write — both now stream-shaped and
 * AEAD-aware. The dup() + manual O_NONBLOCK toggle the old
 * fd-returning shape needed are gone; GSocketConnection is
 * blocking by default and the GIOStream APIs handle EINTR.
 *
 * AEAD subchannel keys (HOPE+ChaCha20) are armed before return
 * when the control channel negotiated CIPHER_MODE_AEAD. */
extern GSocketConnection *htxf_connect (struct htxf_conn *htxf);

/* Worker-thread blocking GSocketClient connect to host:port.
 * Returns a connected GSocketConnection on success (caller owns,
 * g_object_unref drops both the GIO objects and the socket fd),
 * NULL on failure. SOCKS proxying flows through GProxyResolver
 * automatically — same as the rest of GIO. Used by HTXF (file
 * transfer) workers in xfers.c and banner.c.
 *
 * Writes the GError message to errbuf (truncated to errbuf_len)
 * if both are non-NULL on failure.
 *
 * `tls` (Phase 2): when non-zero, flips
 * g_socket_client_set_tls(client, TRUE) before the connect and
 * hooks the same TOFU accept-certificate handler at the
 * G_SOCKET_CLIENT_TLS_HANDSHAKING phase that hx_connect uses
 * (see network.c::tls_accept_certificate). Trust lookups hit
 * the same $CONFIG/known_hosts file, so the HTXF subchannel
 * cert is pinned per (host, port) just like the control
 * channel. Callers should pass htlc->tls so the subchannel
 * mirrors the control channel.
 *
 * The returned GSocketConnection is actually a
 * GTlsClientConnection in the tls=1 case; the rest of the GIO
 * stream APIs (g_io_stream_get_input_stream etc.) work the same
 * way on both shapes. */
extern GSocketConnection *hx_sync_connect_to_host (const char *host,
                                                   guint16 port,
                                                   char *errbuf,
                                                   gsize errbuf_len,
                                                   char tls);

extern void hlwrite (struct htlc_conn *htlc, guint32 type, guint32 flag, int hc,
                     ...);

/* Chunk-array variant of hlwrite. Same trace + write + cipher +
 * compress side-effects, but the chunks come from a caller-built
 * array (typically populated via login_packet.c::hx_login_build_chunks
 * or another shared message builder). Defined in network.c. */
struct hx_chunk;
extern void hlwrite_chunks (struct htlc_conn *htlc, guint32 type, guint32 flag,
                            const struct hx_chunk *chunks, int hc);

extern void hl_code (void *__dst, const void *__src, size_t len);

/* PING keepalive — start the periodic timer once login has
 * succeeded and stop it when the connection ends. ping_stop is also
 * called from inside hx_htlc_close, so callers never need to pair
 * a stop with a start. */
extern void ping_start (struct htlc_conn *htlc);
extern void ping_stop (void);

/* send HTLC_HDR_AGREEMENTAGREE carrying NAME + ICON. The
 * legacy Hotline two-stage login flow defers identity disclosure
 * to this message; see the comment block above login_dispatch in
 * network.c. Triggered by the Agree button on the agreement window
 * (gtkhx.c::concurrence). */
extern void hx_send_agreement_agree (struct htlc_conn *htlc);

/* Phase R3.3.e-4d/4e: install the hxnet bridge over the current
 * connection's fd. Called from rcv.c after HOPE handshake
 * completes (or after non-HOPE login succeeds, for 1.0/1.2
 * servers). Builds the transform stack from the negotiated
 * cipher / compression state on `htlc`, dup()s the fd, disarms
 * the legacy GIOStream sources, and clears `htlc->cipher_*_type`
 * / `htlc->compress_*_type` so the C-side encoders don't
 * double-cipher / double-compress on the send path.
 *
 * As of R3.3.e-4e, hxnet is the default: install runs unless
 * the user opts out via `GTKHX_USE_HXNET=0`. Also a no-op when
 * the connection is TLS or the bridge is already installed.
 * Logs the failure mode via `g_critical` on any reachable
 * error path. */
extern void hx_install_hxnet_post_hope (struct htlc_conn *htlc);

#endif /* HX_NETWORK_H */
