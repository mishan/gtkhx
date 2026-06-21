/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_phase_g_connect.c — Tier 3 coverage for the
 * Phase G "hxnet-owns-the-whole-lifecycle" plaintext connect path
 * (docs/phase-g-migration.md).
 *
 * Unlike test_real_connect_hxnet.c (which drives the production
 * hx_connect against the in-process fake_server and stops at the
 * magic exchange), this test drives the production hx_connect with
 * GTKHX_NEW_CONNECT=1 against a REAL mhxd container so the whole
 * orchestrator runs end to end:
 *
 *   hx_connect → hx_connect_via_orchestrator
 *     → hx_bridge_install_orchestrated_plaintext
 *       → hxnet_connection_open_plaintext  (production Rust orchestrator)
 *         → DNS + TCP + magic + LOGIN + LOGIN-reply against real mhxd
 *         → replay LOGIN reply as a synthetic Event::Frame
 *           → bridge_on_event_cb → hx_bridge_dispatch_frame
 *             → htlc->rcv (recorded by the connect_test_stubs.c
 *                          recording hx_rcv_hdr)
 *         → Event::State(HandshakeDone)
 *
 * What this proves against a real server:
 *
 *   phase_g/orchestrator_login
 *     - The production orchestrator completes the full plaintext
 *       handshake against real mhxd (proves the Rust magic / LOGIN
 *       framing is wire-compatible with a real server, not just the
 *       fake_server / duplex unit fixtures).
 *     - The coarse GtkhxConnectionState sequence the toolbar listens
 *       to fires in order: CONNECTING → TCP_CONNECTED → HANDSHAKE_DONE
 *       (same sequence the legacy GIOStream connect path emits).
 *     - The orchestrator installs the bridge (hx_bridge_is_installed).
 *     - The LOGIN reply was REPLAYED to the C dispatch carrying the
 *       pinned trans (HX_LOGIN_TRANS == 1), the HTLS_HDR_TASK opcode,
 *       and a clear error bit — i.e. mhxd accepted the guest login AND
 *       the Option-B replay + trans-pinning round-trips correctly.
 *       This is the regression guard for the two silent-failure axes
 *       called out in docs/phase-g-migration.md (trans mismatch and
 *       install ordering): either one would leave the replayed frame
 *       undispatched, so connect_test_rcv_count would stay 0.
 *
 * The downstream rcv_task_login side effects (version extraction,
 * USER_CHANGE, SELFINFO timer) are NOT asserted here — the real
 * rcv_task_login lives in rcv.c and drags the whole GTK/UI stack,
 * which a headless test binary can't link, so connect_test_stubs.c
 * supplies a recording hx_rcv_hdr instead. The header-level assertion
 * is the strongest production-path proof achievable headless.
 *
 * Hard-fail contract (same as the rest of Tier 3): if mhxd is
 * unreachable the orchestrator never reaches HandshakeDone, the
 * wait times out, and the g_assert_true on obs->wait_arrived fails
 * loudly. No silent skip.
 *
 * Server selection: GTKHX_TEST_HOST / GTKHX_TEST_PORT (default
 * 127.0.0.1:5500 — the mhxd container).
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <gio/gio.h>

#include "compat.h"
#include "hotline.h"            /* HTLS_HDR_TASK */
#include "protocol.h"
#include "network.h"           /* hx_connect, hx_htlc_close */
#include "gtkhx_session.h"     /* GtkhxConnectionState */
#include "hxnet_bridge.h"      /* hx_bridge_is_installed */

/* From connect_test_stubs.c. */
extern void connect_test_init_fd_table (void);
extern void connect_test_reset_rcv_record (void);
extern guint32 connect_test_first_rcv_type;
extern guint32 connect_test_first_rcv_trans;
extern guint32 connect_test_first_rcv_flag;
extern guint connect_test_rcv_count;

/* Mirror of HX_LOGIN_TRANS in src/network.c — the pinned LOGIN
 * transaction id the orchestrator stamps and the server echoes. */
