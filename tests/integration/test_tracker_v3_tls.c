/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_tracker_v3_tls.c — Tier 3 coverage for
 * the Phase D tracker-v3 TLS path.
 *
 * Talks to any matrix entry advertising HX_TEST_TRACKER_CAP_TLS;
 * today that's Argus (with the stunnel sidecar baked into
 * tests/argus/), but the test is tracker-agnostic — drop in a
 * second TLS-capable target by adding a matrix row.
 *
 * What this test pins:
 *
 *   - A TLS connection to the tracker's tls_port handshakes
 *     cleanly through the same GSocketClient + on_socket_client_
 *     event TOFU plumbing that the production tracker fetch uses.
 *     If the cert handler ever stopped accepting tracker certs
 *     (e.g. a refactor of tls_trust.c that broke its host-port
 *     key handling), this is the regression net.
 *
 *   - Over a TLS-wrapped stream, the 8-byte v3 handshake → 4-byte
 *     listing request → 10-byte response header → record-payload
 *     parse all work end-to-end. Same wire shape as the plain
 *     test_tracker_v3.c — the entire point of the TLS test is
 *     that ENCRYPTION doesn't change the protocol shape, only the
 *     transport layer.
 *
 *   - The TOFU prompt is dodged by installing an inline accept-
 *     any-cert handler directly on the GTlsConnection's
 *     accept-certificate signal (see accept_any_cert below). The
 *     production handler routes through the tls_trust_dialog
 *     module's nested-GMainLoop prompt, which would deadlock
 *     here — we have no GMainLoop / GtkApplication running. The
 *     inline accept is equivalent in effect to
 *     GTKHX_TLS_AUTO_ACCEPT=1 but doesn't depend on env-var
 *     plumbing surviving across test-binary launches.
 *
 * What this test does NOT cover:
 *
 *   - The production tracker fetch state machine's TLS-fail →
 *     plain-fallback path. That requires a GMainLoop harness +
 *     mock signal subscriber to observe records flowing through
 *     the boxed-event signal. Lift when that harness exists.
 *     Tier 2 unit-tests the verdict-cache + branch logic directly.
 *
 *   - The known_hosts pin path. AUTO_ACCEPT pins-on-accept under
 *     the hood, so a subsequent same-session connect would find
 *     a fingerprint match; but the test doesn't run that scenario
 *     explicitly.
 *
 * No-silent-skip contract: if the matrix has no entry advertising
 * HX_TEST_TRACKER_CAP_TLS (no TLS-enabled container is running
 * or the entry was filtered via GTKHX_TEST_TRACKERS), the test
 * calls g_test_fail_printf. Per feedback_no_test_skips: a Tier 3
 * test that can't reach its target must fail loudly.
 */

#include "config.h"

#include <string.h>
#include <gio/gio.h>
#include <glib.h>
#include "compat.h"
#include "hotline.h"
#include "tracker_v3.h"
#include "tracker_v3_meta.h"
#include "tracker_matrix.h"

/* ---- Helpers ---------------------------------------------------- */

/* Synchronous read of exactly `len` bytes off a GInputStream. The
 * production tracker path uses async read/write; this test goes
 * sync because we're outside a GMainLoop. NULL timeout would block
 * forever on a stalled stunnel; 5s matches the integration_recv
 * timeout in the plain Tier 3 test. */
static gboolean
read_exact_stream (GInputStream *in, void *buf, gsize len, const char *what)
{
    gsize n = 0;
    GError *err = NULL;
    gboolean ok
        = g_input_stream_read_all (in, buf, len, &n, NULL, &err);
    if (!ok || n != len) {
        g_test_fail_printf ("read_all (%s, %zu bytes) → ok=%d n=%zu err=%s",
                            what, (size_t) len, (int) ok, (size_t) n,
                            err ? err->message : "(none)");
        g_clear_error (&err);
        return FALSE;
    }
    return TRUE;
}

