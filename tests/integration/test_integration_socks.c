/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_integration_socks.c — Tier 3 coverage for the
 * SOCKS proxy transport (R3 item 9, S1+S2).
 *
 * Drives the PRODUCTION connect path (run_plaintext_lifecycle: DNS + TCP
 * + magic + LOGIN + LOGIN-reply, via the polling FFI the orchestrator
 * harness uses) *through a real SOCKS5 proxy* to the mhxd container. This
 * is the end-to-end check the S1/S2 work deferred: it exercises
 * resolve_and_connect's proxy branch -> proxy_connect -> tokio-socks ->
 * the proxy -> mhxd, with everything downstream (magic, LOGIN) unchanged.
 *
 * Two cases:
 *
 *   1. via_proxy_login — connect through the proxy and complete a guest
 *      login. Proves the tunnel is transparent: the LOGIN reply comes
 *      back exactly as over a direct connect.
 *
 *   2. dead_proxy_fails — connect with a proxy address that refuses
 *      (127.0.0.1:1). mhxd is *directly* reachable in the matrix, so a
 *      bug that ignored the configured proxy and connected direct would
 *      wrongly succeed here. Requiring a shutdown instead is the negative
 *      control that proves the proxy is genuinely on the connection path
 *      (no silent direct bypass) without needing a blocked-egress netns.
 *
 * Config:
 *   GTKHX_TEST_HOST / GTKHX_TEST_PORT — mhxd (default 127.0.0.1:5500)
 *   GTKHX_TEST_SOCKS                  — proxy URI
 *                                       (default socks5://127.0.0.1:1080)
 *
 * Hard-fails (no skip) if the proxy or server is unreachable — same
 * contract as the rest of the integration suite. Exclude the suite at
 * meson-test time if you can't run the Docker matrix.
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <glib.h>

#include "compat.h"
#include "hotline.h" /* HTLS_HDR_TASK */

/* ---- hxnet FFI surface (mirror of rust/crates/hxnet/src/ffi.rs) --- */

typedef struct hxnet_connection hxnet_connection;

typedef struct {
    guint32 type_;
    guint32 trans;
    guint32 flag;
    guint16 hc;
    guint16 _pad;
    guint32 body_len;
    guint8 *body_ptr;
} hxnet_frame_t;

/* try_recv_frame return codes. */
#define HXNET_RECV_EMPTY 0
#define HXNET_RECV_FRAME 1
#define HXNET_RECV_SHUTDOWN 2

extern hxnet_connection *hxnet_connection_open_plaintext_polling (
    const guint8 *host, gsize host_len, guint16 port, const guint8 *login,
    gsize login_len, const guint8 *password, gsize password_len,
    const guint8 *name, gsize name_len, guint16 icon, guint16 version,
    guint16 caps, guint32 trans, const guint8 *proxy_uri, gsize proxy_uri_len);

extern int hxnet_connection_try_recv_frame (hxnet_connection *handle,
                                            hxnet_frame_t *out_frame,
                                            int *out_reason);
extern void hxnet_connection_destroy (hxnet_connection *handle);
extern void hxnet_frame_free (hxnet_frame_t *frame);

/* Poll try_recv_frame until a frame / shutdown arrives or the timeout
 * elapses. The tokio runtime drives the connection on its own thread. */
static int
poll_frame (hxnet_connection *h, hxnet_frame_t *out, int *reason,
            int timeout_ms)
{
    gint64 deadline = g_get_monotonic_time () + (gint64)timeout_ms * 1000;
    for (;;) {
        int rc = hxnet_connection_try_recv_frame (h, out, reason);
        if (rc != HXNET_RECV_EMPTY) {
            return rc;
        }
        if (g_get_monotonic_time () >= deadline) {
            return HXNET_RECV_EMPTY;
        }
        g_usleep (5000); /* 5 ms */
    }
}

static void
test_socks_env (const char **host_out, int *port_out, const char **proxy_out)
{
    const char *host = g_getenv ("GTKHX_TEST_HOST");
    if (!host || !*host) {
        host = "127.0.0.1";
    }
    const char *port_env = g_getenv ("GTKHX_TEST_PORT");
    int port = (port_env && *port_env) ? atoi (port_env) : 5500;
    g_assert_cmpint (port, >, 0);
    g_assert_cmpint (port, <=, 65535);

    const char *proxy = g_getenv ("GTKHX_TEST_SOCKS");
    if (!proxy || !*proxy) {
        proxy = "socks5://127.0.0.1:1080";
    }

    *host_out = host;
    *port_out = port;
    *proxy_out = proxy;
}

/* Positive: a guest login driven entirely by production code, tunnelled
 * through the SOCKS proxy. */
static void
test_via_proxy_login (void)
{
    const char *host, *proxy;
    int port;
    test_socks_env (&host, &port, &proxy);

    const char *login = "guest";
    hxnet_connection *h = hxnet_connection_open_plaintext_polling (
        (const guint8 *)host, strlen (host), (guint16)port,
        (const guint8 *)login, strlen (login), (const guint8 *)"",
        0,                     /* password */
        (const guint8 *)"", 0, /* name */
        /*icon=*/0, /*version=*/185,
        /*caps=*/HTLC_CAP_LARGE_FILES | HTLC_CAP_TEXT_ENCODING,
        /*trans=*/1, (const guint8 *)proxy, strlen (proxy));
    g_assert_nonnull (h);

    /* First frame: the replayed LOGIN reply. Reaching it means connect +
     * magic + LOGIN all completed over the tunnelled stream. */
    hxnet_frame_t frame;
    int reason = 0;
    int rc = poll_frame (h, &frame, &reason, 15000);
    g_assert_cmpint (rc, ==, HXNET_RECV_FRAME);
    g_assert_cmpuint (frame.type_, ==, (guint32)HTLS_HDR_TASK);
    g_assert_cmpuint (frame.trans, ==, 1);     /* HX_LOGIN_TRANS */
    g_assert_cmpuint (frame.flag & 1u, ==, 0); /* login accepted */
    hxnet_frame_free (&frame);

    /* A post-login server frame — the actor keeps reading the tunnelled
     * stream after the login reply. */
    rc = poll_frame (h, &frame, &reason, 15000);
    g_assert_cmpint (rc, ==, HXNET_RECV_FRAME);
    hxnet_frame_free (&frame);

    hxnet_connection_destroy (h);
}

/* Negative control: a refused proxy must fail the connection even though
 * mhxd is directly reachable — proving the proxy is on the path and we
 * don't silently bypass it. */
static void
test_dead_proxy_fails (void)
{
    const char *host, *proxy_unused;
    int port;
    test_socks_env (&host, &port, &proxy_unused);

    /* Port 1 refuses; this is a well-formed SOCKS URI to a dead listener,
     * not a parse error (that path returns NULL synchronously instead). */
    const char *dead_proxy = "socks5://127.0.0.1:1";
    const char *login = "guest";
    hxnet_connection *h = hxnet_connection_open_plaintext_polling (
        (const guint8 *)host, strlen (host), (guint16)port,
        (const guint8 *)login, strlen (login), (const guint8 *)"", 0,
        (const guint8 *)"", 0,
        /*icon=*/0, /*version=*/185,
        /*caps=*/HTLC_CAP_LARGE_FILES | HTLC_CAP_TEXT_ENCODING,
        /*trans=*/1, (const guint8 *)dead_proxy, strlen (dead_proxy));
    g_assert_nonnull (h);

    /* The lifecycle reports the failed connect as a shutdown event. If the
     * proxy were (buggily) ignored, this would instead be a successful
     * TASK login reply against the directly-reachable mhxd. */
    hxnet_frame_t frame;
    int reason = 0;
    int rc = poll_frame (h, &frame, &reason, 15000);
    if (rc == HXNET_RECV_FRAME) {
        g_test_fail_printf (
            "dead proxy produced a frame (type=0x%x) — the proxy was "
            "bypassed and the connection went direct",
            frame.type_);
        hxnet_frame_free (&frame);
        hxnet_connection_destroy (h);
        return;
    }
    g_assert_cmpint (rc, ==, HXNET_RECV_SHUTDOWN);

    hxnet_connection_destroy (h);
}

int
main (int argc, char *argv[])
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/socks/via_proxy_login", test_via_proxy_login);
    g_test_add_func ("/socks/dead_proxy_fails", test_dead_proxy_fails);
    return g_test_run ();
}