#define PHASE_G_LOGIN_TRANS 1u

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
test_orchestrator_login (void)
{
    const char *host = g_getenv ("GTKHX_TEST_HOST");
    if (!host || !*host) {
        host = "127.0.0.1";
    }
    const char *port_env = g_getenv ("GTKHX_TEST_PORT");
    int port = (port_env && *port_env) ? atoi (port_env) : 5500;
    g_assert_cmpint (port, >, 0);

    /* Force the orchestrator path on; force the post-HOPE hxnet
     * opt-out env var clear so neither interferes with the gate. */
    g_setenv ("GTKHX_NEW_CONNECT", "1", TRUE);
    g_unsetenv ("GTKHX_TLS");
    connect_test_reset_rcv_record ();
    memset (&test_htlc, 0, sizeof (test_htlc));

    GtkhxSession *gtkhx = gtkhx_session_get_default ();
    test_observer *obs = observer_new (gtkhx,
                                       GTKHX_CONNECTION_HANDSHAKE_DONE);

    /* Sanity: bridge starts uninstalled. */
    g_assert_false (hx_bridge_is_installed ());

    hx_connect (&test_htlc, host, (guint16) port, "guest", "",
                /*secure=*/0, /*tls=*/0);

    drive_until (obs, 10000);

    /* Reached HANDSHAKE_DONE — the orchestrator completed the full
     * handshake against the real server. If mhxd is unreachable this
     * is where the test fails loudly (no silent skip). */
    g_assert_true (obs->wait_arrived);

    /* Coarse connection-state sequence in order. */
    int idx_connecting = observer_index_of (obs, GTKHX_CONNECTION_CONNECTING);
    int idx_tcp        = observer_index_of (obs, GTKHX_CONNECTION_TCP_CONNECTED);
    int idx_handshake  = observer_index_of (obs, GTKHX_CONNECTION_HANDSHAKE_DONE);
    g_assert_cmpint (idx_connecting, >=, 0);
    g_assert_cmpint (idx_tcp,        >, idx_connecting);
    g_assert_cmpint (idx_handshake,  >, idx_tcp);

    /* The orchestrator installed the bridge. */
    g_assert_true (hx_bridge_is_installed ());

    /* The LOGIN reply was replayed to the C dispatch (Option B). The
     * recording hx_rcv_hdr in connect_test_stubs.c captured it. We
     * assert on the FIRST dispatched frame: the orchestrator replays
     * the LOGIN reply before HandshakeDone, so it's guaranteed first;
     * the server's post-login pushes (SELFINFO / user-list) dispatch
     * afterwards. A count of 0 would mean the replayed frame never
     * dispatched — the trans-mismatch / install-ordering silent
     * failures from docs/phase-g-migration.md. */
    g_assert_cmpuint (connect_test_rcv_count, >=, 1);
    /* mhxd sends the plain 0x00010000 TASK opcode for every TASK
     * reply (the high-16-bit opcode-echo variant is a Heidrun-family
     * quirk, not mhxd). */
    g_assert_cmpuint (connect_test_first_rcv_type, ==, (guint32) HTLS_HDR_TASK);
    /* Pinned trans round-tripped through the real server. */
    g_assert_cmpuint (connect_test_first_rcv_trans, ==, PHASE_G_LOGIN_TRANS);
    /* mhxd accepted the guest login — error bit clear. */
    g_assert_cmpuint (connect_test_first_rcv_flag & 1u, ==, 0);

    observer_free (obs, gtkhx);
    if (test_htlc.fd) {
        hx_htlc_close (&test_htlc, /*expected=*/1);
    }
    /* hx_htlc_close uninstalls the bridge. */
    g_assert_false (hx_bridge_is_installed ());

    g_unsetenv ("GTKHX_NEW_CONNECT");
}

int
main (int argc, char *argv[])
{
    g_test_init (&argc, &argv, NULL);
    connect_test_init_fd_table ();

    g_test_add_func ("/phase_g/orchestrator_login", test_orchestrator_login);

    return g_test_run ();
}