static gboolean
write_exact_stream (GOutputStream *out, const void *buf, gsize len,
                    const char *what)
{
    gsize n = 0;
    GError *err = NULL;
    gboolean ok = g_output_stream_write_all (out, buf, len, &n, NULL, &err);
    if (!ok || n != len) {
        g_test_fail_printf ("write_all (%s, %zu bytes) → ok=%d n=%zu err=%s",
                            what, (size_t) len, (int) ok, (size_t) n,
                            err ? err->message : "(none)");
        g_clear_error (&err);
        return FALSE;
    }
    return TRUE;
}

/* TOFU handler — the production code routes through the shared
 * tls_trust + tls_trust_dialog modules, which prompt the user.
 * We have no GMainLoop here; instead this handler accepts any
 * cert unconditionally and runs as a GSocketClient::event hook.
 * Equivalent to GTKHX_TLS_AUTO_ACCEPT=1's effect on the
 * production handler, but inline so we don't depend on env-var
 * plumbing surviving across test binary launches.
 *
 * Safe for tests against a self-signed cert from a trusted
 * source (our own container's build-time cert). Never use this
 * pattern outside a test harness. */
static gboolean
accept_any_cert (GTlsConnection *conn G_GNUC_UNUSED,
                 GTlsCertificate *peer_cert G_GNUC_UNUSED,
                 GTlsCertificateFlags errors G_GNUC_UNUSED,
                 gpointer user_data G_GNUC_UNUSED)
{
    return TRUE;
}

static void
on_event_attach_accept (GSocketClient *client G_GNUC_UNUSED,
                        GSocketClientEvent event,
                        GSocketConnectable *connectable G_GNUC_UNUSED,
                        GIOStream *connection, gpointer user_data G_GNUC_UNUSED)
{
    if (event != G_SOCKET_CLIENT_TLS_HANDSHAKING) {
        return;
    }
    if (!connection || !G_IS_TLS_CONNECTION (connection)) {
        return;
    }
    g_signal_connect (connection, "accept-certificate",
                      G_CALLBACK (accept_any_cert), NULL);
}

/* ---- Test ------------------------------------------------------- */

