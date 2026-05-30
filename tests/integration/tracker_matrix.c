/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "integration_harness.h" /* hx_integration_connect_to prototype */
#include "tracker_matrix.h"

/* ---- The matrix --------------------------------------------------- */
/*
 * Phase A on this branch: only the v1-only hxtrackd entry — that's
 * what's needed to give the probe-then-fallback code path in
 * network.c its own Tier 3 coverage. A v3-capable entry (Argus)
 * lands on a follow-up branch that wraps VesperNet's closed-source
 * tracker; the matrix grows by one row there, no other changes.
 */
const hx_test_tracker hx_test_tracker_matrix[] = {
    {
        /* hxtrackd: mhxd's bundled pre-spec v1 tracker, wrapped in
         * tests/hxtrackd/. Coverage for the probe-then-fallback
         * path (8-byte v3 magic → watchdog timeout → fresh conn
         * with 6-byte v1 magic). Pre-spec v1 trackers memcmp the
         * full 6-byte HTRK_MAGIC and silently ignore the v3
         * version byte, so we need a real one in the matrix to
         * make sure that fallback can't regress.
         *
         * Container exposes its internal 5498/5499 on host
         * 5598/5599 (the conventional 5498/5499 are reserved for
         * future v3-capable matrix rows that benefit from running
         * on the spec port).
         *
         * Caps: V1 only. No V2 (mhxd's tracker doesn't speak v2),
         * no V3, no FEAT_QUERY, no pagination, no TLS.
         * seed-tracker.py inside the container registers one
         * server via UDP — that's not a v3 "promoted" entry, just
         * a regular registration. expected_promoted_count stays 0;
         * the v1 Tier 3 test asserts at least one record arrives,
         * not a specific count. */
        .name                    = "hxtrackd",
        .host                    = "127.0.0.1",
        .port                    = 5598,
        .udp_port                = 5599,
        .tls_port                = 0,
        .expected_promoted_count = 0,
        .caps                    = HX_TEST_TRACKER_CAP_V1,
    },
};

const gsize hx_test_tracker_matrix_count =
    G_N_ELEMENTS (hx_test_tracker_matrix);

/* ---- GTKHX_TEST_TRACKERS env filter ------------------------------ */
/*
 * Parsed once on first call and cached. Symmetric with
 * server_matrix.c's GTKHX_TEST_SERVERS plumbing — kept separate
 * here so a CI run can include "argus" in the tracker matrix
 * without also pulling it into the HTLS-server matrix (which it
 * isn't an entry in anyway).
 */

static gchar **env_filter_names = NULL;       /* NULL-terminated, owned */
static gboolean env_filter_loaded = FALSE;

static void
load_env_filter (void)
{
    if (env_filter_loaded) {
        return;
    }
    env_filter_loaded = TRUE;

    const char *v = g_getenv ("GTKHX_TEST_TRACKERS");
    if (!v || !*v) {
        return;
    }

    gchar **parts = g_strsplit (v, ",", -1);
    GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
    for (gchar **p = parts; p && *p; p++) {
        gchar *name = g_strstrip (g_strdup (*p));
        if (*name) {
            g_ptr_array_add (names, name);
        } else {
            g_free (name);
        }
    }
    g_strfreev (parts);

    g_ptr_array_add (names, NULL);
    env_filter_names = (gchar **) g_ptr_array_free (names, FALSE);
}

static gboolean
name_passes_env_filter (const char *name)
{
    load_env_filter ();
    if (!env_filter_names) {
        return TRUE;
    }
    for (gchar **p = env_filter_names; *p; p++) {
        if (g_ascii_strcasecmp (*p, name) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

/* ---- Public helpers --------------------------------------------- */

GPtrArray *
hx_test_trackers_with (guint32 required_caps)
{
    GPtrArray *result = g_ptr_array_new ();
    if (!result) {
        return NULL;
    }
    for (gsize i = 0; i < hx_test_tracker_matrix_count; i++) {
        const hx_test_tracker *t = &hx_test_tracker_matrix[i];
        if ((t->caps & required_caps) != required_caps) {
            continue;
        }
        if (!name_passes_env_filter (t->name)) {
            continue;
        }
        g_ptr_array_add (result, (gpointer) t);
    }
    return result;
}

int
hx_test_tracker_connect (const hx_test_tracker *trk)
{
    g_return_val_if_fail (trk != NULL, -1);
    return hx_integration_connect_to (trk->host, trk->port,
                                      /*timeout_ms=*/2000);
}
