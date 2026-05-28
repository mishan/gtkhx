/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_real_tls.c — Tier 3 coverage for the
 * Mobius / Janus separate-port TLS model (docs/tls-scoping.md
 * Phase 1). Drives production network.c::hx_connect with tls=1
 * against a real server's TLS-listening port and asserts the
 * connection-state signal sequence completes through to
 * HANDSHAKE_DONE — i.e. TCP connect + TLS handshake + HTLS magic
 * exchange all succeeded over the wrapped socket.
 *
 * Sister test to test_real_connect.c, which exercises the same
 * state machine over plaintext against an in-process
 * GSocketListener. This binary differs in two ways:
 *
 *   1. The server is real (no fake-server harness). We pick
 *      whichever matrix entry advertises HX_TEST_CAP_TLS — today
 *      Janus, on the tls_port (5610 host-mapped).
 *   2. hx_connect is called with tls=1, which trips the
 *      g_socket_client_set_tls + accept-certificate stub path
 *      added in commit 4550730. The stub accepts every cert; the
 *      Janus container ships a self-signed CN=localhost so a
 *      stricter trust check would refuse, and Phase 3 will
 *      replace the stub with a real TOFU lookup.
 *
 * The observer + stub plumbing is identical to test_real_connect's
 * (connect_test_stubs.c is reused), so the assertions are pinned
 * to the same GtkhxSession::connection-state-changed sequence
 * (CONNECTING -> TCP_CONNECTED -> HANDSHAKE_DONE).
 *
 * Fails (NOT skips) when no TLS-capable matrix entry is
 * configured (e.g. GTKHX_TEST_SERVERS=mhxd or the GTKHX_NO_TLS
 * env var). Per the "no silent skips" memory, we hard-fail via
 * g_test_fail_printf with a specific diagnostic: a missing
 * matrix entry should never masquerade as a green run in CI.
 * If you don't want to run TLS coverage, deselect this binary
 * from meson — don't make it pass-by-skipping.
 */

#include "config.h"

#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>

#include "compat.h"
#include "hotline.h"
#include "protocol.h"
#include "network.h"           /* hx_connect */
#include "gtkhx_session.h"     /* GtkhxConnectionState */
#include "server_matrix.h"
#include "integration_harness.h" /* hx_integration_connect_to prototype */

/* Provided by connect_test_stubs.c. */
extern void connect_test_init_fd_table (void);

/* server_matrix.c's hx_test_server_connect calls
 * hx_integration_connect_to (defined in integration_harness.c).
 * We deliberately don't link integration_harness.c — its
 * hlwrite_chunks stub would collide with the real one we pull in
 * from src/network.c. This test only uses hx_test_servers_with
 * for matrix-filter lookups, so the connect-to entry point is
 * never reached. Provide a g_assert_not_reached stub so the
 * linker is happy AND so a future revision that accidentally
 * calls hx_test_server_connect aborts loudly (rather than
 * silently succeeding on a NULL-success or hanging on a
 * NULL-return). When that day comes, replace this with the
 * real connect-with-timeout implementation. */
int
hx_integration_connect_to (const char *host G_GNUC_UNUSED,
                           int port G_GNUC_UNUSED,
                           int timeout_ms G_GNUC_UNUSED)
{
    g_assert_not_reached ();
    return -1;
}

typedef struct {
    GMainLoop *loop;
    GArray    *states;       /* of GtkhxConnectionState */
    guint      timeout_id;
    GtkhxConnectionState wait_for;
    gboolean   wait_arrived;
} test_observer;

static void
on_connection_state (GtkhxSession *self, GtkhxConnectionState state,
                     gpointer user_data)
{
    test_observer *obs = user_data;
    (void) self;
    g_array_append_val (obs->states, state);
    if (state == obs->wait_for) {
        obs->wait_arrived = TRUE;
        g_main_loop_quit (obs->loop);
    }
}

static gboolean
on_observer_timeout (gpointer u)
{
    test_observer *obs = u;
    obs->timeout_id = 0;
    g_main_loop_quit (obs->loop);
    return G_SOURCE_REMOVE;
}

static void
drive_until (test_observer *obs, guint timeout_ms)
{
    obs->loop = g_main_loop_new (NULL, FALSE);
    obs->timeout_id = g_timeout_add (timeout_ms, on_observer_timeout, obs);
    g_main_loop_run (obs->loop);
    if (obs->timeout_id) {
        g_source_remove (obs->timeout_id);
        obs->timeout_id = 0;
    }
    g_main_loop_unref (obs->loop);
    obs->loop = NULL;
}

static test_observer *
observer_new (GtkhxSession *sess, GtkhxConnectionState wait_for)
{
    test_observer *obs = g_new0 (test_observer, 1);
    obs->states = g_array_new (FALSE, FALSE, sizeof (GtkhxConnectionState));
    obs->wait_for = wait_for;
    obs->wait_arrived = FALSE;
    g_signal_connect (sess, "connection-state-changed",
                      G_CALLBACK (on_connection_state), obs);
    return obs;
}

static void
observer_free (test_observer *obs, GtkhxSession *sess)
{
    g_signal_handlers_disconnect_by_data (sess, obs);
    g_array_free (obs->states, TRUE);
    g_free (obs);
}

/* Index of the first occurrence of `state` in the observed
 * sequence, or -1 if it never arrived. */