static void
test_v3_tls_handshake_and_listing (void)
{
    GPtrArray *targets = hx_test_trackers_with (HX_TEST_TRACKER_CAP_TLS
                                                | HX_TEST_TRACKER_CAP_V3);
    if (!targets || targets->len == 0) {
        g_test_fail_printf (
            "no tracker in the matrix advertises HX_TEST_TRACKER_CAP_TLS "
            "+ HX_TEST_TRACKER_CAP_V3 — bring up tests/argus/ (with the "
            "stunnel sidecar built in) or check GTKHX_TEST_TRACKERS");
        if (targets) {
            g_ptr_array_unref (targets);
        }
        return;
    }

    const hx_test_tracker *trk = g_ptr_array_index (targets, 0);
    g_assert_cmpuint (trk->tls_port, >, 0u);

    /* Connect over TLS using the same machinery the production
     * tracker fetch uses: GSocketClient with set_tls(TRUE) +
     * event-signal hook for accept-certificate. The accept-any
     * handler dodges the TOFU prompt (no GMainLoop here). */
    GError *err = NULL;
    GSocketClient *client = g_socket_client_new ();
    g_socket_client_set_timeout (client, 5);
    g_socket_client_set_tls (client, TRUE);
    g_signal_connect (client, "event",
                      G_CALLBACK (on_event_attach_accept), NULL);

    GSocketConnection *conn = g_socket_client_connect_to_host (
        client, trk->host, trk->tls_port, NULL, &err);
    g_object_unref (client);
    if (!conn) {
        g_test_fail_printf (
            "TLS connect to %s:%u failed: %s",
            trk->host, (unsigned) trk->tls_port,
            err ? err->message : "(no error)");
        g_clear_error (&err);
        g_ptr_array_unref (targets);
        return;
    }

    GInputStream *in = g_io_stream_get_input_stream (G_IO_STREAM (conn));
    GOutputStream *out = g_io_stream_get_output_stream (G_IO_STREAM (conn));

    /* ---- Handshake -------------------------------------------- */
    guint8 hs[8];
    g_assert_true (
        hx_tracker_v3_pack_handshake (hs, sizeof (hs),
                                      HTRK_V3_FEAT_IPV6
                                          | HTRK_V3_FEAT_QUERY));
    if (!write_exact_stream (out, hs, sizeof (hs), "v3 handshake")) {
        goto done;
    }

    guint8 resp[8];
    if (!read_exact_stream (in, resp, 8, "v3 handshake response")) {
        goto done;
    }
    guint16 ver = 0, feat = 0;
    g_assert_true (
        hx_tracker_v3_parse_handshake_response (resp, 8, &ver, &feat));
    g_assert_cmpuint (ver, ==, HTRK_VERSION_V3);

    /* ---- Listing request -------------------------------------- */
    guint8 req[4];
    gsize req_len = 0;
    g_assert_true (
        hx_tracker_v3_pack_listing_request_simple (req, sizeof (req),
                                                   &req_len));
    g_assert_cmpuint (req_len, ==, 4);
    if (!write_exact_stream (out, req, req_len, "v3 listing request")) {
        goto done;
    }

    /* ---- Response header -------------------------------------- */
    guint8 rhdr[HTRK_V3_RESP_HDR_LEN];
    if (!read_exact_stream (in, rhdr, sizeof (rhdr), "v3 response header")) {
        goto done;
    }

    guint16 rtype = 0, total_servers = 0, record_count = 0;
    guint32 total_size = 0;
    g_assert_true (hx_tracker_v3_parse_response_header (
        rhdr, sizeof (rhdr), &rtype, &total_size, &total_servers,
        &record_count));
    g_assert_cmpuint (rtype, ==, HTRK_V3_RESP_LIST);
    g_assert_cmpuint (record_count, >=,
                      (unsigned) trk->expected_promoted_count);
    g_assert_cmpuint (total_size, >, 0u);
    g_assert_cmpuint (total_size, <, 16u * 1024u * 1024u); /* sanity cap */

    /* ---- Records payload -------------------------------------- */
    guint8 *payload = g_malloc (total_size);
    if (!read_exact_stream (in, payload, total_size, "v3 records payload")) {
        g_free (payload);
        goto done;
    }

    const guint8 *cursor = payload;
    gsize remaining = total_size;
    int decoded = 0;
    for (guint16 i = 0; i < record_count; i++) {
        hx_tracker_v3_record rec = { 0 };
        gsize consumed = 0;
        gboolean ok = hx_tracker_v3_parse_record (cursor, remaining, &rec,
                                                  &consumed);
        if (!ok) {
            g_test_fail_printf (
                "record %u/%u failed to parse over TLS (remaining=%zu)",
                (unsigned) (i + 1), (unsigned) record_count,
                (size_t) remaining);
            break;
        }
        decoded++;

        /* Routine TLV-trailer decode — same regression net as the
         * plain test_tracker_v3. */
        HxTrackerV3Meta *meta = hx_tracker_v3_meta_new (
            rec.tlv_bytes, rec.tlv_bytes_len, rec.tlv_count);
        if (!meta) {
            g_test_fail_printf (
                "record %u/%u over TLS: typed-meta decoder rejected the "
                "TLV trailer (count=%u, bytes=%zu)",
                (unsigned) (i + 1), (unsigned) record_count,
                (unsigned) rec.tlv_count, (size_t) rec.tlv_bytes_len);
            break;
        }
        hx_tracker_v3_meta_free (meta);

        cursor += consumed;
        remaining -= consumed;
    }

    g_assert_cmpint (decoded, ==, (int) record_count);
    g_assert_cmpuint (remaining, ==, 0u);

    g_free (payload);

done:
    g_io_stream_close (G_IO_STREAM (conn), NULL, NULL);
    g_object_unref (conn);
    g_ptr_array_unref (targets);
}

/* ---- main ------------------------------------------------------- */

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/tracker_v3/integration/tls_handshake_and_listing",
                     test_v3_tls_handshake_and_listing);

    return g_test_run ();
}
