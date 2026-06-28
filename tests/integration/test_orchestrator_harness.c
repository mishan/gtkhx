/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_orchestrator_harness.c — increment 2
 * foundation: drive the PRODUCTION connect + login path from a
 * synchronous (no-GLib-main-loop) test, the way the Tier 3 harness
 * does, via the polling lifecycle FFI.
 *
 * The existing Tier 3 harness (integration_harness.c) hand-rolls its
 * own magic + LOGIN + recv over a raw blocking socket — a second
 * client wire implementation that never exercises the production
 * connect code (that's why the capabilities regression slipped
 * through; see docs/rust/phase-g-migration.md "Tier 3 coverage of the
 * production connect path"). hxnet_connection_open_plaintext_polling
 * runs the SAME production lifecycle the GUI uses
 * (run_plaintext_lifecycle: DNS + TCP + magic + LOGIN + LOGIN-reply +
 * Option-B replay + actor) but drains events synchronously via
 * hxnet_connection_try_recv_frame — no GLib main loop required.
 *
 * This test proves the foundation end-to-end against a real server:
 * a guest login driven entirely by production code, with the LOGIN
 * reply and a post-login server frame read back through the actor.
 * Converting the ~30 existing harness tests to this path (so their
 * login phase exercises production networking) is the mechanical
 * follow-up.
 *
 * Server: GTKHX_TEST_HOST / GTKHX_TEST_PORT (default 127.0.0.1:5500,
 * the mhxd container). Hard-fails (no skip) if unreachable.
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <glib.h>

#include "compat.h"
#include "hotline.h"           /* HTLS_HDR_TASK */

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
#define HXNET_RECV_EMPTY    0
#define HXNET_RECV_FRAME    1
#define HXNET_RECV_SHUTDOWN 2

extern hxnet_connection *hxnet_connection_open_plaintext_polling (
    const guint8 *host, gsize host_len, guint16 port,
    const guint8 *login, gsize login_len,
    const guint8 *password, gsize password_len,
    const guint8 *name, gsize name_len,
    guint16 icon, guint16 version, guint16 caps, guint32 trans,
    const guint8 *proxy_uri, gsize proxy_uri_len);

extern int hxnet_connection_try_recv_frame (hxnet_connection *handle,
                                            hxnet_frame_t *out_frame,
                                            int *out_reason);
extern void hxnet_connection_destroy (hxnet_connection *handle);
extern void hxnet_frame_free (hxnet_frame_t *frame);

/* Poll try_recv_frame until a frame / shutdown arrives or the timeout
 * elapses. The tokio runtime drives the connection on its own thread;
 * we just drain the event queue. Returns the HXNET_RECV_* code
 * (EMPTY on timeout). */
static int
poll_frame (hxnet_connection *h, hxnet_frame_t *out, int *reason,
            int timeout_ms)
{
    gint64 deadline = g_get_monotonic_time () + (gint64) timeout_ms * 1000;
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
test_login_via_orchestrator (void)
{
    const char *host = g_getenv ("GTKHX_TEST_HOST");
    if (!host || !*host) {
        host = "127.0.0.1";
    }
    const char *port_env = g_getenv ("GTKHX_TEST_PORT");
    int port = (port_env && *port_env) ? atoi (port_env) : 5500;
    /* Bound before the (guint16) cast below so an out-of-range
     * GTKHX_TEST_PORT fails loudly instead of silently wrapping to an
     * unintended port. */
    g_assert_cmpint (port, >, 0);
    g_assert_cmpint (port, <=, 65535);

    const char *login = "guest";
    hxnet_connection *h = hxnet_connection_open_plaintext_polling (
        (const guint8 *) host, strlen (host), (guint16) port,
        (const guint8 *) login, strlen (login),
        (const guint8 *) "", 0,   /* password */
        (const guint8 *) "", 0,   /* name (sent later via USER_CHANGE) */
        /*icon=*/0, /*version=*/185,
        /*caps=*/HTLC_CAP_LARGE_FILES | HTLC_CAP_TEXT_ENCODING
               | HTLC_CAP_CHAT_HISTORY,
        /*trans=*/1,
        /*proxy_uri=*/NULL, /*proxy_uri_len=*/0);
    g_assert_nonnull (h);

    /* First frame: the replayed LOGIN reply. The whole connect +
     * magic + LOGIN ran through production code (run_plaintext_lifecycle)
     * — if any of it were wrong against a real server, we'd get a
     * shutdown or a timeout here instead. */
    hxnet_frame_t frame;
    int reason = 0;
    int rc = poll_frame (h, &frame, &reason, 10000);
    g_assert_cmpint (rc, ==, HXNET_RECV_FRAME);
    g_assert_cmpuint (frame.type_, ==, (guint32) HTLS_HDR_TASK);
    g_assert_cmpuint (frame.trans, ==, 1);          /* HX_LOGIN_TRANS */
    g_assert_cmpuint (frame.flag & 1u, ==, 0);      /* login accepted */
    hxnet_frame_free (&frame);

    /* At least one post-login server frame (SELFINFO / agreement /
     * banner / chat-history …) — proves the actor keeps reading the
     * stream after the login reply, i.e. a converted harness test
     * could drive its protocol exchange from here. */
    rc = poll_frame (h, &frame, &reason, 10000);
    g_assert_cmpint (rc, ==, HXNET_RECV_FRAME);
    hxnet_frame_free (&frame);

    hxnet_connection_destroy (h);
}

int
main (int argc, char *argv[])
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/orchestrator_harness/login_via_orchestrator",
                     test_login_via_orchestrator);
    return g_test_run ();
}
