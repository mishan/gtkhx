/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_real_connect.c — Tier 3 coverage that
 * actually drives the production network.c::hx_connect end-to-end
 * against the fake-server harness.
 *
 * Companion / successor to test_fake_connect.c. The earlier test
 * drives a HAND-WRITTEN mirror of hx_connect's GSocketClient state
 * machine — same shape, same primitives, but written in the test
 * file. That left the literal hx_connect symbol uncovered.
 *
 * This test links src/network.c (plus the rest of its transitive
 * cipher / hope / proto pile) into the binary and calls the real
 * hx_connect. tests/integration/connect_test_stubs.c supplies
 * minimal stand-ins for the half-dozen UI/file-table symbols
 * network.c references — see that file's preamble for the
 * boundary trade-offs.
 *
 * Coverage:
 *
 *   real_connect/full_handshake  — happy path: real hx_connect,
 *       fake server returns valid HTLS_MAGIC, assert the
 *       gtkhx_session::connection-state-changed signal sequence
 *       CONNECTING -> TCP_CONNECTED -> HANDSHAKE_DONE arrives in
 *       that relative order. Verifies the server received exactly
 *       HTLC_MAGIC (the fake server stops reading after
 *       HTLC_MAGIC_LEN bytes), and that LOGIN was built and
 *       queued into htlc->out by hx_login_build_chunks +
 *       hlwrite_chunks. The LOGIN bytes never reach the wire in
 *       this test because hxd_fd_set is a no-op stub.
 *
 *   real_connect/bad_magic       — fake server replies with
 *       garbage; assert CONNECTING + TCP_CONNECTED + DISCONNECTED
 *       arrive in that order and HANDSHAKE_DONE never fires.
 *       Production's connect_fail unconditionally emits
 *       DISCONNECTED on every terminal connect failure.
 *
 *   real_connect/connect_refused — point hx_connect at a port
 *       nothing's listening on; assert CONNECTING -> DISCONNECTED
 *       arrives in order, with no TCP_CONNECTED or HANDSHAKE_DONE.
 *
 * The test does NOT drive past send_login — production installs
 * an fd watch via hxd_fd_set (stubbed no-op here) and the
 * subsequent receive loop expects rcv_task_login + chat plumbing
 * we don't link in. That boundary is the right one for testing
 * the connect state machine; downstream receive coverage is the
 * domain of the existing Tier 3 tests against mhxd/Janus.
 */

#include "config.h"

#include <string.h>
#include <glib.h>
#include <gio/gio.h>

#include "compat.h"
#include "hotline.h"
#include "protocol.h"
#include "network.h"           /* hx_connect */
#include "gtkhx_session.h"     /* GtkhxConnectionState */
#include "fake_server.h"

/* Provided by connect_test_stubs.c — lazily allocates the
 * production hxd_files[] table the first time it's called. The
 * test calls it before any hx_connect to avoid network.c's
 * send_login writing through a NULL pointer. */
extern void connect_test_init_fd_table (void);

