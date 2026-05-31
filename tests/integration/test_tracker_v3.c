/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_tracker_v3.c — Tier 3 coverage for the
 * tracker v3 client path against any v3-capable tracker in the
 * matrix. Picks the first entry advertising HX_TEST_TRACKER_CAP_V3;
 * today that's Argus (wrapped in tests/argus/), but the test is
 * tracker-agnostic — drop in another v3-capable target by adding
 * a matrix row and CI step, no test changes required.
 *
 * What this test pins:
 *
 *   - The 8-byte v3 handshake is accepted by a real v3 tracker and
 *     the response confirms version=0x0003 with a non-empty feature
 *     flags reply. The Tier 2 parser tests pin the byte layout
 *     against canned fixtures; this test pins the *negotiated* flow
 *     against software that wasn't written from our spec
 *     interpretation.
 *
 *   - The 4-byte minimum listing request elicits a valid 10-byte
 *     response header — HTRK_V3_RESP_LIST type, sane total_size,
 *     record_count >= the matrix entry's expected_promoted_count.
 *
 *   - The response payload decodes record-by-record via the
 *     production hx_tracker_v3_parse_record. consumed_out advances
 *     exactly to the buffer end after walking record_count entries
 *     — pin for the "two_back_to_back" Tier 2 test in a real
 *     production setting.
 *
 *   - When the picked target is Argus specifically: one record's
 *     name matches "Promoted Alpha" (a seed entry in
 *     tests/argus/conf/config.yaml), addr_type is 0x48 (hostname),
 *     and the TLV trailer carries IS_PROMOTED. This catches a
 *     class of bugs where bytes are read correctly but the field-
 *     offset math is wrong (e.g. desc/name swap) — silent in a
 *     synthesized Tier 2 fixture because the test set the bytes
 *     itself. Other v3 trackers added to the matrix later can pin
 *     their own seed names via additional name-gated blocks if
 *     they want the same offset-math coverage.
 *
 * What this test does NOT cover:
 *
 *   - The boxed-event signal path. Argus emits ALL promoted
 *     entries as v3 0x48 (hostname) records — see
 *     tests/argus/README.md "Known gotcha". Phase E (string-keyed
 *     dedup) routes those through tracker_server_create, but the
 *     UI render layer needs a GMainLoop test harness this binary
 *     doesn't ship. Lift to the signal layer once that harness
 *     exists.
 *
 *   - Search / pagination TLVs (Phase C). Even though the picked
 *     tracker may advertise FEAT_QUERY, we don't exercise it here.
 *
 *   - TLS on the listing port. No v3 tracker in the matrix ships
 *     a TLS listener today. Phase D-shaped test for whenever one
 *     does.
 *
 * No-silent-skip contract: if the matrix has no entry advertising
 * HX_TEST_TRACKER_CAP_V3 (no v3 container is running or all v3
 * entries have been filtered out via GTKHX_TEST_TRACKERS), the test
 * calls g_test_fail_printf and returns a failure. Per the project's
 * feedback_no_test_skips memory: a Tier 3 test that can't reach its
 * target must surface in CI as a red light, not a green skip.
 */

#include "config.h"

#include <string.h>
#include <glib.h>
#include "compat.h"
#include "hotline.h"
#include "tracker_v3.h"
#include "tracker_v3_meta.h"
#include "tracker_matrix.h"
#include "integration_harness.h"

/* ---- Helpers ---------------------------------------------------- */

/* Drain `len` bytes off `fd` into `buf` via the harness's
 * integration_recv (5-second read timeout per call). g_assert on
 * partial / failed reads so a wire-level desync surfaces with the
 * test's name attached to the failure. */
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

/* ---- Test ------------------------------------------------------- */

