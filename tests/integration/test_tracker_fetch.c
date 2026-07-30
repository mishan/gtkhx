/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_tracker_fetch.c — Tier 3 coverage for the
 * hxnet tracker-fetch BRIDGE end-to-end (R3 item 8, T4).
 *
 * The other tracker Tier 3 tests (test_tracker_v1 / _v3 / _v3_tls) open
 * a raw socket and pin the wire bytes — they stop short of the
 * production fetch path because that used to need a GMainLoop harness.
 * Since T2 the walk / connect / TLS / probe-fallback orchestration lives
 * in the Rust hxnet crate behind the hxnet_tracker_fetch_* FFI, so we
 * can drive the WHOLE production path — resolve + connect (+ optional
 * rustls) + v3 probe + v1/v3 listing + the repr(C) event marshalling —
 * directly through that FFI against the live matrix trackers, no
 * GMainLoop required (a plain tracker never invokes the TOFU callback,
 * so nothing marshals to a main thread).
 *
 * What this pins that the wire-level tests don't:
 *   - hxnet_tracker_fetch_open over a real URL list, the serial walk,
 *     and the connect/TLS/probe fallback ladder reaching a real reply.
 *   - the HxnetTrackerEvent ABI mirror (shared src/tracker_fetch_ffi.h)
 *     carrying real begin/record/done events across the FFI boundary —
 *     the same struct network.c's bridge consumes to emit the view
 *     signals.
 *
 * No-silent-skip contract: if the matrix has no tracker (no container
 * up, or all filtered out via GTKHX_TEST_TRACKERS) the test fails
 * loudly. It also fails if the walk returns zero records — every matrix
 * tracker seeds promoted servers, so an empty listing means something
 * broke, not "nothing to see".
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "compat.h" /* PACKED — required before hotline.h */
#include "hotline.h"
#include "tracker_fetch_ffi.h"
#include "tracker_matrix.h"

/* Accept-all TOFU verify. The matrix trackers are plain (Argus / hxtrackd
 * don't speak TLS on the listing port), so this is never actually
 * invoked; it's here so a future TLS tracker doesn't get rejected and
 * turn this test red for the wrong reason. */
static int
accept_all_verify (const guint8 *host G_GNUC_UNUSED,
                   gsize host_len G_GNUC_UNUSED, guint16 port G_GNUC_UNUSED,
                   const guint8 *fp G_GNUC_UNUSED, gsize fp_len G_GNUC_UNUSED,
                   void *user_data G_GNUC_UNUSED)
{
    return 1;
}

/* v3-probe watchdog, honouring GTKHX_TRACKER_V3_PROBE_MS for the rig
 * (a v1-only tracker silently ignores the v3 magic, so the probe waits
 * this long before falling back). 800 ms is plenty for a local
 * container. */
static guint32
probe_ms (void)
{
    const char *env = g_getenv ("GTKHX_TRACKER_V3_PROBE_MS");
    if (env && *env) {
        char *endp = NULL;
        long v = strtol (env, &endp, 10);
        if (endp != env && *endp == '\0' && v >= 100 && v <= 60000) {
            return (guint32)v;
        }
    }
    return 800;
}

