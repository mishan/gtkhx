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
#include "tls_trust.h"           /* hx_tls_trust_pin */

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

/* Pump the default main context until `path` is non-empty or
 * `timeout_ms` elapses. The TOFU pin is deferred via g_idle_add
 * (schedule_trust_pin -> trust_pin_idle in network.c), so after a
 * connect reaches HANDSHAKE_DONE the known_hosts write may still be
 * queued; this lets it run. Returns TRUE once the file has content. */
static gboolean
wait_for_known_hosts (const char *path, guint timeout_ms)
{
    gint64 deadline = g_get_monotonic_time () + (gint64) timeout_ms * 1000;
    do {
        while (g_main_context_iteration (NULL, FALSE)) {
            /* drain queued idle/timeout sources, incl. the pin */
        }
        gchar *contents = NULL;
        gsize len = 0;
        if (g_file_get_contents (path, &contents, &len, NULL) && len > 0) {
            g_free (contents);
            return TRUE;
        }
        g_free (contents);
        g_usleep (10 * 1000);
    } while (g_get_monotonic_time () < deadline);
    return FALSE;
}

/* Two-phase TOFU test that exercises the PRODUCTION trust decision
 * (network.c::tls_trust_decide) WITHOUT the AUTO_ACCEPT bypass:
 *
 *   Phase 1 (AUTO_ACCEPT on):  connect -> Janus's self-signed cert is
 *       UNKNOWN -> auto-pinned to the test known_hosts file.
 *   Phase 2 (AUTO_ACCEPT off): reconnect -> the now-pinned fingerprint
 *       looks up TRUSTED -> tls_trust_decide returns TRUE *silently*
 *       (no prompt, no auto-accept) -> handshake completes.
 *
 * Phase 2 reaching HANDSHAKE_DONE with AUTO_ACCEPT unset is the proof
 * that the real known-hosts decision — not the headless escape hatch —
 * accepted the pinned cert. (The classification logic is unit-tested in
 * tests/unit/test_tls_trust.c; this pins the live wiring: real
 * handshake -> real fingerprint -> lookup -> silent accept. It also
 * covers the same tls_trust_decide the orchestrator + HTXF-rust TLS
 * paths call via hx_tls_orchestrator_verify_cert.) */
static void
test_tls_trusted_pin_silent_accept (void)
{
    const hx_test_server *srv = pick_tls_server ();
    if (!srv) {
        g_test_fail_printf (
            "no TLS-capable server in matrix (need HX_TEST_CAP_TLS + "
            "tls_port != 0; run with GTKHX_TEST_SERVERS=janus).");
        return;
    }
    const char *known_hosts = g_getenv ("GTKHX_KNOWN_HOSTS");
    g_assert_nonnull (known_hosts);

    GtkhxSession *gtkhx = gtkhx_session_get_default ();

    /* ---- Phase 1: AUTO_ACCEPT pins the (UNKNOWN) cert. ---- */
    g_setenv ("GTKHX_TLS_AUTO_ACCEPT", "1", TRUE);
    {
        test_observer *obs =
            observer_new (gtkhx, GTKHX_CONNECTION_HANDSHAKE_DONE);
        reset_test_htlc ();
        hx_connect (&test_htlc, srv->host, srv->tls_port, "guest", "", 0, 1);
        drive_until (obs, 10000);
        g_assert_true (obs->wait_arrived);
        observer_free (obs, gtkhx);
        if (test_htlc.fd) {
            hx_htlc_close (&test_htlc, /*expected=*/1);
        }
    }
    /* The pin is g_idle-deferred — let it land before phase 2. */
    g_assert_true (wait_for_known_hosts (known_hosts, 3000));

    /* Janus rate-limits per IP between connection bursts; settle. */
    g_usleep (1000 * 1000);

    /* ---- Phase 2: no AUTO_ACCEPT — the pin alone must carry it. ----
     * If the pin didn't take (or the fp differs), tls_trust_decide
     * falls through to the prompt path, which in this headless binary
     * never resolves: HANDSHAKE_DONE won't arrive and the assert below
     * fails. A clean pass means the TRUSTED branch (line ~1763) returned
     * TRUE without a prompt. */
    g_unsetenv ("GTKHX_TLS_AUTO_ACCEPT");
    {
        test_observer *obs =
            observer_new (gtkhx, GTKHX_CONNECTION_HANDSHAKE_DONE);
        reset_test_htlc ();
        hx_connect (&test_htlc, srv->host, srv->tls_port, "guest", "", 0, 1);
        drive_until (obs, 10000);
        g_assert_true (obs->wait_arrived);
        observer_free (obs, gtkhx);
        if (test_htlc.fd) {
            hx_htlc_close (&test_htlc, /*expected=*/1);
        }
    }

    /* Restore the harness default for any later test in this binary. */
    g_setenv ("GTKHX_TLS_AUTO_ACCEPT", "1", TRUE);
}