static void
test_v3_handshake_and_listing (void)
{
    GPtrArray *targets = hx_test_trackers_with (HX_TEST_TRACKER_CAP_V3);
    if (!targets || targets->len == 0) {
        g_test_fail_printf (
            "no tracker in the matrix advertises HX_TEST_TRACKER_CAP_V3 "
            "— bring up a v3-capable test container (tests/argus/ is the "
            "current one) or check GTKHX_TEST_TRACKERS");
        if (targets) {
            g_ptr_array_unref (targets);
        }
        return;
    }

    /* Use the first matching tracker. Today only one entry advertises
     * V3 (Argus); if a second v3 tracker shows up in the matrix, lift
     * the body into a for-each so we exercise both. */
    const hx_test_tracker *trk = g_ptr_array_index (targets, 0);
    int fd = hx_test_tracker_connect (trk);
    if (fd < 0) {
        g_test_fail_printf (
            "couldn't connect to %s tracker at %s:%u — is its container "
            "up?", trk->name, trk->host, (unsigned) trk->port);
        g_ptr_array_unref (targets);
        return;
    }

    /* ---- Handshake -------------------------------------------- */
    /* Offer FEAT_IPV6 + FEAT_QUERY. A v3 tracker advertises whichever
     * features it supports; the intersection matters less than the
     * round-trip working at all. */
    guint8 hs[8];
    g_assert_true (
        hx_tracker_v3_pack_handshake (hs, sizeof (hs),
                                      HTRK_V3_FEAT_IPV6
                                          | HTRK_V3_FEAT_QUERY));
    g_assert_true (integration_send (fd, hs, sizeof (hs)));

    /* Read 6 bytes first — same shape the production state machine
     * uses. A v1 tracker would stop here; we asserted V3 in the
     * caps filter, so we expect to read 2 more. */
    guint8 resp[8];
    read_exact (fd, resp, 6, "handshake response (first 6 bytes)");

    guint16 ver = 0, feat = 0;
    g_assert_true (
        hx_tracker_v3_parse_handshake_response (resp, 6, &ver, &feat));
    g_assert_cmpuint (ver, ==, HTRK_VERSION_V3);
    g_assert_cmpuint (feat, ==, 0); /* features only in the trailing 2 */

    read_exact (fd, resp + 6, 2, "handshake feature flags");
    g_assert_true (
        hx_tracker_v3_parse_handshake_response (resp, 8, &ver, &feat));
    g_assert_cmpuint (ver, ==, HTRK_VERSION_V3);
    /* If the matrix entry claims FEAT_QUERY, the live tracker must
     * actually advertise the bit back — pin that so a tracker that
     * dropped the feature breaks loudly instead of silently failing
     * the Phase C SEARCH_TEXT path when it lands. */
    if (trk->caps & HX_TEST_TRACKER_CAP_SEARCH_TEXT) {
        g_assert_cmpuint (feat & HTRK_V3_FEAT_QUERY, ==,
                          HTRK_V3_FEAT_QUERY);
    }

    /* ---- Listing request -------------------------------------- */
    guint8 req[4];
    gsize req_len = 0;
    g_assert_true (
        hx_tracker_v3_pack_listing_request_simple (req, sizeof (req),
                                                   &req_len));
    g_assert_cmpuint (req_len, ==, 4);
    g_assert_true (integration_send (fd, req, req_len));

    /* ---- Response header -------------------------------------- */
    guint8 rhdr[HTRK_V3_RESP_HDR_LEN];
    read_exact (fd, rhdr, sizeof (rhdr), "v3 response header");

    guint16 rtype = 0, total_servers = 0, record_count = 0;
    guint32 total_size = 0;
    g_assert_true (hx_tracker_v3_parse_response_header (
        rhdr, sizeof (rhdr), &rtype, &total_size, &total_servers,
        &record_count));
    g_assert_cmpuint (rtype, ==, HTRK_V3_RESP_LIST);
    /* At least the promoted entries the matrix entry promises must
     * come back. (A v3 tracker also serves any real registrations,
     * but the test containers don't have upstream HTLS servers
     * registering against them; the count is usually exactly the
     * promoted total.) */
    g_assert_cmpuint (record_count, >=,
                      (unsigned) trk->expected_promoted_count);
    g_assert_cmpuint (total_size, >, 0u);
    g_assert_cmpuint (total_size, <, 16u * 1024u * 1024u); /* sanity cap */

    /* ---- Records payload -------------------------------------- */
    guint8 *payload = g_malloc (total_size);
    read_exact (fd, payload, total_size, "v3 records payload");

    /* Walk records. When the picked target is Argus specifically,
     * verify one record matches the known "Promoted Alpha" seed
     * from tests/argus/conf/config.yaml — that's the field-offset-
     * math regression net. For other v3 trackers, the test just
     * pins that all records decode cleanly and the cursor lands
     * exactly at total_size.
     *
     * Phase B addition: every record's TLV trailer is run through
     * the production typed decoder (hx_tracker_v3_meta_new). Two
     * regression nets:
     *   1. The decoder accepts every TLV blob the picked tracker
     *      emits — i.e. no exotic TLV id, no length-prefix encoding
     *      we missed, no oversized count tripping the bounds check.
     *      The Tier 2 tests in test_tracker_v3_meta.c pin individual
     *      shapes against synthesised bytes; this is the "actual
     *      tracker software in the wild" cross-check.
     *   2. For the Argus seed, meta->is_promoted must be TRUE.
     *      Argus tags every promoted_servers entry with the
     *      tracker-injected IS_PROMOTED (0x0600) TLV, so this is a
     *      direct pin that the typed decoder routes the byte to the
     *      expected field. */
    const gboolean expect_argus_seed = (strcmp (trk->name, "argus") == 0);
    const guint8 *cursor = payload;
    gsize remaining = total_size;
    int seen_argus_seed = 0;
    int decoded = 0;
    for (guint16 i = 0; i < record_count; i++) {
        hx_tracker_v3_record rec = { 0 };
        gsize consumed = 0;
        gboolean ok = hx_tracker_v3_parse_record (cursor, remaining, &rec,
                                                  &consumed);
        if (!ok) {
            g_test_fail_printf (
                "record %u/%u failed to parse (remaining=%zu)",
                (unsigned) (i + 1), (unsigned) record_count,
                (size_t) remaining);
            break;
        }
        decoded++;

        /* Drive the typed-meta decoder over the trailer. A NULL
         * return means the TLV blob was malformed against our
         * decoder's contract — that's a Tier 3 regression we want
         * loud, not silent: it points at either a decoder bug or a
         * tracker that started emitting bytes we can't yet handle. */
        HxTrackerV3Meta *meta = hx_tracker_v3_meta_new (
            rec.tlv_bytes, rec.tlv_bytes_len, rec.tlv_count);
        if (!meta) {
            g_test_fail_printf (
                "record %u/%u: typed-meta decoder rejected the TLV "
                "trailer (count=%u, bytes=%zu)",
                (unsigned) (i + 1), (unsigned) record_count,
                (unsigned) rec.tlv_count, (size_t) rec.tlv_bytes_len);
            break;
        }

        if (expect_argus_seed
            && rec.name_len == strlen ("Promoted Alpha")
            && memcmp (rec.name, "Promoted Alpha", rec.name_len) == 0) {
            seen_argus_seed = 1;
            /* Sanity: addr_type should be 0x48 hostname (Argus
             * emits all promoted entries that way) and the
             * IS_PROMOTED TLV should be in the trailer. */
            g_assert_cmpuint (rec.addr_type, ==, HTRK_V3_ADDR_HOSTNAME);
            g_assert_cmpuint (rec.tlv_count, >=, 1u);
            /* The typed decoder routed the tracker-injected
             * IS_PROMOTED (0x0600) TLV into the meta->is_promoted
             * field — pin that. */
            g_assert_true (meta->is_promoted);
        }

        hx_tracker_v3_meta_free (meta);
        cursor += consumed;
        remaining -= consumed;
    }

    g_assert_cmpint (decoded, ==, (int) record_count);
    /* The state-machine pattern (read total_size bytes, walk all
     * records, check cursor lands at end) — replicates Tier 2's
     * two_back_to_back test against real wire bytes. */
    g_assert_cmpuint (remaining, ==, 0u);

    if (expect_argus_seed) {
        g_assert_true (seen_argus_seed);
    }

    g_free (payload);
    integration_close (fd);
    g_ptr_array_unref (targets);
}

/* ---- main ------------------------------------------------------- */

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/tracker_v3/integration/handshake_and_listing",
                     test_v3_handshake_and_listing);

    return g_test_run ();
}
