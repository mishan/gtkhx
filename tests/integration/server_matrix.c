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
         * 1.8.5-style flow, HOPE, file-mode banner, threaded
         * news. No chat-history. */
        .name       = "mhxd",
        .host       = "127.0.0.1",
        .port       = 5500,
        .xfer_port  = 5501,
        .hl_version = 185,
        .caps       = HX_TEST_CAP_HOPE
                    | HX_TEST_CAP_BANNER_HTXF
                    | HX_TEST_CAP_NEWS_15,
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
         * TLS is still off — the cert-generation flow is a
         * separate follow-up. */
        .name       = "janus",
        .host       = "127.0.0.1",
        .port       = 5510,
        .xfer_port  = 5511,
        .hl_version = 190,
        .caps       = HX_TEST_CAP_LARGE_FILES
                    | HX_TEST_CAP_TEXT_ENCODING
                    | HX_TEST_CAP_CHAT_HISTORY
                    | HX_TEST_CAP_BANNER_HTXF
                    | HX_TEST_CAP_NEWS_15
                    | HX_TEST_CAP_HOPE
                    | HX_TEST_CAP_CHACHA20,
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
