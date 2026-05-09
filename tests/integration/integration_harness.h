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

#endif /* HX_INTEGRATION_HARNESS_H */
