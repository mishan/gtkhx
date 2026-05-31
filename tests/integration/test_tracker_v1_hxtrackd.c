/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_tracker_v1_hxtrackd.c — Tier 3 coverage
 * for the v1 fallback wire path against a real pre-spec v1
 * tracker (mhxd's hxtrackd, wrapped by tests/hxtrackd/).
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
 *   - At least one record matches the seeded
 *     "hxtrackd test server" entry that tests/hxtrackd/
 *     seed-tracker.py registers via UDP at container startup.
 *
 * The probe-then-fallback state machine in network.c (8-byte v3
 * probe → 2-second watchdog → fresh conn with v1 magic) is the
 * code path this container exists to regression-net. We don't
 * exercise the watchdog inline here — the production state
 * machine is main-loop-driven and would need a GMainLoop test
 * harness this binary doesn't ship. Instead, the test's
 * existence + the matrix entry's HX_TEST_TRACKER_CAP_V1-only
 * caps mean a future change that breaks the v1 wire path against
 * hxtrackd surfaces in CI, which gives us most of the protection
 * we want.
 *
 * What this test does NOT cover:
 *
 *   - The GtkhxSession::tracker-server-create signal path.
 *     Sandbox doesn't have GTK available.
 *
 *   - The actual production state machine's watchdog timing. The
 *     `tracker_v3_argus` test exercises the v3 happy path, which
 *     forces the same callbacks down a different branch; together
 *     the two tests give end-to-end wire-format coverage.
 *
 * No-silent-skip contract: if the matrix has no V1-only tracker
 * entry (hxtrackd container down or filtered out via
 * GTKHX_TEST_TRACKERS), the test fails loudly with
 * g_test_fail_printf.
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
        g_test_fail_printf ("integration_recv (%s, %zu bytes) failed",
                            what, (size_t) len);
    }
    g_assert_true (ok);
}

/* Pick the hxtrackd entry — the matrix's only v1-only tracker. If
 * it's not there (container down / env filter), fail loudly. */
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
        /* Prefer a tracker that has V1 but NOT V3 — the v3-aware
         * matrix entry (Argus) also has V1 set, but pointing this
         * test at Argus would exercise the v3 happy path, not the
         * fallback. Subset semantics (not strict equality) so a
         * future hxtrackd that grows an unrelated metadata bit
         * (HOSTNAME_RECS, PROMOTED, ...) still gets picked. */
        if ((t->caps & HX_TEST_TRACKER_CAP_V1)
            && !(t->caps & HX_TEST_TRACKER_CAP_V3)) {
            picked = t;
            break;
        }
    }
    g_ptr_array_unref (targets);
    if (!picked) {
        g_test_fail_printf (
            "no v1-only tracker in the matrix — bring up tests/hxtrackd "
            "container or check GTKHX_TEST_TRACKERS");
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
            "couldn't connect to %s tracker at %s:%u — is the container "
            "up? (`docker run --rm -p 5598:5498 -p 5599:5499/udp "
            "gtkhx-hxtrackd`)",
            trk->name, trk->host, (unsigned) trk->port);
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

    /* First 6 bytes echo HTRK_MAGIC. Pin that so a future
     * hxtrackd that started serving raw chunk headers (without
     * the magic prefix) breaks the test loudly instead of
     * misaligning record reads downstream. */
    g_assert_cmpmem (reply_hdr, HTRK_MAGIC_LEN, HTRK_MAGIC, HTRK_MAGIC_LEN);

    /* nservers lives at offset [10..11] (u16 BE) per the pure
     * helper in src/tracker_parser.c. */
    guint16 nservers = 0xffff;
    g_assert_true (
        hx_tracker_reply_parse_header (reply_hdr, sizeof (reply_hdr),
                                       &nservers));
    /* The hxtrackd container's seed-tracker.py registers exactly
     * one server (and one in the listing means one in the count —
     * v1's u16 nservers can include padding entries but hxtrackd's
     * encoder for our case won't emit any). Demand >= 1 so a
     * future seed-script change can grow the count without
     * breaking the test. */
    g_assert_cmpuint (nservers, >=, 1);

    /* Drain the seeded record(s). For each, peel:
     *   - 8 bytes addr(4)+port(2)+nusers(2)
     *   - 3 bytes reserved(2)+name_len(1)
     *   - name_len bytes name
     *   - 1 byte desc_len
     *   - desc_len bytes desc
     *
     * Look for the "hxtrackd test server" entry seeded by
     * tests/hxtrackd/seed-tracker.py. Skip padding entries
     * (first byte = 0 sentinel). */
    int decoded = 0;
    int seen_seed = 0;
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

        hx_tracker_record_fixed rec = { { 0 }, 0, 0, 0 };
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
        if (rec.name_len == strlen ("hxtrackd test server")
            && memcmp (name, "hxtrackd test server", rec.name_len) == 0) {
            seen_seed = 1;
            /* Sanity: the seed-tracker.py packet stamps port=5500
             * and nusers=4. Pin those so an accidental script
             * tweak surfaces here. */
            g_assert_cmpuint (rec.port, ==, 5500);
            g_assert_cmpuint (rec.nusers, ==, 4);
        }
    }

    g_assert_cmpint (decoded, >=, 1);
    g_assert_true (seen_seed);

    integration_close (fd);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/tracker_v1/hxtrackd/probe_then_v1_listing",
                     test_v1_fallback_listing);

    return g_test_run ();
}
