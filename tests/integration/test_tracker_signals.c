/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_tracker_signals.c — Tier 3 coverage for the
 * tracker fetch -> view-signal boundary: the exact input contract the
 * (now Rust) Tracker window consumes.
 *
 * test_tracker_fetch.c drives the hxnet tracker-fetch FFI directly and
 * stops at the HxnetTrackerEvent layer. This test goes one layer higher —
 * the PRODUCTION path in network.c:
 *
 *     hx_tracker_list_async()
 *       -> g_timeout drain (tracker_fetch_drain)
 *       -> tracker_fetch_dispatch_event()
 *            - builds an HxTrackerServer (v1 or v3) from the raw event
 *            - gtkhx_session_emit_tracker_batch_begin / _server_create
 *
 * — against the live matrix trackers (Argus / hxtrackd). We subscribe to
 * the GtkhxSession `tracker-batch-begin` + `tracker-server-create` signals
 * (a plain GObject — no GdkDisplay, so this runs headless in CI) and
 * assert the boxed HxTrackerServer payloads the Rust window would render:
 * a non-empty printable address, a port, a name, and — for a v3 tracker —
 * a non-NULL typed meta. This pins the network.c v1/v3 HxTrackerServer
 * construction + signal emit that no other Tier 3 exercises.
 *
 * Why this and not a window-level test: the tracker window is gtk4-rs,
 * and gtk4-rs asserts main-thread init in every model/widget constructor,
 * so it can't be built without a real display — which the CI runners
 * don't have. This test covers everything up to the window's doorstep
 * headlessly; the window itself is verified manually.
 *
 * No-silent-skip contract: if the matrix has no tracker (no container up,
 * or all filtered out via GTKHX_TEST_TRACKERS) the test fails loudly. It
 * also fails if the walk emits zero begin/record signals — every matrix
 * tracker seeds promoted servers, so an empty result means something
 * broke.
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <glib.h>

#include "compat.h" /* PACKED — required before hotline.h */
#include "hotline.h"
#include "session.h"       /* session, hx_tracker_list_async */
#include "prefs.h"         /* gtkhx_prefs */
#include "tracker.h"       /* tracker_kill_threads */
#include "tracker_event.h" /* HxTrackerServer */
#include "gtkhx_session.h" /* GtkhxSession, gtkhx_session_get_default */
#include "tracker_matrix.h"

/* The stub session from connect_test_stubs.c. Production has no session
 * global — sessions are heap objects in session_registry.c — but these
 * binaries deliberately don't link it (or gtkhx.c, or the GTK surface it
 * drags in), so the stubs keep a single zeroed one. Declared here rather
 * than in a header because it exists only for this pair of files. */
extern session the_session;

typedef struct {
    int begins;
    int records;
    guint8 last_version;
    char *first_name;
    char *first_address;
    guint16 first_port;
    gboolean first_had_meta;
} Collected;

/* tracker-batch-begin: (url:string, version:u8, count:u32). */
static void
on_batch_begin (GtkhxSession *hxsession G_GNUC_UNUSED,
                const char *url G_GNUC_UNUSED, guchar version,
                guint count G_GNUC_UNUSED, gpointer user_data)
{
    Collected *c = user_data;
    c->begins++;
    c->last_version = version;
    g_assert_true (version == 1 || version == 3);
}

/* tracker-server-create: (HxTrackerServer* boxed). */
static void
on_server_create (GtkhxSession *hxsession G_GNUC_UNUSED, HxTrackerServer *event,
                  gpointer user_data)
{
    Collected *c = user_data;

    g_assert_nonnull (event);
    /* The constructor guarantees a non-NULL printable address for every
     * addr_type — the Rust window dedups + renders on it. */
    g_assert_nonnull (event->address);
    c->records++;
    if (!c->first_name) {
        c->first_name = g_strdup (event->name ? event->name : "");
        c->first_address = g_strdup (event->address);
        c->first_port = event->port;
        /* v1 records still carry a zero-init meta (never NULL). */
        c->first_had_meta = (event->meta != NULL);
    }
}

/* server_matrix.c / tracker_matrix.c reference hx_integration_connect_to
 * (via the unused-here hx_test_*_connect). This test never calls it, but
 * the symbol must resolve under -Wl,--no-undefined; we compile the matrix
 * files in directly rather than linking the full harness lib (whose
 * hlwrite_chunks / track_prog_update stubs would collide). */
