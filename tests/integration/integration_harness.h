#ifndef HX_INTEGRATION_HARNESS_H
#define HX_INTEGRATION_HARNESS_H 1

/*
 * Tier 3 integration-test harness — drives a real TCP connection
 * to a Hotline server (the mhxd Docker container under tests/mhxd
 * by default) and exercises the protocol layer end-to-end.
 *
 * The harness is intentionally minimal: it opens a socket, runs
 * the magic handshake, and gives tests blocking send / receive
 * primitives over the connection. No worker threads, no GTK, no
 * libadwaita — the test binary links against proto_helpers and
 * the system socket APIs and that's it.
 *
 * Skip-if-unavailable contract: if the server isn't reachable
 * within a short timeout, integration_connect returns -1 and the
 * test is expected to call g_test_skip and return. That way the
 * tests run on machines with mhxd running and skip silently
 * elsewhere.
 *
 * Configuration via env vars:
 *
 *   GTKHX_TEST_HOST   — server hostname (default: 127.0.0.1)
 *   GTKHX_TEST_PORT   — server port     (default: 5500)
 *
 * The first time integration_connect is called the harness reads
 * these and caches them.
 */

#include <glib.h>
#include "server_matrix.h"

/*
 * Open a TCP connection to the configured host:port.
 *
 * Returns the socket fd on success, or -1 if:
 *   - the host can't be resolved
 *   - the connection is refused
 *   - the connection takes longer than the timeout (default 2 s)
 *
 * The caller closes the fd via integration_close.
 */
extern int integration_connect (void);

/*
 * Run the 12-byte client magic + 8-byte server magic exchange:
 *
 *   client → "TRTPHOTL\0\1\0\2"  (HTLC_MAGIC)
 *   server → "TRTP\0\0\0\0"       (HTLS_MAGIC)
 *
 * Returns TRUE on a clean exchange, FALSE on any read/write error
 * or magic-byte mismatch.
 */
extern gboolean integration_handshake (int fd);

/*
 * Read exactly `len` bytes from `fd` into `buf`. Blocks until all
 * bytes arrive, an error occurs, or the read times out (default
 * 5 s). Returns TRUE on success, FALSE on partial / failed reads.
 */
extern gboolean integration_recv (int fd, void *buf, gsize len);

/*
 * Write exactly `len` bytes from `buf` to `fd`. Blocks until all
 * bytes are written or an error occurs. Returns TRUE on success.
 */
extern gboolean integration_send (int fd, const void *buf, gsize len);

/*
 * Close the integration socket. Safe to call with -1 (no-op).
 */
extern void integration_close (int fd);

/*
 * Helper: combine integration_connect + integration_handshake. If
 * the connect fails, calls g_test_skip with an explanatory message
 * and returns -1. If the handshake fails after the connect,
 * g_test_fail and returns -1. On success returns the fd.
 *
 * The most common open of an integration test:
 *
 *   int fd = integration_open_or_skip ();
 *   if (fd < 0) return;
 *   ... drive the protocol over fd ...
 *   integration_close (fd);
 */
extern int integration_open_or_skip (void);

/*
 * Connect with a short timeout to an explicit host:port pair —
 * used by server_matrix.c so the matrix walker doesn't have to
 * duplicate the addrinfo + non-blocking-connect dance. Returns
 * the socket fd on success, -1 on resolve failure, connection
 * refused, or timeout. The legacy integration_connect() above
 * is a thin wrapper that reads GTKHX_TEST_HOST / GTKHX_TEST_PORT
 * and routes through this. Phase A multi-server work.
 */
extern int hx_integration_connect_to (const char *host, int port,
                                      int timeout_ms);

/* ---- Higher-level helpers built on send / recv ----------------- */

struct htlc_conn;

