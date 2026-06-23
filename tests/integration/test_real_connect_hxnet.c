/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_real_connect_hxnet.c — R3.3.e-5 Tier 3
 * coverage that exercises the hxnet production switch.
 *
 * Companion to test_real_connect.c. Same fake-server harness,
 * same production network.c symbol under test (hx_connect). As
 * of R3.3.e-4e, hxnet is the default; this test runs with
 * GTKHX_USE_HXNET unset so it covers the no-env-var default-on
 * path.
 *
 * What this proves:
 *
 *   real_connect_hxnet/install_on_handshake
 *       Regression guard for the R3.3.e-4d install-deferral
 *       refactor. hx_connect drives through the magic exchange
 *       and send_login fires HANDSHAKE_DONE, but the bridge
 *       install is intentionally NOT triggered at handshake
 *       time — it moved out of send_login into rcv_task_login
 *       so it can capture the HOPE-negotiated cipher / compress
 *       state. The fake server stops reading after MAGIC and
 *       never sends a LOGIN response, so rcv_task_login never
 *       runs here, the install never fires, the bridge stays
 *       UN-installed, and the packed LOGIN frame stays buffered
 *       in htlc->out — exactly the shape the non-hxnet
 *       real_connect test asserts. End-to-end coverage of the
 *       actual install path is the live-server Tier 3 matrix
 *       (mhxd / Janus / hlserver.com).
 *
 *   real_connect_hxnet/tls_skips_install
 *       Negative gate: htlc->tls=1 must leave the bridge
 *       UN-installed even with hxnet as the default. TLS
 *       connections stay on the legacy GIOStream path (TLS lives
 *       in GTlsConnection, not on a raw fd hxnet's
 *       TcpStream::from_raw_fd can adopt).
 *
 *   real_connect_hxnet/opt_out_with_env_zero
 *       Opt-out gate: GTKHX_USE_HXNET=0 must leave the bridge
 *       UN-installed across the same path that otherwise installs
 *       it. Guards the only escape hatch users have if hxnet
 *       misbehaves against a real server.
 *
 * The fake-server stops reading after HTLC_MAGIC_LEN bytes —
 * after HANDSHAKE_DONE the LOGIN frame is shipped through hxnet,
 * but the server has already stopped reading and closed the
 * connection (see fake_server.c::on_write_done). That close
 * propagates back to hxnet as EOF; the actor emits
 * Event::Shutdown(Eof) which the forwarder turns into
 * bridge_on_shutdown_cb. hx_htlc_close fires from there
 * naturally — which is exactly the uninstall path the second
 * test asserts.
 */

#include "config.h"

#include <string.h>
#include <glib.h>
#include <gio/gio.h>

#include "compat.h"
#include "hotline.h"
#include "protocol.h"
#include "network.h"           /* hx_connect, hx_htlc_close */
#include "gtkhx_session.h"     /* GtkhxConnectionState */
#include "hxnet_bridge.h"      /* hx_bridge_is_installed */
#include "fake_server.h"

extern void connect_test_init_fd_table (void);

