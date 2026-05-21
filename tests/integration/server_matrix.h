#ifndef HX_TEST_SERVER_MATRIX_H
#define HX_TEST_SERVER_MATRIX_H 1

/*
 * Multi-server test matrix — Phase A.
 *
 * Today the Tier 3 suite runs against a single mhxd Docker container
 * on localhost:5500. As we add more server families (Mobius for the
 * 1.9-style flow, eventually a chat-history-capable server, ...) we
 * want individual tests to fan out across the matrix so a base-
 * protocol round-trip is exercised against every target in one CI
 * cycle, while capability-gated tests skip targets that don't
 * advertise the feature.
 *
 * Phase A (this header) adds:
 *
 *   - hx_test_server struct describing one server family
 *   - hx_test_server_matrix[] static table of known targets
 *   - hx_test_servers_with(caps) — filtered subset for a test
 *   - GTKHX_TEST_SERVERS env var that scopes the matrix at runtime
 *
 * No new containers are added yet — the matrix has one entry (mhxd
 * on 127.0.0.1:5500) that matches the existing harness behaviour.
 * Phase B adds Mobius. See docs/multi-server-test-spike.md for the
 * full plan.
 *
 * Existing tests that call integration_connect() / integration_open*
 * keep working unchanged — those helpers route through the matrix
 * looking for the entry with name "mhxd" (or the first entry if
 * mhxd is filtered out), which preserves the pre-matrix
 * single-target behaviour by default.
 */

#include <glib.h>

/* ---- Capability bits -------------------------------------------- */
/*
 * Capability bits a test can require of a server target via
 * hx_test_servers_with(). These match the wire-level
 * CAPABILITY_* numbers in protocol.h where applicable, so a
 * future autodetection step that probes a real server's
 * DATA_CAPABILITIES echo can populate the matrix entry directly
 * from what the server advertised. The non-wire bits (HOPE,
 * BANNER_HTXF, ...) live in the high half so they don't collide.
 */

#define HX_TEST_CAP_LARGE_FILES   (1u << 0)  /* >4 GiB file transfers */
#define HX_TEST_CAP_TEXT_ENCODING (1u << 1)  /* UTF-8 negotiation     */
/* (1u << 2) reserved — CAPABILITY_DATE_FORMAT in protocol.h */
/* (1u << 3) reserved */
#define HX_TEST_CAP_CHAT_HISTORY  (1u << 4)  /* fogWraith spec        */

/* Non-wire test-only bits live in the high half. */
#define HX_TEST_CAP_HOPE          (1u << 16) /* HOPE handshake        */
#define HX_TEST_CAP_BANNER_HTXF   (1u << 17) /* HTXF-mode banner      */
#define HX_TEST_CAP_NEWS_15       (1u << 18) /* threaded 1.5+ news    */
#define HX_TEST_CAP_CHACHA20      (1u << 19) /* HOPE ChaCha20-Poly1305*/

/* ---- The matrix struct ------------------------------------------ */

typedef struct {
    const char *name;       /* "mhxd" | "mobius" | "vespernet" | ... */
    const char *host;       /* DNS name or IP, used by getaddrinfo  */
    guint16     port;       /* HTLS server port                     */
    guint16     xfer_port;  /* HTXF subchannel port (usually port+1)*/
    guint16     hl_version; /* HTLS_DATA_VERSION value, e.g. 185    */
    guint32     caps;       /* HX_TEST_CAP_* bitmask                */
} hx_test_server;

/* Static, immutable table of known test targets. Defined in
 * server_matrix.c. */
extern const hx_test_server hx_test_server_matrix[];
extern const gsize hx_test_server_matrix_count;

/* ---- Helpers ----------------------------------------------------- */

/*
 * Returns a GPtrArray of `const hx_test_server *` pointers into the
 * static matrix, containing every entry whose caps bitmask is a
 * SUPERSET of `required_caps`. The result is further filtered by
 * the GTKHX_TEST_SERVERS env var if set (comma-separated names —
 * "mhxd", "mhxd,mobius", etc.).
 *
 * Pass `required_caps = 0` to get every matrix entry that survives
 * the env-var filter.
 *
 * Caller g_ptr_array_unref's the returned array. The entries
 * pointed to live in static storage; do not free them.
 *
 * Returns NULL on internal failure (out-of-memory etc.) — callers
 * should treat NULL the same as an empty array and skip the test.
 */
extern GPtrArray *hx_test_servers_with (guint32 required_caps);

/*
 * Convenience: open a TCP connection to the named server. Same
 * connect-with-timeout semantics as integration_connect(): returns
 * the fd, or -1 if unreachable inside 2 seconds.
 *
 * Used by parameterised tests that have already picked their
 * server from hx_test_servers_with().
 */
extern int hx_test_server_connect (const hx_test_server *srv);

/*
 * Look up the default target for the legacy single-server harness
 * entry points (integration_connect / integration_open_or_skip).
 * Honours GTKHX_TEST_SERVERS — if the env filter excludes "mhxd",
 * the first surviving entry is returned. Returns NULL only if the
 * filter excludes every entry in the matrix; callers should treat
 * that as "skip the test."
 *
 * GTKHX_TEST_HOST / GTKHX_TEST_PORT still override the host:port
 * for the default target (back-compat with pre-matrix CI configs).
 */
extern const hx_test_server *hx_test_server_default (void);

#endif /* HX_TEST_SERVER_MATRIX_H */
