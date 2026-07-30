/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_tracker_v1.c — Tier 3 coverage for the v1
 * fallback wire path against any v1-only tracker in the matrix.
 * Picks the first entry that has HX_TEST_TRACKER_CAP_V1 but NOT
 * HX_TEST_TRACKER_CAP_V3 (the v3-aware row has V1 too but pointing
 * the test at it would exercise the v3 happy path, not the
 * fallback). Today that's hxtrackd (wrapped in tests/hxtrackd/),
 * but the test is tracker-agnostic — drop in another v1-only
 * target by adding a matrix row and CI step, no test changes
 * required.
 *
 * What this test pins:
 *
 *   - The 6-byte v1 magic (HTRK\\0\\1) sent to a pre-spec v1
 *     tracker elicits a clean response: 6-byte magic echo, then
 *     the 14-byte v1 response header (with nservers at offset
 *     [10..11]), then the v1 records back-to-back.
 *   - The pure parsers in src/tracker_parser.c decode every
 *     piece (reply header, padding sentinel, fixed record
 *     prefix) against real wire bytes from a real tracker — not
 *     just the canned fixtures in tests/proto/test_tracker_parser.c.
 *   - When the picked target is hxtrackd specifically: one
 *     record matches the seeded "hxtrackd test server" entry
 *     that tests/hxtrackd/seed-tracker.py registers via UDP at
 *     container startup, with port=5500 and nusers=4 from the
 *     seed packet. This is the offset-math regression net.
 *     Other v1-only trackers added to the matrix later can pin
 *     their own seed names via additional name-gated blocks if
 *     they want the same coverage.
 *
 * The probe-then-fallback state machine in network.c (8-byte v3
 * probe → 2-second watchdog → fresh conn with v1 magic) is the
 * code path this test exists to regression-net. We don't exercise
 * the watchdog inline here — the production state machine is
 * main-loop-driven and would need a GMainLoop test harness this
 * binary doesn't ship. Instead, the test's existence + the matrix
 * entry's V1-only caps mean a future change that breaks the v1
 * wire path against a v1-only tracker surfaces in CI, which gives
 * us most of the protection we want.
 *
 * What this test does NOT cover:
 *
 *   - The GtkhxSession::tracker-server-create signal path. The UI
 *     render path would need a GMainLoop test harness this binary
 *     doesn't ship.
 *
 *   - The actual production state machine's watchdog timing. The
 *     `tracker_v3_listing` test exercises the v3 happy path, which
 *     forces the same callbacks down a different branch; together
 *     the two tests give end-to-end wire-format coverage.
 *
 * No-silent-skip contract: if the matrix has no V1-only tracker
 * entry (no v1-only container is running or all such entries have
 * been filtered out via GTKHX_TEST_TRACKERS), the test fails
 * loudly with g_test_fail_printf.
 */

#include "config.h"

#include <string.h>
#include <glib.h>
#include "compat.h"
#include "hotline.h"
#include "tracker_parser.h"
#include "tracker_matrix.h"
#include "integration_harness.h"

/* Read exactly `len` bytes via the harness's integration_recv (5s
 * per-call timeout). g_assert on partial / failed reads so a wire-
 * level desync surfaces with the test name attached. */
static void
read_exact (int fd, void *buf, gsize len, const char *what)
{
    gboolean ok = integration_recv (fd, buf, len);
    if (!ok) {
        g_test_fail_printf ("integration_recv (%s, %zu bytes) failed", what,
                            (size_t)len);
    }
    g_assert_true (ok);
}

/* Pick a tracker that has V1 but NOT V3 — the v3-aware matrix entry
 * also has V1 set, but pointing this test at it would exercise the
 * v3 happy path, not the fallback. Subset semantics so a future
 * v1-only tracker that grows an unrelated metadata bit
 * (HOSTNAME_RECS, PROMOTED, ...) still gets picked. */
static const hx_test_tracker *
pick_v1_only_tracker (void)
{
    GPtrArray *targets = hx_test_trackers_with (HX_TEST_TRACKER_CAP_V1);
    if (!targets) {
        g_test_fail_printf (
            "hx_test_trackers_with returned NULL — internal failure");
        return NULL;
    }
    const hx_test_tracker *picked = NULL;
    for (guint i = 0; i < targets->len; i++) {
        const hx_test_tracker *t = g_ptr_array_index (targets, i);
        if ((t->caps & HX_TEST_TRACKER_CAP_V1)
            && !(t->caps & HX_TEST_TRACKER_CAP_V3)) {
            picked = t;
            break;
        }
    }
    g_ptr_array_unref (targets);
    if (!picked) {
        g_test_fail_printf (
            "no v1-only tracker in the matrix — bring up a v1-only test "
            "container (tests/hxtrackd/ is the current one) or check "
            "GTKHX_TEST_TRACKERS");
    }
    return picked;
}