typedef struct {
    GMainLoop *loop;
    GArray    *states;       /* of GtkhxConnectionState */
    guint      timeout_id;
    /* True when the test's expected terminal state arrived. */
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

/* Run the GMainLoop until either the observer's wait_for state
 * arrives or timeout_ms elapses. */
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

static gboolean
observer_saw (test_observer *obs, GtkhxConnectionState state)
{
    for (guint i = 0; i < obs->states->len; i++) {
        if (g_array_index (obs->states, GtkhxConnectionState, i) == state) {
            return TRUE;
        }
    }
    return FALSE;
}

/* Returns the index of the first occurrence of `state` in the
 * observed sequence, or -1 if it never arrived. Used to assert
 * relative ordering between states ("CONNECTING happened before
 * TCP_CONNECTED happened before HANDSHAKE_DONE") rather than only
 * "each state showed up at some point". */
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

/* The htlc_conn the test passes to hx_connect. Zero-initialised;
 * hx_connect sets serverhost/serverport on entry and send_login
 * sets fd / trans / in / out / rcv. */
static struct htlc_conn test_htlc;

static void
reset_test_htlc (void)
{
    memset (&test_htlc, 0, sizeof (test_htlc));
}

/* ---- tests --------------------------------------------------- */

static void
test_full_handshake (void)
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

    hx_connect (&test_htlc, "127.0.0.1", hx_fake_server_get_port (srv),
                /*login=*/"guest", /*pass=*/"", /*secure=*/0);

    drive_until (obs, 5000);

    g_assert_true (obs->wait_arrived);

    /* Sequence assertion (not just "each state showed up once"):
	 * CONNECTING must precede TCP_CONNECTED must precede
	 * HANDSHAKE_DONE. observer_index_of returns -1 for missing
	 * states; the g_assert_cmpint(... >= 0) below catches that
	 * case and the < ordering catches misordering. */
    int idx_connecting   = observer_index_of (obs, GTKHX_CONNECTION_CONNECTING);
    int idx_tcp          = observer_index_of (obs, GTKHX_CONNECTION_TCP_CONNECTED);
    int idx_handshake    = observer_index_of (obs, GTKHX_CONNECTION_HANDSHAKE_DONE);
    g_assert_cmpint (idx_connecting, >=, 0);
    g_assert_cmpint (idx_tcp,        >, idx_connecting);
    g_assert_cmpint (idx_handshake,  >, idx_tcp);

    /* The server should have received exactly HTLC_MAGIC ("TRTPHOTL"
	 * \0\1\0\2, 12 bytes). The fake server reads HTLC_MAGIC_LEN
	 * bytes from the client before reacting, so anything beyond that
	 * (like the LOGIN frame) wouldn't show up in get_received_bytes
	 * even if it had been flushed. */
    g_assert_true (hx_fake_server_was_accepted (srv));
    GBytes *got = hx_fake_server_get_received_bytes (srv);
    g_assert_nonnull (got);
    gsize got_len = 0;
    const guint8 *got_data = g_bytes_get_data (got, &got_len);
    g_assert_cmpuint (got_len, ==, HTLC_MAGIC_LEN);
    g_assert_cmpmem (got_data, got_len, HTLC_MAGIC, HTLC_MAGIC_LEN);
    g_bytes_unref (got);

    /* send_login built the LOGIN packet and queued the bytes via
	 * hlwrite_chunks, which calls hxd_fd_set(htlc->fd, FDW) to ask
	 * the event loop to flush them. Production wires that to a
	 * GIOChannel write watch; the stub in connect_test_stubs.c is a
	 * no-op, so the bytes stay in htlc->out and never reach the
	 * wire. We still want to know LOGIN was built and queued, so
	 * assert on htlc->out.len directly — non-zero proves
	 * hx_login_build_chunks + hlpack_chunks ran to completion. */
    g_assert_cmpuint (test_htlc.out.len, >, 0);

    observer_free (obs, gtkhx);
    /* Tear down the open client connection so the next subtest
	 * starts from a clean htlc + clean current_conn in network.c.
	 * hx_htlc_close emits DISCONNECTED but the observer is already
	 * unhooked above, so that signal is harmless here. */
    if (test_htlc.fd) {
        hx_htlc_close (&test_htlc, /*expected=*/1);
    }
    hx_fake_server_free (srv);
}