typedef struct {
    GMainLoop *loop;
    GArray    *states;
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

/* ---- tests --------------------------------------------------- */

static void
test_install_on_handshake (void)
{
    GError *err = NULL;
    hx_fake_server *srv = hx_fake_server_new (HX_FAKE_BEHAVIOR_SEND_MAGIC,
                                              HTLC_MAGIC_LEN, &err);
    g_assert_nonnull (srv);
    g_assert_no_error (err);

    GtkhxSession *gtkhx = gtkhx_session_get_default ();
    test_observer *obs = observer_new (gtkhx,
                                       GTKHX_CONNECTION_HANDSHAKE_DONE);
    reset_test_htlc ();

    /* Sanity: bridge starts uninstalled. */
    g_assert_false (hx_bridge_is_installed ());

    hx_connect (&test_htlc, "127.0.0.1", hx_fake_server_get_port (srv),
                "guest", "", 0, /*tls=*/0);

    drive_until (obs, 5000);

    g_assert_true (obs->wait_arrived);

    /* Same signal-sequence assertion as the non-hxnet test. */
    int idx_connecting = observer_index_of (obs, GTKHX_CONNECTION_CONNECTING);
    int idx_tcp        = observer_index_of (obs, GTKHX_CONNECTION_TCP_CONNECTED);
    int idx_handshake  = observer_index_of (obs, GTKHX_CONNECTION_HANDSHAKE_DONE);
    g_assert_cmpint (idx_connecting, >=, 0);
    g_assert_cmpint (idx_tcp,        >, idx_connecting);
    g_assert_cmpint (idx_handshake,  >, idx_tcp);

    /* R3.3.e-4d moved the bridge install out of send_login (which
     * fires HANDSHAKE_DONE) and into rcv_task_login (which runs
     * after the server's LOGIN response). The fake server stops
     * at MAGIC and never sends a LOGIN response, so the install
     * never fires in this fixture — that's the regression guard.
     * (An end-to-end Tier 3 against a real mhxd / Janus is what
     * actually exercises the install path; that's the live
     * matrix follow-up in R3.3.e-5.) */
    g_assert_false (hx_bridge_is_installed ());

    /* With install deferred to post-login, the LOGIN frame
     * still sits in the legacy htlc->out queue — same
     * assertion as the non-hxnet real_connect test. */
    g_assert_cmpuint (test_htlc.out.len, >, 0);

    /* The server should still have received exactly HTLC_MAGIC.
     * The LOGIN bytes that followed went on the wire through
     * hxnet but the fake server stopped reading after the magic
     * (see fake_server.c::react_after_read). */
    g_assert_true (hx_fake_server_was_accepted (srv));
    GBytes *got = hx_fake_server_get_received_bytes (srv);
    g_assert_nonnull (got);
    gsize got_len = 0;
    const guint8 *got_data = g_bytes_get_data (got, &got_len);
    g_assert_cmpuint (got_len, ==, HTLC_MAGIC_LEN);
    g_assert_cmpmem (got_data, got_len, HTLC_MAGIC, HTLC_MAGIC_LEN);
    g_bytes_unref (got);

    observer_free (obs, gtkhx);
    if (test_htlc.fd) {
        hx_htlc_close (&test_htlc, /*expected=*/1);
    }
    /* The install never fired (post-login is gated on a LOGIN
     * response we don't get in this fixture), so the bridge
     * stays uninstalled all the way through teardown. */
    g_assert_false (hx_bridge_is_installed ());

    hx_fake_server_free (srv);
}

static void
test_tls_skips_install (void)
{
    /* TLS gate: htlc->tls=1 must leave the bridge uninstalled
     * even with GTKHX_USE_HXNET set. We can't actually connect
     * with TLS in this harness (the fake_server is plain TCP),
     * so we manually set htlc->tls before driving hx_connect.
     * The connect itself will fail at the magic exchange (the
     * fake server doesn't speak TLS), but the assertion runs
     * AFTER drive_until on the DISCONNECTED state — at no point
     * during the (failed) connect did the bridge get installed,
     * because the env-var path checks htlc->tls and skips
     * straight to the legacy path.
     *
     * The cleanest test of this gate would be a TLS-capable fake
     * server; until R3.3.e-5b adds that, the assertion here is
     * "hx_bridge_is_installed remains FALSE throughout a
     * tls-flagged connect attempt." */
    GError *err = NULL;
    hx_fake_server *srv = hx_fake_server_new (HX_FAKE_BEHAVIOR_SEND_MAGIC,
                                              HTLC_MAGIC_LEN, &err);
    g_assert_nonnull (srv);
    g_assert_no_error (err);

    GtkhxSession *gtkhx = gtkhx_session_get_default ();
    test_observer *obs = observer_new (gtkhx,
                                       GTKHX_CONNECTION_DISCONNECTED);
    reset_test_htlc ();

    g_assert_false (hx_bridge_is_installed ());

    hx_connect (&test_htlc, "127.0.0.1", hx_fake_server_get_port (srv),
                "guest", "", 0, /*tls=*/1);

    drive_until (obs, 5000);

    /* The connect either fails at TLS handshake (server not TLS)
     * or somewhere later. Either way the bridge must not have
     * been installed at any point — and the gate check in
     * hx_install_hxnet_post_hope is what enforces that. */
    g_assert_false (hx_bridge_is_installed ());

    observer_free (obs, gtkhx);
    if (test_htlc.fd) {
        hx_htlc_close (&test_htlc, /*expected=*/1);
    }
    hx_fake_server_free (srv);
}

/* R3.3.e-4e: with hxnet as the default, GTKHX_USE_HXNET=0 is the
 * users' opt-out. The other tests rely on the env-var being
 * unset (covers the default-on path); this test sets it to "0"
 * for its scope and confirms the bridge stays uninstalled all
 * the way through a normal connect attempt. */
static void
test_opt_out_with_env_zero (void)
{
    g_setenv ("GTKHX_USE_HXNET", "0", TRUE);

    GError *err = NULL;
    hx_fake_server *srv = hx_fake_server_new (HX_FAKE_BEHAVIOR_SEND_MAGIC,
                                              HTLC_MAGIC_LEN, &err);
    g_assert_nonnull (srv);
    g_assert_no_error (err);

    GtkhxSession *gtkhx = gtkhx_session_get_default ();
    test_observer *obs = observer_new (gtkhx,
                                       GTKHX_CONNECTION_HANDSHAKE_DONE);
    reset_test_htlc ();
    g_assert_false (hx_bridge_is_installed ());

    hx_connect (&test_htlc, "127.0.0.1", hx_fake_server_get_port (srv),
                "guest", "", 0, /*tls=*/0);
    drive_until (obs, 5000);
    g_assert_true (obs->wait_arrived);

    /* The user opted out — bridge must not install regardless of
     * how far the connect got. */
    g_assert_false (hx_bridge_is_installed ());

    observer_free (obs, gtkhx);
    if (test_htlc.fd) {
        hx_htlc_close (&test_htlc, /*expected=*/1);
    }
    g_assert_false (hx_bridge_is_installed ());

    hx_fake_server_free (srv);
    g_unsetenv ("GTKHX_USE_HXNET");
}

int
main (int argc, char *argv[])
{
    /* R3.3.e-4e: hxnet is the default. Force the env-var unset
     * so the install / TLS-gate tests cover the no-env-var
     * default-on path even if the caller's environment had
     * GTKHX_USE_HXNET=0 set (which would otherwise silently
     * take the opt-out path in the first two tests and stop
     * covering the intended default-on behaviour). The opt-out
     * test sets and unsets the variable within its own scope. */
    g_unsetenv ("GTKHX_USE_HXNET");

    /* This suite exercises the LEGACY hx_connect connect machinery
     * (GSocketClient state machine + magic/LOGIN) plus the post-login
     * GTKHX_USE_HXNET data-path bridge installed on top of it. Now that
     * the Phase G orchestrator is the default connect path
     * (PHASE_G_DEFAULT_ON=1), pin these tests to the legacy path with
     * the GTKHX_OLD_CONNECT escape hatch — the orchestrator connect path
     * has its own coverage in test_phase_g_connect.c. Drop this when
     * delete-old-connect removes the legacy path (and this suite with
     * it). */
    g_setenv ("GTKHX_OLD_CONNECT", "1", TRUE);

    g_test_init (&argc, &argv, NULL);
    connect_test_init_fd_table ();

    g_test_add_func ("/real_connect_hxnet/install_on_handshake",
                     test_install_on_handshake);
    g_test_add_func ("/real_connect_hxnet/tls_skips_install",
                     test_tls_skips_install);
    g_test_add_func ("/real_connect_hxnet/opt_out_with_env_zero",
                     test_opt_out_with_env_zero);

    return g_test_run ();
}