/*
 * Pack and send a Hotline message via the production hlpack helper.
 * Variadic args follow the same shape as hlwrite():
 *
 *   integration_send_message (fd, htlc, type, flag, hc,
 *                             type1, len1, data1,
 *                             type2, len2, data2, ...);
 *
 * `htlc` is used purely as scratch space — its in.buf may be nuked
 * by integration_recv_message. The caller owns lifetime; pass a
 * memset-zero'd struct htlc_conn on the stack and free its in/out
 * bufs at end of test via integration_release_htlc.
 *
 * Returns TRUE on full send.
 */
extern gboolean integration_send_message (int fd, struct htlc_conn *htlc,
                                          guint32 type, guint32 flag, int hc,
                                          ...);

/*
 * Read one full Hotline message into htlc->in (overwriting any
 * previous contents). The caller can then walk the chunks via
 * dh_start, drive proto_helpers extractors, etc.
 *
 * Times out after `timeout_ms` if no header arrives. Returns TRUE
 * on a clean read; FALSE on timeout, short read, or oversized
 * payload (> 1 MiB, which is the same cap network.c enforces).
 */
extern gboolean integration_recv_message (int fd, struct htlc_conn *htlc,
                                          int timeout_ms);

/*
 * Free any g_malloc'd qbuf memory inside htlc->in / htlc->out.
 * Safe to call on a memset-zero htlc.
 */
extern void integration_release_htlc (struct htlc_conn *htlc);

/*
 * Send a guest login (no password). This is the canonical "any
 * server should accept this" handshake — mhxd's default config
 * exposes a `guest` account with no password and basic permissions
 * (chat, file download, etc.).
 *
 * Sends HTLC_HDR_LOGIN with three chunks:
 *   HTLC_DATA_ICON   = `icon` big-endian
 *   HTLC_DATA_LOGIN  = hl_code-encoded "guest"
 *   HTLC_DATA_NAME   = `display_name` (utf8 bytes, unencoded)
 *
 * Servers that don't have a guest account will respond with a
 * task-error; the test should consume the next message and
 * inspect the flag bit.
 */
extern gboolean integration_login_guest (int fd, struct htlc_conn *htlc,
                                         const char *display_name,
                                         guint16 icon);

/*
 * Capability-aware guest login. Same wire shape as
 * integration_login_guest plus an HTLC_DATA_CAPABILITIES chunk
 * advertising `caps` (a bitmask of HTLC_CAP_* values from
 * hotline.h — typically HTLC_CAP_CHAT_HISTORY for the chat-history
 * extension test set, or 0 to advertise no capabilities).
 *
 * Cap-aware servers (Janus) echo the agreed-on bits in the TASK
 * login reply via HTLS_DATA_CAPABILITIES;
 * integration_drain_until_selfinfo_or_error stashes that echo
 * into htlc->caps so the caller can gate behaviour on the
 * negotiated bits the same way production gtkhx does in
 * src/rcv.c::rcv_task_login.
 *
 * Cap-unaware servers (mhxd) silently ignore the chunk per spec.
 */
extern gboolean integration_login_guest_caps (int fd, struct htlc_conn *htlc,
                                              const char *display_name,
                                              guint16 icon, guint16 caps);

/*
 * Drain server messages until we either see HTLS_HDR_USER_SELFINFO
 * (login fully accepted, server has sent us our session state) or
 * a task-error reply (login refused). Returns the wire type that
 * broke the loop, or 0 on timeout.
 *
 * The caller already-sent integration_login_guest before calling
 * this. The harness loops up to `max_messages` per call (8 by
 * default if you pass 0) so a server that interleaves AGREEMENT
 * or banner messages before SELFINFO is handled cleanly.
 *
 * On success the LAST received message is in htlc->in — typically
 * the SELFINFO or the task-error chunk — and the caller can run
 * extractors against it directly.
 */
extern guint32
integration_drain_until_selfinfo_or_error (int fd, struct htlc_conn *htlc,
                                           int max_messages);

