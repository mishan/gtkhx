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
extern void hx_connect (struct htlc_conn *htlc, const char *serverstr,
                        guint16 port, const char *login, const char *pass,
                        char secure);

extern void kill_threads (void);

/* Open the HTXF subchannel for `htxf` and send the 16/24-byte
 * plaintext preamble. Returns a connected GSocketConnection that
 * the caller owns (g_object_unref drops both the GIO machinery
 * and the underlying socket). On failure returns NULL.
 *
 * Worker threads pull the fd back out via
 *   int s = g_socket_get_fd (g_socket_connection_get_socket (conn));
 * and pass it to htxf_io_read / htxf_io_write exactly as the
 * earlier dup-and-close-yourself shape did. The Phase A
 * conversion to GSocketConnection drops the dup and the manual
 * O_NONBLOCK toggle but doesn't change any worker logic; the
 * GIOStream-shaped htxf_io_* port lands in Phase B.
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
 * if both are non-NULL on failure. */
extern GSocketConnection *hx_sync_connect_to_host (const char *host,
                                                   guint16 port,
                                                   char *errbuf,
                                                   gsize errbuf_len);

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

/* Phase 5: PING keepalive — start the periodic timer once login has
 * succeeded and stop it when the connection ends. ping_stop is also
 * called from inside hx_htlc_close, so callers never need to pair
 * a stop with a start. */
extern void ping_start (struct htlc_conn *htlc);
extern void ping_stop (void);

/* Phase 5: send HTLC_HDR_AGREEMENTAGREE carrying NAME + ICON. The
 * legacy Hotline two-stage login flow defers identity disclosure
 * to this message; see the comment block above login_dispatch in
 * network.c. Triggered by the Agree button on the agreement window
 * (gtkhx.c::concurrence). */
extern void hx_send_agreement_agree (struct htlc_conn *htlc);

#endif /* HX_NETWORK_H */
