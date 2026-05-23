#ifndef HX_NETWORK_H
#define HX_NETWORK_H

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

extern int htxf_connect (struct htxf_conn *htxf);

/* Worker-thread connect helper: blocking GSocketClient connect to
 * host:port, returns a blocking-mode raw fd the caller must close.
 * On failure returns -1 and writes the GError message to errbuf if
 * non-NULL. Used by HTXF (file transfer) workers in xfers.c and
 * banner.c, both of which need a connected fd to do blocking
 * byte-streaming with read(2) / write(2). */
extern int hx_sync_connect_to_host (const char *host, guint16 port,
                                    char *errbuf, gsize errbuf_len);

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
