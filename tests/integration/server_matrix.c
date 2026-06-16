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
#include "server_matrix.h"

/* ---- The matrix --------------------------------------------------- */
/*
 * Phase A added mhxd as a row in the table rather than hardcoded
 * throughout the harness. Phase C (this revision) adds Janus, the
 * closed-source VesperNet server we use as the chat-history test
 * target. Phase B (Mobius) is still pending and can be slotted in
 * between when it lands.
 *
 * Caps are conservative — only bits the server reliably handles for
 * tests we already run get set. Adding a bit later (e.g. when we
 * audit Janus's HOPE flow and turn it on in the container) is a
 * one-line change.
 *
 * Container ports: mhxd answers on the conventional 5500/5501; Janus
 * is mapped to 5510/5511 in the test Compose setup so both can run
 * side-by-side. The matrix records the *host-side* port the
 * container is published on.
 */
const hx_test_server hx_test_server_matrix[] = {
    {
        /* mhxd: the controlled-codebase reference server. Built
         * from the vendored mhxd/ source in tests/mhxd/. Hotline
         * 1.8.5-style flow, HOPE, threaded news. No chat-history.
         *
         * BANNER_HTXF intentionally NOT asserted here even though
         * mhxd can serve an HTXF banner when started with
         * BANNER_MODE=JPEG (see tests/mhxd/docker-entrypoint.sh).
         * The default container starts in URL mode and an HTXF-
         * picker matching mhxd in that state would correctly
         * time out — Janus is the canonical HTXF-banner server
         * in the matrix (always serves HTXF, no env knob). The
         * basic banner test (test_banner.c) opens the default
         * server irrespective of caps and asserts on whichever
         * mode the container is in, so URL-mode mhxd still
         * exercises that subtest. The HOPE+stream banner test
         * (test_hope_blowfish_banner) needs a server that actually
         * serves HTXF banner under HOPE+stream — only Janus
         * today. */
        .name          = "mhxd",
        .host          = "127.0.0.1",
        .port          = 5500,
        .xfer_port     = 5501,
        /* mhxd doesn't ship a TLS listener (no built-in cert/key
         * config). tls_port=0 marks "no TLS"; the matrix-filter
         * for HX_TEST_CAP_TLS won't pick mhxd. */
        .tls_port      = 0,
        .tls_xfer_port = 0,
        /* mhxd has no voice support — the extension is a Janus
         * (VesperNet) thing. voice_port=0 keeps mhxd out of the
         * HX_TEST_CAP_VOICE matrix filter. */
        .voice_port    = 0,
        .hl_version    = 185,
        .caps          = HX_TEST_CAP_HOPE
                       | HX_TEST_CAP_NEWS_15
                       | HX_TEST_CAP_BLOWFISH,
    },
    {
        /* Janus: VesperNet's closed-source server, pulled from
         * get.vespernet.net at image-build time. The only target
         * that ships the fogWraith chat-history extension. Also
         * supports large files, text-encoding negotiation, file-
         * mode banner, and HOPE-Secure-Login with ChaCha20-
         * Poly1305 AEAD. test_hope_chacha20 logs in as guest
         * with the empty password Janus ships upstream — that's
         * the path that works for HOPE login (server computes
         * HMAC(key="", session_key) without needing a stored
         * HOPEPassword). The Dockerfile additionally seeds
         * admin's password to "adminpass" via the REST admin
         * API at build time; see tests/janus/seed-hope-
         * passwords.sh for the full background on why we don't
         * seed guest.
         *
         * TLS shipped 2026-05 (claude/tls-phase1-control-channel):
         * the Dockerfile generates a self-signed CN=localhost cert
         * at build time and the container exposes the matching
         * HTLS / HTXF TLS ports. Host-side mapping follows the same
         * +10 convention the plain ports use (5600→5610,
         * 5601→5611). Janus is the only matrix entry advertising
         * HX_TEST_CAP_TLS today; the cap-filter routes
         * test_real_tls (Phase 1) and the upcoming Phase 2 HTXF
         * TLS tests here. */
        .name          = "janus",
        .host          = "127.0.0.1",
        .port          = 5510,
        .xfer_port     = 5511,
        .tls_port      = 5610,
        .tls_xfer_port = 5611,
        /* Janus must publish its WebRTC subchannel on a real host
         * UDP port the test client can reach. With the container
         * running under --network=host (required so libnice's
         * srflx / DTLS path actually negotiates against 127.0.0.1
         * — `docker run -p` strips the kernel route the way
         * WebRTC depends on it), Janus's internal VoiceUDPPort IS
         * the host port. config.yaml pins it to 5514 to match. */
        .voice_port    = 5514,
        .hl_version    = 190,
        .caps          = HX_TEST_CAP_LARGE_FILES
                       | HX_TEST_CAP_TEXT_ENCODING
                       | HX_TEST_CAP_CHAT_HISTORY
                       | HX_TEST_CAP_BANNER_HTXF
                       | HX_TEST_CAP_NEWS_15
                       | HX_TEST_CAP_HOPE
                       | HX_TEST_CAP_CHACHA20
                       /* Janus also accepts Blowfish under HOPE
                        * Step 2 in addition to ChaCha20 AEAD.
                        * Asserting the cap here lets the
                        * test_hope_blowfish_chat_history matrix pick
                        * land on Janus — without it, no row in the
                        * matrix had both CHAT_HISTORY and a stream
                        * cipher, and the test failed with "no server
                        * has both" at pick time. Confirmed by Misha
                        * 2026-05-24. RC4 used to be asserted here too
                        * but was retired alongside the RC4 removal. */
                       | HX_TEST_CAP_BLOWFISH
                       | HX_TEST_CAP_NICK_COLORS
                       | HX_TEST_CAP_TLS
                       /* Phase 8.F voice tests. Janus is the only
                        * matrix entry shipping the WebRTC voice
                        * extension; mhxd has no voice support. The
                        * Dockerfile EnableVoice: true + the bundled
                        * guest/admin account VoiceChat: true flip
                        * cover both the cap echo and the per-account
                        * access bit the toolbar gates on. */
                       | HX_TEST_CAP_VOICE
                       /* Phase 9.F inline-media tests. Per
                        * [[gtkhx_janus]], Misha noted 2026-06 that
                        * Janus ships the fogWraith inline-media
                        * extension. The Tier 3 binary
                        * (test_integration_inline_media) starts by
                        * probing the LOGIN reply for the cap echo —
                        * if Janus isn't actually emitting it, the
                        * test fails loudly rather than silently
                        * skipping (per [[feedback_no_test_skips]]),
                        * which is the cheapest way to learn whether
                        * the assumption holds. */
                       | HX_TEST_CAP_INLINE_MEDIA,
    },
};