static void
test_v1_fallback_listing (void)
{
    const hx_test_tracker *trk = pick_v1_only_tracker ();
    if (!trk) {
        return; /* g_test_fail_printf already called */
    }

    int fd = hx_test_tracker_connect (trk);
    if (fd < 0) {
        g_test_fail_printf (
            "couldn't connect to %s tracker at %s:%u — is its container "
            "up?",
            trk->name, trk->host, (unsigned)trk->port);
        return;
    }

    /* Send 6-byte v1 magic (HTRK\\0\\1). hxtrackd embeds the magic
     * as the FIRST 6 BYTES of the 14-byte v1 reply header (not as
     * a separate echo frame before it) — that's what its
     * tracker.c::htrk_send_list does: qbuf_add(HTRK_MAGIC, 6) +
     * qbuf_add(chunk_header_and_records, ...). The production
     * state machine in network.c reads 6 bytes first (to peek
     * the version) and then 8 more (to complete the v1 reply
     * header); same TCP framing the test pins here, just in one
     * read for simplicity. */
    g_assert_true (integration_send (fd, HTRK_MAGIC, HTRK_MAGIC_LEN));

    guint8 reply_hdr[14];
    read_exact (fd, reply_hdr, sizeof (reply_hdr), "v1 reply header");

    /* First 6 bytes echo HTRK_MAGIC. Pin that so a future v1
     * tracker that started serving raw chunk headers (without
     * the magic prefix) breaks the test loudly instead of
     * misaligning record reads downstream. */
    g_assert_cmpmem (reply_hdr, HTRK_MAGIC_LEN, HTRK_MAGIC, HTRK_MAGIC_LEN);

    /* nservers lives at offset [10..11] (u16 BE) per the pure
     * helper in src/tracker_parser.c. */
    guint16 nservers = 0xffff;
    g_assert_true (hx_tracker_reply_parse_header (reply_hdr, sizeof (reply_hdr),
                                                  &nservers));
    /* Demand >= 1 record. A v1-only test container is expected to
     * have at least one server (either a seeded registration or a
     * real one) so the test exercises record parsing as well as
     * the header. */
    g_assert_cmpuint (nservers, >=, 1);

    /* Drain the records. For each, peel:
     *   - 8 bytes addr(4)+port(2)+nusers(2)
     *   - 3 bytes reserved(2)+name_len(1)
     *   - name_len bytes name
     *   - 1 byte desc_len
     *   - desc_len bytes desc
     *
     * When the picked target is hxtrackd, look for the "hxtrackd
     * test server" entry seeded by tests/hxtrackd/seed-tracker.py
     * — that's the field-offset regression net. For other v1
     * trackers, just pin that records decode cleanly. Skip
     * padding entries (first byte = 0 sentinel). */
    const gboolean expect_hxtrackd_seed = (strcmp (trk->name, "hxtrackd") == 0);
    int decoded = 0;
    int seen_hxtrackd_seed = 0;
    for (guint16 i = 0; i < nservers; i++) {
        guint8 hdr8[8];
        read_exact (fd, hdr8, sizeof (hdr8), "v1 record hdr");
        if (hx_tracker_record_is_padding (hdr8, 1)) {
            continue; /* padding sentinel — first byte zero */
        }
        guint8 rest3[3];
        read_exact (fd, rest3, sizeof (rest3), "v1 record reserved+nlen");

        guint8 packed[11];
        memcpy (packed, hdr8, 8);
        memcpy (packed + 8, rest3, 3);

        hx_tracker_record_fixed rec = { 0 };
        g_assert_true (
            hx_tracker_record_parse_fixed (packed, sizeof (packed), &rec));

        char name[256] = { 0 };
        if (rec.name_len > 0) {
            read_exact (fd, name, rec.name_len, "v1 record name");
        }

        guint8 dlen_byte = 0;
        read_exact (fd, &dlen_byte, 1, "v1 record desc_len");
        char desc[256] = { 0 };
        if (dlen_byte > 0) {
            read_exact (fd, desc, dlen_byte, "v1 record desc");
        }

        decoded++;
        if (expect_hxtrackd_seed
            && rec.name_len == strlen ("hxtrackd test server")
            && memcmp (name, "hxtrackd test server", rec.name_len) == 0) {
            seen_hxtrackd_seed = 1;
            /* Sanity: the seed-tracker.py packet stamps port=5500
             * and nusers=4. Pin those so an accidental script
             * tweak surfaces here. */
            g_assert_cmpuint (rec.port, ==, 5500);
            g_assert_cmpuint (rec.nusers, ==, 4);
        }
    }

    g_assert_cmpint (decoded, >=, 1);
    if (expect_hxtrackd_seed) {
        g_assert_true (seen_hxtrackd_seed);
    }

    integration_close (fd);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/tracker_v1/integration/v1_listing",
                     test_v1_fallback_listing);

    return g_test_run ();
}
