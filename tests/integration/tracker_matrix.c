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
 * Two rows today:
 *
 *   - argus: VesperNet's closed-source v3-capable tracker
 *     (wrapped in tests/argus/). Speaks v1+v2+v3 simultaneously
 *     on the same port; advertises FEAT_QUERY + pagination;
 *     emits all promoted_servers entries as 0x48 hostname
 *     records (see tests/argus/README.md "Known gotcha"). Covers
 *     the v3 happy path.
 *
 *   - hxtrackd: mhxd's bundled pre-spec v1 tracker (wrapped in
 *     tests/hxtrackd/). v1-only — pre-spec trackers memcmp the
 *     full 6-byte HTRK_MAGIC and silently ignore the v3 version
 *     byte. Covers the probe-then-fallback path in network.c.
 *
 * Container port mapping: the rig runs every container on host
 * networking (no user-defined bridge — that perturbs the voice
 * ICE path). hxtrackd's listen ports are hardcoded compile-time
 * constants (HTRK_TCPPORT/HTRK_UDPPORT = 5498/5499) that can't be
 * moved without patching, so it keeps 5498/5499 and the
 * config-driven Argus is shifted up to 5698/5699 (+ TLS 6498) to
 * avoid the collision.
 */
const hx_test_tracker hx_test_tracker_matrix[] = {
    {
        /* Argus: the first real v3-capable Hotline tracker we
         * have access to (and the impetus for shipping the v3
         * client support). Wrapped in tests/argus/ — Dockerfile
         * pulls the binary from get.vespernet.net, COPY-overlays
         * our config.yaml, exposes the standard ports.
         *
         * Capabilities are conservative — only bits Argus
         * reliably handles get set. We know from the bundled
         * configuration.md + a live wire probe (see commit body
         * and the tests/argus/README.md "Known gotcha" section)
         * that Argus speaks v1+v2+v3, accepts FEAT_QUERY
         * (SEARCH_TEXT TLV), and emits every promoted_servers
         * entry as a 0x48 hostname record regardless of whether
         * the YAML address is an IP literal.
         *
         * IPV6_RECORDS: not asserted yet — Argus supports IPv6
         * server addresses on the registration side, but none of
         * our test promoted entries use IPv6 addresses so we
         * don't currently exercise the emit path.
         *
         * TLS: Phase D adds a stunnel sidecar inside the same
         * Argus container that terminates TLS on tcp/6498 and
         * forwards to plain Argus on 127.0.0.1:5698. Argus
         * itself has no native TLS support (we verified by
         * grepping the binary's yaml-tag strings — no tls/cert/
         * ssl keys), so the wrapper is how we exercise the v3
         * spec's "TLS on the listing port" recommendation. The
         * stunnel cert is self-signed, generated at image-build
         * time; the test relies on GTKHX_TLS_AUTO_ACCEPT=1 or
         * a pre-pin via GTKHX_KNOWN_HOSTS to dodge the TOFU
         * prompt that would otherwise block a headless run.
         *
         * Argus is shifted to 5698/5699 (TLS 6498) so it doesn't
         * collide with hxtrackd's un-moveable 5498/5499 under host
         * networking. */
        .name = "argus",
        .host = "127.0.0.1",
        .port = 5698,
        .udp_port = 5699,
        .tls_port = 6498,
        /* tests/argus/conf/config.yaml seeds three promoted
         * entries (Alpha / Beta / Gamma). Test asserts on this
         * count. If the config grows, bump this. */
        .expected_promoted_count = 3,
        .caps = HX_TEST_TRACKER_CAP_V1 | HX_TEST_TRACKER_CAP_V2
                | HX_TEST_TRACKER_CAP_V3 | HX_TEST_TRACKER_CAP_SEARCH_TEXT
                | HX_TEST_TRACKER_CAP_PAGINATION
                | HX_TEST_TRACKER_CAP_HOSTNAME_RECS
                | HX_TEST_TRACKER_CAP_PROMOTED | HX_TEST_TRACKER_CAP_TLS,
    },
    {
        /* hxtrackd: mhxd's bundled pre-spec v1 tracker, wrapped
         * in tests/hxtrackd/. Coverage for the probe-then-
         * fallback path (8-byte v3 magic → watchdog timeout →
         * fresh conn with 6-byte v1 magic). Pre-spec v1 trackers
         * memcmp the full 6-byte HTRK_MAGIC and silently ignore
         * the v3 version byte, so we need a real one in the
         * matrix to make sure that fallback can't regress.
         *
         * Keeps its native 5498/5499 (hardcoded HTRK_TCPPORT/
         * HTRK_UDPPORT); Argus moved up to 5698/5699 above so
         * nothing collides under host networking.
         *
         * Caps: V1 only. No V2 (mhxd's tracker doesn't speak v2),
         * no V3, no FEAT_QUERY, no pagination, no TLS.
         * seed-tracker.py inside the container registers one
         * server via UDP — that's not a v3 "promoted" entry,
         * just a regular registration. expected_promoted_count
         * stays 0; the v1 Tier 3 test asserts at least one
         * record arrives, not a specific count. */
        .name = "hxtrackd",
        .host = "127.0.0.1",
        .port = 5498,
        .udp_port = 5499,
        .tls_port = 0,
        .expected_promoted_count = 0,
        .caps = HX_TEST_TRACKER_CAP_V1,
    },
};

const gsize hx_test_tracker_matrix_count
    = G_N_ELEMENTS (hx_test_tracker_matrix);

/* ---- GTKHX_TEST_TRACKERS env filter ------------------------------ */
/*
 * Parsed once on first call and cached. Symmetric with
 * server_matrix.c's GTKHX_TEST_SERVERS plumbing — kept separate
 * here so a CI run can include "argus" in the tracker matrix
 * without also pulling it into the HTLS-server matrix (which it
 * isn't an entry in anyway).
 */

static gchar **env_filter_names = NULL; /* NULL-terminated, owned */
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
    env_filter_names = (gchar **)g_ptr_array_free (names, FALSE);
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
        g_ptr_array_add (result, (gpointer)t);
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
