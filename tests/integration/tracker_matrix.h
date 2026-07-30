/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#ifndef HX_TEST_TRACKER_MATRIX_H
#define HX_TEST_TRACKER_MATRIX_H 1

/*
 * Multi-tracker test matrix.
 *
 * Parallel to server_matrix.{c,h} but for the Hotline tracker (HTRK)
 * protocol, which is its own thing — different ports, different wire
 * shape, different capability axes. Keeping the two matrices separate
 * is cleaner than stuffing a tracker_port + tracker_caps onto every
 * HTLS-server row.
 *
 * What lives here:
 *
 *   - hx_test_tracker struct describing one tracker family
 *   - hx_test_tracker_matrix[] static table of known targets
 *   - hx_test_trackers_with(caps) — filtered subset for a test
 *   - GTKHX_TEST_TRACKERS env var that scopes the matrix at runtime
 *     (parallel to GTKHX_TEST_SERVERS in server_matrix.c)
 *
 * Phase A (this header) adds one entry: Argus, our first real v3-
 * capable tracker target, wrapped in tests/argus/. As we add more
 * tracker families (mhxd's hxtrackd as a v1-only reference, an
 * alternative v3 if one shows up), they slot in as additional
 * matrix rows. Capability bits route tests automatically.
 *
 * Skip semantics: callers that hx_test_trackers_with() against caps
 * that no entry advertises get an empty array and SHOULD fail
 * loudly (per the project's no-silent-skips feedback —
 * feedback_no_test_skips.md). Don't g_test_skip — the test should
 * print why and return a failure so CI surfaces missing
 * coverage.
 */

#include <glib.h>

/* ---- Capability bits -------------------------------------------- */
/*
 * Tracker-side capability bits. These describe what the tracker
 * supports, not what the spec defines — e.g. a v1-only tracker
 * advertises HX_TEST_TRACKER_CAP_V1 and nothing else; Argus
 * advertises all three. Bits are independent (a tracker can speak
 * v1+v3 but not v2, for instance).
 */
#define HX_TEST_TRACKER_CAP_V1 (1u << 0) /* v1 listing (HTRK\0\1) */
#define HX_TEST_TRACKER_CAP_V2 (1u << 1) /* v2 listing (HTRK\0\2) */
#define HX_TEST_TRACKER_CAP_V3                                                 \
    (1u << 2) /* v3 listing (HTRK + 0x0003 + features) */
#define HX_TEST_TRACKER_CAP_SEARCH_TEXT                                        \
    (1u << 3) /* v3 FEAT_QUERY — accepts SEARCH_TEXT TLV */
#define HX_TEST_TRACKER_CAP_PAGINATION                                         \
    (1u << 4) /* v3 PAGE_OFFSET / PAGE_LIMIT TLVs */
#define HX_TEST_TRACKER_CAP_IPV6_RECORDS                                       \
    (1u << 5) /* emits 0x06 records when applicable */
#define HX_TEST_TRACKER_CAP_HOSTNAME_RECS                                      \
    (1u << 6) /* emits 0x48 records (Argus does for ALL promoted) */
#define HX_TEST_TRACKER_CAP_TLS                                                \
    (1u << 7) /* TLS on the listing port (Phase D) */
#define HX_TEST_TRACKER_CAP_PROMOTED                                           \
    (1u << 8) /* config-driven pinned entries */

/* ---- The matrix struct ------------------------------------------ */

typedef struct {
    const char *name; /* "argus" | "hxtrackd" | ... */
    const char *host; /* DNS name or IP, used by getaddrinfo */
    guint16 port;     /* TCP listing port (canonical 5498) */
    guint16 udp_port; /* UDP registration port (canonical 5499) */
    /* TLS-on-listing-port future-proofing — Argus doesn't support TLS
     * today (v3 spec recommends but doesn't require it). Janus's
     * separate-port TLS pattern doesn't apply here because the
     * tracker listener has no STARTTLS shape. tls_port == 0 means
     * the entry doesn't advertise TLS. */
    guint16 tls_port;
    /* Expected number of promoted_servers entries the container
     * config seeds. Lets a test assert "this listing returned at
     * least N records" without hard-coding the magic number. Zero
     * for trackers that don't advertise HX_TEST_TRACKER_CAP_PROMOTED. */
    int expected_promoted_count;
    guint32 caps; /* HX_TEST_TRACKER_CAP_* bitmask */
} hx_test_tracker;

/* Static, immutable table. Defined in tracker_matrix.c. */
extern const hx_test_tracker hx_test_tracker_matrix[];
extern const gsize hx_test_tracker_matrix_count;

/* ---- Helpers ----------------------------------------------------- */

/*
 * Returns a GPtrArray of `const hx_test_tracker *` pointers into the
 * static matrix, containing every entry whose caps bitmask is a
 * SUPERSET of `required_caps`. Further filtered by the
 * GTKHX_TEST_TRACKERS env var if set (comma-separated names —
 * "argus", "argus,hxtrackd", etc.).
 *
 * Pass `required_caps = 0` to get every matrix entry that survives
 * the env-var filter.
 *
 * Caller g_ptr_array_unref's the returned array. The entries point
 * into static storage; do not free them.
 *
 * Returns NULL on internal failure — callers should treat NULL the
 * same as an empty array and fail loudly (no silent skips).
 */
extern GPtrArray *hx_test_trackers_with (guint32 required_caps);

/*
 * Convenience: open a TCP connection to the named tracker. Same
 * connect-with-timeout semantics as hx_test_server_connect();
 * returns the fd, or -1 if unreachable inside 2 seconds.
 */
extern int hx_test_tracker_connect (const hx_test_tracker *trk);

#endif /* HX_TEST_TRACKER_MATRIX_H */