const gsize hx_test_server_matrix_count =
    G_N_ELEMENTS (hx_test_server_matrix);

/* ---- GTKHX_TEST_SERVERS env filter ------------------------------- */
/*
 * Parsed once on first call and cached. We intentionally don't
 * support runtime mutation — a single test binary runs in one
 * process and all its tests should see a consistent matrix.
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

    const char *v = g_getenv ("GTKHX_TEST_SERVERS");
    if (!v || !*v) {
        return;
    }

    /* Split on comma, strip whitespace, drop empty tokens. */
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
        return TRUE; /* no filter -> everything passes */
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
hx_test_servers_with (guint32 required_caps)
{
    GPtrArray *result = g_ptr_array_new ();
    if (!result) {
        return NULL;
    }
    for (gsize i = 0; i < hx_test_server_matrix_count; i++) {
        const hx_test_server *s = &hx_test_server_matrix[i];
        if ((s->caps & required_caps) != required_caps) {
            continue;
        }
        if (!name_passes_env_filter (s->name)) {
            continue;
        }
        g_ptr_array_add (result, (gpointer) s);
    }
    return result;
}

const hx_test_server *
hx_test_server_default (void)
{
    /* Static storage for the GTKHX_TEST_HOST/PORT override case —
     * we synthesise an entry with the user's host/port and the
     * caps of whatever matrix entry we'd otherwise pick. Keeping
     * this static so the returned pointer outlives the caller.
     * Repopulated on every override-path call, so stale state
     * from a previous call with different env vars is not an
     * issue. */
    static hx_test_server overridden;

    const hx_test_server *base = NULL;

    /* Prefer "mhxd" if it survives the filter; otherwise the first
     * surviving entry. */
    for (gsize i = 0; i < hx_test_server_matrix_count; i++) {
        const hx_test_server *s = &hx_test_server_matrix[i];
        if (!name_passes_env_filter (s->name)) {
            continue;
        }
        if (strcmp (s->name, "mhxd") == 0) {
            base = s;
            break;
        }
        if (base == NULL) {
            base = s; /* first-surviving fallback */
        }
    }

    if (!base) {
        return NULL; /* env filter excluded every entry */
    }

    /* Pre-matrix env vars still work — GTKHX_TEST_HOST /
     * GTKHX_TEST_PORT swap in for the default target's
     * host:port so existing CI configs that set them keep working
     * even though the matrix entry hard-codes 127.0.0.1:5500. */
    const char *host_env = g_getenv ("GTKHX_TEST_HOST");
    const char *port_env = g_getenv ("GTKHX_TEST_PORT");
    if (!(host_env && *host_env) && !(port_env && *port_env)) {
        return base;
    }

    overridden = *base;
    if (host_env && *host_env) {
        overridden.host = host_env;
    }
    if (port_env && *port_env) {
        int v = atoi (port_env);
        if (v > 0 && v < 65536) {
            overridden.port = (guint16) v;
            /* HTXF port follows HTLS port + 1 unless explicitly
             * overridden. Honour GTKHX_TEST_XFER_PORT if set. */
            overridden.xfer_port = (guint16) (v + 1);
        }
    }
    const char *xfer_env = g_getenv ("GTKHX_TEST_XFER_PORT");
    if (xfer_env && *xfer_env) {
        int v = atoi (xfer_env);
        if (v > 0 && v < 65536) {
            overridden.xfer_port = (guint16) v;
        }
    }
    return &overridden;
}

/* connect-with-timeout lives in integration_harness.c — we call
 * its hx_integration_connect_to() entry point so we don't
 * duplicate the addrinfo + non-blocking-connect dance. The
 * prototype lives in integration_harness.h, included above. */

int
hx_test_server_connect (const hx_test_server *srv)
{
    g_return_val_if_fail (srv != NULL, -1);
    return hx_integration_connect_to (srv->host, srv->port,
                                      /*timeout_ms=*/2000);
}
