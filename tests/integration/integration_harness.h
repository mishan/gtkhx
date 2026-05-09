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
                                          guint32 type, guint32 flag,
                                          int hc, ...);

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

#endif /* HX_INTEGRATION_HARNESS_H */