int hx_integration_connect_to (const char *host, int port, int timeout_ms);
int
hx_integration_connect_to (const char *host G_GNUC_UNUSED,
                           int port G_GNUC_UNUSED, int timeout_ms G_GNUC_UNUSED)
{
    return -1;
}

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
test_tracker_signals (void)
{
    GPtrArray *targets = hx_test_trackers_with (0); /* every matrix entry */
    if (!targets) {
        g_test_fail_printf ("hx_test_trackers_with returned NULL");
        return;
    }
    if (targets->len == 0) {
        g_test_fail_printf ("no tracker in the matrix — bring up a tracker "
                            "container (tests/argus/ or tests/hxtrackd/) or "
                            "check GTKHX_TEST_TRACKERS");
        g_ptr_array_unref (targets);
        return;
    }

    /* Point gtkhx_prefs.tracker[] at the matrix trackers the way the
     * Settings page would; hx_tracker_list_async reads this list. */
    int n = (int)targets->len;
    gtkhx_prefs.tracker = g_new0 (char *, n);
    for (int i = 0; i < n; i++) {
        const hx_test_tracker *t = g_ptr_array_index (targets, i);
        gtkhx_prefs.tracker[i]
            = g_strdup_printf ("%s:%u", t->host, (unsigned)t->port);
    }
    gtkhx_prefs.num_tracker = n;

    Collected col;
    memset (&col, 0, sizeof col);

    GtkhxSession *hxsession = gtkhx_session_get_default ();
    g_assert_nonnull (hxsession);
    gulong h1 = g_signal_connect (hxsession, "tracker-batch-begin",
                                  G_CALLBACK (on_batch_begin), &col);
    gulong h2 = g_signal_connect (hxsession, "tracker-server-create",
                                  G_CALLBACK (on_server_create), &col);

    /* Kick off the production fetch (installs a 50 ms main-loop drain). */
    hx_tracker_list_async (&the_session);

    /* Spin the main loop, collecting signals. The view layer gets no
     * explicit "done", so budget off the probe timeout (like
     * test_tracker_fetch) and break early once results have landed and
     * gone quiet for ~600 ms. */
    guint32 pms = probe_ms ();
    gint64 budget_s = (gint64)n * ((pms / 1000) + 5) + 10;
    if (budget_s > 55) {
        budget_s = 55;
    }
    gint64 deadline = g_get_monotonic_time () + budget_s * G_USEC_PER_SEC;
    int quiet_ticks = 0, last_records = -1;
    while (g_get_monotonic_time () < deadline) {
        g_main_context_iteration (NULL, FALSE);
        g_usleep (10000); /* 10 ms */
        if (col.records == last_records) {
            quiet_ticks++;
            if (col.begins >= 1 && col.records >= 1 && quiet_ticks > 60) {
                break; /* ~600 ms with no new records after data arrived */
            }
        } else {
            quiet_ticks = 0;
            last_records = col.records;
        }
    }

    g_signal_handler_disconnect (hxsession, h1);
    g_signal_handler_disconnect (hxsession, h2);
    tracker_kill_threads ();

    g_message ("tracker signals: %d begin, %d record; first server = '%s' "
               "@ %s:%u (meta=%d, last version=%u)",
               col.begins, col.records,
               col.first_name ? col.first_name : "(none)",
               col.first_address ? col.first_address : "", col.first_port,
               col.first_had_meta, col.last_version);

    g_assert_cmpint (col.begins, >=, 1);  /* at least one tracker replied */
    g_assert_cmpint (col.records, >=, 1); /* at least one server listed */
    g_assert_nonnull (col.first_name);
    g_assert_nonnull (col.first_address);
    g_assert_cmpuint (strlen (col.first_address), >, 0);
    /* A v3 reply must build a typed meta on the boxed event (the Country /
     * Caps columns + details dialog read it). */
    if (col.last_version == 3) {
        g_assert_true (col.first_had_meta);
    }

    for (int i = 0; i < n; i++) {
        g_free (gtkhx_prefs.tracker[i]);
    }
    g_free (gtkhx_prefs.tracker);
    gtkhx_prefs.tracker = NULL;
    gtkhx_prefs.num_tracker = 0;
    g_free (col.first_name);
    g_free (col.first_address);
    g_ptr_array_unref (targets);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/tracker_signals/integration/emit", test_tracker_signals);
    return g_test_run ();
}