/*
 * Compose the previous helpers: open the socket, run the magic
 * handshake, send a guest login, drain to SELFINFO. Returns the
 * fd on full success, or -1 if any step failed (with the
 * appropriate g_test_skip / g_test_fail already called). The
 * caller closes the fd when done via integration_close.
 *
 * `display_name` is the string the server sees for our chat /
 * user-list entry. `icon` is the small-icon id (412 is a sensible
 * default — that's GtkHx's own icon).
 *
 * On return, `htlc` is in the state the harness left it: the last
 * received message (the SELFINFO) is in htlc->in and the htlc
 * fields hx_selfinfo_parse fills (uid, icon, name, access) are
 * already populated.
 */
extern int integration_open_login_or_skip (struct htlc_conn *htlc,
                                           const char *display_name,
                                           guint16 icon);

/*
 * Variant of integration_open_login_or_skip that targets a specific
 * matrix entry and advertises `caps` to the server. Used by the
 * chat-history tests which need to talk to a chat-history-capable
 * server (Janus) regardless of which entry is the default.
 *
 * Picks `srv` (must be non-NULL — typically from
 * hx_test_servers_with), opens a TCP connection to its
 * host:port with a 2 s connect timeout, runs the magic handshake,
 * sends a guest LOGIN with the requested capability advertisement,
 * and drains to SELFINFO. Returns the connected fd, or -1 with
 * g_test_skip / g_test_fail already called.
 *
 * On return htlc is in the state the harness left it, including
 * htlc->caps populated with whatever bits the server echoed back
 * in HTLS_DATA_CAPABILITIES during the drain.
 */
extern int
integration_open_login_to_caps_or_skip (const hx_test_server *srv,
                                        struct htlc_conn *htlc,
                                        const char *display_name,
                                        guint16 icon, guint16 caps);

/*
 * Send HTLC_HDR_GET_CHAT_HISTORY (TRAN 700) for `channel_id`
 * with the same "0 means omit" cursor / limit semantics as
 * src/chat_history.c::hx_get_chat_history. Returns the trans
 * id assigned by hlpack (or 0 on send failure) — the caller
 * filters the TASK reply by matching trans against this value.
 *
 * The harness does NOT cap-gate this call: tests can issue it
 * even against a server that didn't echo CAP_CHAT_HISTORY, in
 * order to verify the server's task-error response. Production
 * gtkhx gates the send (src/chat_history.c:137) and won't
 * exercise that path.
 */
extern guint32 integration_send_get_chat_history (int fd,
                                                  struct htlc_conn *htlc,
                                                  guint32 channel_id,
                                                  guint64 before, guint64 after,
                                                  guint16 limit);

/* ---- HTXF subchannel helpers ---------------------------------- */

/*
 * Open a TCP connection to the file-transfer subchannel. mhxd
 * binds this on the main port + 1 (5501 by default), per
 * mhxd/src/hxd/files.c rcv_file_get's
 *     htxf->listen_sockaddr.SIN_PORT =
 *         htons(ntohs(...) + 1);
 *
 * Honors GTKHX_TEST_HOST; the subchannel port comes from
 * GTKHX_TEST_PORT + 1 (or GTKHX_TEST_XFER_PORT if set explicitly).
 *
 * Returns the fd on success, or -1 if the subchannel can't be
 * reached. The caller closes via integration_close.
 */
extern int integration_connect_xfer (void);

/*
 * Send the 16-byte HTXF transfer header that initiates a file
 * transfer subchannel exchange:
 *
 *   guint32 magic    = htonl(0x48545846)   // "HTXF"
 *   guint32 ref      = htonl(ref_from_TASK_reply)
 *   guint32 size     = htonl(total_size_from_TASK_reply)
 *   guint32 unknown  = 0
 *
 * After this header, the server (for downloads) starts streaming
 * the flat-file payload, or (for uploads) waits for the client
 * to stream it.
 *
 * Returns TRUE on a clean send.
 */
extern gboolean integration_send_xfer_hdr (int fd, guint32 ref,
                                           guint32 total_size);

#endif /* HX_INTEGRATION_HARNESS_H */