static void
test_bad_magic (void)
{
    GError *err = NULL;
    hx_fake_server *srv = hx_fake_server_new (HX_FAKE_BEHAVIOR_SEND_WRONG_MAGIC,
                                              HTLC_MAGIC_LEN, &err);
    g_assert_nonnull (srv);
    g_assert_no_error (err);

    GtkhxSession *gtkhx = gtkhx_session_get_default ();
    /* Wait for DISCONNECTED instead of HANDSHAKE_DONE — on bad
	 * magic, network.c::on_magic_received calls connect_fail, which
	 * unconditionally emits GTKHX_CONNECTION_DISCONNECTED. That's
	 * the contract: every terminal connect failure should reach the
	 * UI as DISCONNECTED so the toolbar/banner can react. The 5000 ms
	 * timeout below is a watchdog (DISCONNECTED arrives in <10 ms on
	 * a clean run; the wide budget exists so a loaded CI machine
	 * doesn't flake). */
    test_observer *obs = observer_new (gtkhx,
                                       GTKHX_CONNECTION_DISCONNECTED);
    reset_test_htlc ();

    hx_connect (&test_htlc, "127.0.0.1", hx_fake_server_get_port (srv),
                "guest", "", 0);

    drive_until (obs, 5000);

    /* CONNECTING + TCP_CONNECTED arrive (we got past the TCP
	 * handshake), then DISCONNECTED on the bad-magic failure path —
	 * but HANDSHAKE_DONE does NOT, because production validated
	 * the wrong magic and bailed via connect_fail before ever
	 * reaching on_async_connected_post_magic. Assert the relative
	 * ordering, not just presence. */
    g_assert_true (obs->wait_arrived);
    int idx_connecting   = observer_index_of (obs, GTKHX_CONNECTION_CONNECTING);
    int idx_tcp          = observer_index_of (obs, GTKHX_CONNECTION_TCP_CONNECTED);
    int idx_disconnected = observer_index_of (obs, GTKHX_CONNECTION_DISCONNECTED);
    g_assert_cmpint (idx_connecting,   >=, 0);
    g_assert_cmpint (idx_tcp,          >, idx_connecting);
    g_assert_cmpint (idx_disconnected, >, idx_tcp);
    g_assert_false (observer_saw (obs, GTKHX_CONNECTION_HANDSHAKE_DONE));

    observer_free (obs, gtkhx);
    if (test_htlc.fd) {
        hx_htlc_close (&test_htlc, /*expected=*/1);
    }
    hx_fake_server_free (srv);
}

static void
test_connect_refused (void)
{
    /* Bind+close a listener to grab a known-free ephemeral port. */
    GSocketListener *throwaway = g_socket_listener_new ();
    GError *err = NULL;
    int port_int = g_socket_listener_add_any_inet_port (throwaway, NULL, &err);
    g_assert_no_error (err);
    g_socket_listener_close (throwaway);
    g_object_unref (throwaway);

    GtkhxSession *gtkhx = gtkhx_session_get_default ();
    /* Wait for DISCONNECTED. Same contract as the bad-magic case:
	 * every terminal connect failure emits DISCONNECTED through
	 * connect_fail. The connect itself fails synchronously on the
	 * GSocketClient async callback with ECONNREFUSED, so DISCONNECTED
	 * follows CONNECTING with no TCP_CONNECTED in between. */
    test_observer *obs = observer_new (gtkhx,
                                       GTKHX_CONNECTION_DISCONNECTED);
    reset_test_htlc ();

    hx_connect (&test_htlc, "127.0.0.1", (guint16) port_int,
                "guest", "", 0);

    /* GSocketClient's ECONNREFUSED comes back in <10 ms on a clean
	 * box. 5000 ms is a watchdog for loaded CI; the loop quits on
	 * DISCONNECTED so the passing case is still fast. */
    drive_until (obs, 5000);

    g_assert_true (obs->wait_arrived);
    int idx_connecting   = observer_index_of (obs, GTKHX_CONNECTION_CONNECTING);
    int idx_disconnected = observer_index_of (obs, GTKHX_CONNECTION_DISCONNECTED);
    g_assert_cmpint (idx_connecting,   >=, 0);
    g_assert_cmpint (idx_disconnected, >, idx_connecting);
    /* TCP_CONNECTED and HANDSHAKE_DONE never happen because the
	 * connect itself failed before either could fire. */
    g_assert_false (observer_saw (obs, GTKHX_CONNECTION_TCP_CONNECTED));
    g_assert_false (observer_saw (obs, GTKHX_CONNECTION_HANDSHAKE_DONE));

    observer_free (obs, gtkhx);
    /* htlc->fd is 0 because send_login never ran; the close call is
	 * here for symmetry, guarded so it's a no-op if no fd was opened. */
    if (test_htlc.fd) {
        hx_htlc_close (&test_htlc, /*expected=*/1);
    }
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    connect_test_init_fd_table ();

    g_test_add_func ("/real_connect/full_handshake",
                     test_full_handshake);
    g_test_add_func ("/real_connect/bad_magic",
                     test_bad_magic);
    g_test_add_func ("/real_connect/connect_refused",
                     test_connect_refused);

    return g_test_run ();
}