static void
test_tracker_fetch_listing (void)
{
    GPtrArray *targets = hx_test_trackers_with (0); /* every matrix entry */
    if (!targets) {
        g_test_fail_printf ("hx_test_trackers_with returned NULL — internal "
                            "failure");
        return;
    }
    if (targets->len == 0) {
        g_test_fail_printf ("no tracker in the matrix — bring up a tracker "
                            "container (tests/argus/ or tests/hxtrackd/) or "
                            "check GTKHX_TEST_TRACKERS");
        g_ptr_array_unref (targets);
        return;
    }

    /* Build the "host:port" URL list the way gtkhx_prefs.tracker[] would
     * hold it; hand the whole list to one fetch so we exercise the
     * serial walk too. */
    GPtrArray *url_strs = g_ptr_array_new_with_free_func (g_free);
    for (guint i = 0; i < targets->len; i++) {
        const hx_test_tracker *t = g_ptr_array_index (targets, i);
        g_ptr_array_add (url_strs,
                         g_strdup_printf ("%s:%u", t->host, (unsigned)t->port));
    }
    const char **urls = g_new (const char *, url_strs->len);
    for (guint i = 0; i < url_strs->len; i++) {
        urls[i] = g_ptr_array_index (url_strs, i);
    }

    guint32 pms = probe_ms ();
    /* GTKHX_TEST_SOCKS, when set, tunnels the whole walk through that
     * SOCKS proxy — exercises the tracker proxy path end to end against
     * the real tracker. Unset (the default) connects direct. */
    const char *proxy_uri = g_getenv ("GTKHX_TEST_SOCKS");
    if (proxy_uri && !*proxy_uri) {
        proxy_uri = NULL;
    }
    HxnetTrackerFetch *fetch = hxnet_tracker_fetch_open (
        (const char *const *)urls, url_strs->len, HTRK_V3_FEAT_IPV6, pms,
        proxy_uri, accept_all_verify, NULL);
    g_free (urls);
    g_assert_nonnull (fetch);

    int begins = 0, records = 0, errors = 0;
    gboolean closed = FALSE, done = FALSE;
    char *first_name = NULL;
    /* Budget the wall-clock deadline off the chosen probe timeout rather
     * than a fixed 30s: each tracker can burn up to ~pms on a silent v3
     * probe plus a TLS-first attempt + listing read, and the walk is
     * serial. Cap at 55s — just under the 60s meson test timeout — so we
     * fail with a clear diagnostic instead of getting SIGTERM'd, and so a
     * large GTKHX_TRACKER_V3_PROBE_MS doesn't push us past it. */
    gint64 budget_s = (gint64)url_strs->len * ((pms / 1000) + 5) + 10;
    if (budget_s > 55) {
        budget_s = 55;
    }
    gint64 deadline = g_get_monotonic_time () + budget_s * G_USEC_PER_SEC;

    while (g_get_monotonic_time () < deadline) {
        HxnetTrackerEvent ev;
        int rc = hxnet_tracker_fetch_poll (fetch, &ev);
        if (rc == HXNET_TRK_POLL_CLOSED) {
            closed = TRUE;
            break;
        }
        if (rc == HXNET_TRK_POLL_EMPTY) {
            g_usleep (5000); /* 5 ms — events trickle in as the walk runs */
            continue;
        }
        /* HXNET_TRK_POLL_EVENT */
        switch (ev.kind) {
        case HXNET_TRK_KIND_BEGIN:
            begins++;
            g_assert_true (ev.version == 1 || ev.version == 3);
            break;
        case HXNET_TRK_KIND_RECORD:
            records++;
            if (!first_name && ev.name_len) {
                first_name = g_strndup ((const char *)ev.name_ptr, ev.name_len);
            }
            break;
        case HXNET_TRK_KIND_ERROR:
            errors++;
            break;
        case HXNET_TRK_KIND_DONE:
            done = TRUE;
            break;
        default:
            g_test_fail_printf ("unexpected event kind %u", ev.kind);
            break;
        }
    }

    hxnet_tracker_fetch_close (fetch);

    g_message ("tracker fetch: %u tracker(s), %d begin, %d record, %d error; "
               "first server = '%s'",
               url_strs->len, begins, records, errors,
               first_name ? first_name : "(none)");

    g_assert_true (closed);           /* walk finished + drained in time */
    g_assert_true (done);             /* explicit Done event arrived */
    g_assert_cmpint (begins, >=, 1);  /* at least one tracker replied */
    g_assert_cmpint (records, >=, 1); /* at least one server listed */
    g_assert_nonnull (first_name);

    g_free (first_name);
    g_ptr_array_unref (url_strs);
    g_ptr_array_unref (targets);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/tracker_fetch/integration/listing",
                     test_tracker_fetch_listing);
    return g_test_run ();
}