static int
observer_index_of (test_observer *obs, GtkhxConnectionState state)
{
    for (guint i = 0; i < obs->states->len; i++) {
        if (g_array_index (obs->states, GtkhxConnectionState, i) == state) {
            return (int) i;
        }
    }
    return -1;
}

static struct htlc_conn test_htlc;

static void
reset_test_htlc (void)
{
    memset (&test_htlc, 0, sizeof (test_htlc));
}

/* Pick the first matrix entry advertising both TLS support and a
 * non-zero tls_port. The matrix is small enough that linear scan
 * is fine. Returns NULL when no entry qualifies; caller handles
 * the skip diagnostic. */
static const hx_test_server *
pick_tls_server (void)
{
    GPtrArray *candidates = hx_test_servers_with (HX_TEST_CAP_TLS);
    if (!candidates) {
        return NULL;
    }
    const hx_test_server *picked = NULL;
    for (guint i = 0; i < candidates->len; i++) {
        const hx_test_server *s = g_ptr_array_index (candidates, i);
        if (s->tls_port != 0) {
            picked = s;
            break;
        }
    }
    g_ptr_array_unref (candidates);
    return picked;
}

/* ---- tests --------------------------------------------------- */

static void
test_tls_full_handshake (void)
{
    const hx_test_server *srv = pick_tls_server ();
    if (!srv) {
        /* Loud failure rather than g_test_skip so a missing matrix
         * entry shows up in CI as a failed test, not a silent pass.
         * See memory feedback_no_test_skips for the rationale. */
        g_test_fail_printf (
            "no TLS-capable server in matrix (need HX_TEST_CAP_TLS + "
            "tls_port != 0). Run with GTKHX_TEST_SERVERS=janus or bring "
            "up the Janus container with tests/janus/Dockerfile.");
        return;
    }

    GtkhxSession *gtkhx = gtkhx_session_get_default ();
    test_observer *obs = observer_new (gtkhx,
                                       GTKHX_CONNECTION_HANDSHAKE_DONE);
    reset_test_htlc ();

    /* tls=1 trips network.c::hx_connect's
     * g_socket_client_set_tls(client, TRUE) path. The accept-
     * certificate stub (tls_accept_certificate_phase1_stub) is
     * hooked via the GSocketClient event signal at
     * G_SOCKET_CLIENT_TLS_HANDSHAKING and accepts the self-signed
     * Janus cert unconditionally. */
    hx_connect (&test_htlc, srv->host, srv->tls_port,
                /*login=*/"guest", /*pass=*/"", /*secure=*/0, /*tls=*/1);

    /* 10s budget: TLS handshake + Janus's HTLS magic exchange take
     * < 200 ms on a clean box. The wide timeout covers loaded CI +
     * the per-IP rate-limit quiescence Janus enforces between
     * connection bursts. */
    drive_until (obs, 10000);

    g_assert_true (obs->wait_arrived);

    /* Same ordering invariant as test_real_connect's happy path:
     * CONNECTING (immediate) -> TCP_CONNECTED (kernel accepted)
     * -> HANDSHAKE_DONE (HTLS magic round-tripped over the TLS
     * stream). TCP_CONNECTED fires after the GSocketClient async
     * connect resolves — that includes the TLS handshake because
     * g_socket_client_set_tls extends the connect phase. */
    int idx_connecting = observer_index_of (obs, GTKHX_CONNECTION_CONNECTING);
    int idx_tcp        = observer_index_of (obs, GTKHX_CONNECTION_TCP_CONNECTED);
    int idx_handshake  = observer_index_of (obs, GTKHX_CONNECTION_HANDSHAKE_DONE);
    g_assert_cmpint (idx_connecting, >=, 0);
    g_assert_cmpint (idx_tcp,        >, idx_connecting);
    g_assert_cmpint (idx_handshake,  >, idx_tcp);

    /* send_login should have queued the LOGIN packet into htlc->out
     * the same way the plaintext test asserts — the TLS layer is
     * transparent to the LOGIN build path. Non-zero out.len proves
     * hx_login_build_chunks + hlpack_chunks ran past send_login. */
    g_assert_cmpuint (test_htlc.out.len, >, 0);

    observer_free (obs, gtkhx);
    if (test_htlc.fd) {
        hx_htlc_close (&test_htlc, /*expected=*/1);
    }
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    connect_test_init_fd_table ();

    /* Phase 3: the production accept-certificate handler now does
     * a real TOFU lookup. In the test harness there's no
     * GtkApplication / toolbar window to prompt against, so the
     * dialog path would hit a g_assert_not_reached stub. Flip
     * the auto-accept env override so unknown certs are pinned
     * silently — the test cares about the wire path, not the
     * trust UX (which the tls_trust Tier 1 + dialog Adwaita
     * unit coverage handles separately). Also point
     * GTKHX_KNOWN_HOSTS at a per-process tmpdir so the test
     * doesn't touch the developer's real $CONFIG/known_hosts. */
    g_setenv ("GTKHX_TLS_AUTO_ACCEPT", "1", TRUE);
    g_autofree char *tmp_known_hosts =
        g_build_filename (g_get_tmp_dir (), "gtkhx-real-tls-known-hosts",
                          NULL);
    g_setenv ("GTKHX_KNOWN_HOSTS", tmp_known_hosts, TRUE);
    (void) g_unlink (tmp_known_hosts);

    g_test_add_func ("/real_tls/full_handshake",
                     test_tls_full_handshake);

    return g_test_run ();
}