/* The reject side of TOFU: a cert that classifies as MISMATCH and is
 * rejected at the prompt must NOT complete the handshake. AUTO_ACCEPT
 * can't test this (it always accepts), so this uses the
 * GTKHX_TLS_TEST_PROMPT=reject seam — which still runs the real
 * lookup/classify, only substituting the human's "reject" click.
 *
 * Setup pins a BOGUS fingerprint for the server's host:port in an
 * isolated known_hosts file, so the real Janus cert classifies as
 * MISMATCH. The load-bearing assertion is that HANDSHAKE_DONE never
 * arrives — a refused cert can't reach login. */
static void
test_tls_mismatch_rejected (void)
{
    const hx_test_server *srv = pick_tls_server ();
    if (!srv) {
        g_test_fail_printf (
            "no TLS-capable server in matrix (need HX_TEST_CAP_TLS + "
            "tls_port != 0; run with GTKHX_TEST_SERVERS=janus).");
        return;
    }

    /* Isolated known_hosts so this test's bogus pin can't leak into (or
     * be clobbered by) the shared file the other subtests use. */
    g_autofree char *kh = g_build_filename (
        g_get_tmp_dir (), "gtkhx-real-tls-mismatch-known-hosts", NULL);
    (void) g_unlink (kh);
    const char *prev_kh = g_getenv ("GTKHX_KNOWN_HOSTS");
    g_autofree char *prev_kh_dup = prev_kh ? g_strdup (prev_kh) : NULL;
    g_setenv ("GTKHX_KNOWN_HOSTS", kh, TRUE);

    /* Pin a fingerprint the real cert can't match → MISMATCH. */
    const char *bogus = "sha256:"
        "0000000000000000000000000000000000000000000000000000000000000000";
    g_assert_true (hx_tls_trust_pin (srv->host, srv->tls_port, bogus));

    /* Reject path, driven headlessly. AUTO_ACCEPT must be OFF or it
     * would override the MISMATCH. */
    g_unsetenv ("GTKHX_TLS_AUTO_ACCEPT");
    g_setenv ("GTKHX_TLS_TEST_PROMPT", "reject", TRUE);

    g_usleep (1000 * 1000); /* settle Janus per-IP rate limit */

    GtkhxSession *gtkhx = gtkhx_session_get_default ();
    test_observer *obs = observer_new (gtkhx, GTKHX_CONNECTION_DISCONNECTED);
    reset_test_htlc ();
    hx_connect (&test_htlc, srv->host, srv->tls_port, "guest", "", 0, 1);
    drive_until (obs, 10000);

    /* The connection must be torn down (DISCONNECTED reached, not a
     * timeout) AND must never have completed the handshake. Together
     * these prove the rejected cert was refused and the connection
     * closed — not silently accepted or left hanging. */
    g_assert_true (obs->wait_arrived);
    int idx_handshake =
        observer_index_of (obs, GTKHX_CONNECTION_HANDSHAKE_DONE);
    g_assert_cmpint (idx_handshake, ==, -1);

    observer_free (obs, gtkhx);
    if (test_htlc.fd) {
        hx_htlc_close (&test_htlc, /*expected=*/0);
    }

    /* Restore the harness defaults for any later test in this binary.
     * Restore GTKHX_KNOWN_HOSTS to its exact prior state — re-set it if
     * it was set before, but UNSET it if it wasn't, so the temporary
     * mismatch file never leaks into a later subtest or a future
     * refactor of main(). */
    g_unsetenv ("GTKHX_TLS_TEST_PROMPT");
    g_setenv ("GTKHX_TLS_AUTO_ACCEPT", "1", TRUE);
    if (prev_kh_dup) {
        g_setenv ("GTKHX_KNOWN_HOSTS", prev_kh_dup, TRUE);
    } else {
        g_unsetenv ("GTKHX_KNOWN_HOSTS");
    }
}

int
main (int argc, char **argv)
{
    /* Covers the LEGACY hx_connect tls=1 path (GSocketClient TLS
     * handshake + accept-certificate handler). Now that the Phase G
     * orchestrator is the default (PHASE_G_DEFAULT_ON=1), pin to the
     * legacy path via the GTKHX_OLD_CONNECT escape hatch; the
     * orchestrator TLS path is covered by test_phase_g_connect.c. Drop
     * when delete-old-connect removes the legacy path. */
    g_setenv ("GTKHX_OLD_CONNECT", "1", TRUE);

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
    g_test_add_func ("/real_tls/trusted_pin_silent_accept",
                     test_tls_trusted_pin_silent_accept);
    g_test_add_func ("/real_tls/mismatch_rejected",
                     test_tls_mismatch_rejected);

    return g_test_run ();
}
