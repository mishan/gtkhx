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
#include <glib/gstdio.h>      /* g_unlink, g_rmdir */
#include <gio/gio.h>

#include "compat.h"
#include "hotline.h"            /* HTLS_HDR_TASK */
#include "protocol.h"
#include "network.h"           /* hx_connect, hx_htlc_close */
#include "gtkhx_session.h"     /* GtkhxConnectionState */
#include "hxnet_bridge.h"      /* hx_bridge_is_installed */
#include "server_matrix.h"     /* hx_test_servers_with — cap-aware server pick */

/* From connect_test_stubs.c. */
extern void connect_test_init_fd_table (void);
extern void connect_test_reset_rcv_record (void);
extern guint32 connect_test_first_rcv_type;
extern guint32 connect_test_first_rcv_trans;
extern guint32 connect_test_first_rcv_flag;
extern guint connect_test_rcv_count;
extern gboolean connect_test_first_rcv_caps_present;
extern guint16 connect_test_first_rcv_caps_value;

/* server_matrix.c references hx_integration_connect_to (via the
 * unused-here hx_test_server_connect). This test never calls it, but
 * the symbol must resolve under -Wl,--no-undefined. Stub it — same
 * approach test_real_tls uses when it compiles server_matrix.c in
 * directly rather than linking the full harness lib (whose
 * hlwrite_chunks stub would collide with production network.c). */
int hx_integration_connect_to (const char *host, int port, int timeout_ms);
int
hx_integration_connect_to (const char *host, int port, int timeout_ms)
{
    (void) host;
    (void) port;
    (void) timeout_ms;
    return -1;
}

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
    /* Bound before hx_connect casts to guint16, so an out-of-range
     * GTKHX_TEST_PORT fails loudly instead of silently wrapping. */
    g_assert_cmpint (port, >, 0);
    g_assert_cmpint (port, <=, 65535);

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

/* Increment 1: prove the orchestrator advertises capabilities
 * end-to-end through production code. The plain orchestrator_login
 * test runs against the default server (mhxd), which is cap-UNAWARE
 * and ignores the chunk per spec — so it can't prove negotiation.
 * Here we drive the production hx_connect orchestrator against a
 * capability-aware matrix server (Janus, which ships the fogWraith
 * chat-history extension) and assert the server echoed our
 * HTLC_DATA_CAPABILITIES back in the LOGIN reply. Had the
 * orchestrator's LOGIN omitted the caps chunk (the bug this guards),
 * a cap-aware server would echo nothing and caps_present stays FALSE.
 *
 * Fails loudly (not g_test_skip) if no cap-aware server is in the
 * matrix — per the "no silent skips" rule. */
static void
test_orchestrator_capabilities_negotiated (void)
{
    GPtrArray *cand = hx_test_servers_with (HX_TEST_CAP_CHAT_HISTORY);
    const hx_test_server *srv = NULL;
    if (cand && cand->len > 0) {
        srv = g_ptr_array_index (cand, 0);
    }
    if (!srv) {
        if (cand) {
            g_ptr_array_unref (cand);
        }
        g_test_fail_printf (
            "no capability-aware server in matrix; need "
            "HX_TEST_CAP_CHAT_HISTORY (Janus). Start the Janus "
            "container or set GTKHX_TEST_SERVERS=janus.");
        return;
    }

    g_setenv ("GTKHX_NEW_CONNECT", "1", TRUE);
    g_unsetenv ("GTKHX_TLS");
    connect_test_reset_rcv_record ();
    memset (&test_htlc, 0, sizeof (test_htlc));

    GtkhxSession *gtkhx = gtkhx_session_get_default ();
    test_observer *obs = observer_new (gtkhx,
                                       GTKHX_CONNECTION_HANDSHAKE_DONE);

    hx_connect (&test_htlc, srv->host, srv->port, "guest", "",
                /*secure=*/0, /*tls=*/0);
    drive_until (obs, 10000);

    g_assert_true (obs->wait_arrived);

    /* The first dispatched frame is the replayed LOGIN reply. A
     * cap-aware server echoes the capability bits it accepted; the
     * orchestrator must therefore have advertised them. */
    g_assert_cmpuint (connect_test_rcv_count, >=, 1);
    g_assert_cmpuint (connect_test_first_rcv_type, ==, (guint32) HTLS_HDR_TASK);
    g_assert_true (connect_test_first_rcv_caps_present);
    /* The server echoes the subset it accepted; chat-history (bit 4)
     * is the one we picked this server for, so it must be lit. */
    g_assert_cmphex (connect_test_first_rcv_caps_value & HTLC_CAP_CHAT_HISTORY,
                     ==, HTLC_CAP_CHAT_HISTORY);

    observer_free (obs, gtkhx);
    if (test_htlc.fd) {
        hx_htlc_close (&test_htlc, /*expected=*/1);
    }
    g_unsetenv ("GTKHX_NEW_CONNECT");
    g_ptr_array_unref (cand);
}

/* Drive the production hx_connect HOPE-Secure-Login path through the
 * orchestrator (GTKHX_NEW_CONNECT=1, secure=1) against a matrix
 * server advertising `required_cap`, with `cipheralg` configured on
 * the htlc. Asserts the full handshake reaches HANDSHAKE_DONE, the
 * bridge installs, and the replayed step-2 reply dispatched to the C
 * side with the right trans (HX_LOGIN_TRANS+1 — HOPE replays step 2,
 * not the LOGIN) + success flag.
 *
 * This exercises the production Rust run_hope_lifecycle end-to-end:
 * magic + step1 + key derivation + step2 + cipher transition + the
 * encrypted step-2 reply read back through the negotiated cipher. The
 * Blowfish variant is the regression guard for the secure_login probe
 * (mhxd signals secure_login by echoing the macalg name; getting that
 * wrong made mhxd silently close after step2). */
static void
run_hope_orchestrator_against (guint32 required_cap, const char *cipheralg)
{
    GPtrArray *cand = hx_test_servers_with (required_cap);
    const hx_test_server *srv = NULL;
    if (cand && cand->len > 0) {
        srv = g_ptr_array_index (cand, 0);
    }
    if (!srv) {
        if (cand) {
            g_ptr_array_unref (cand);
        }
        g_test_fail_printf (
            "no matrix server with cap 0x%x for HOPE cipher %s; start the "
            "mhxd / Janus containers.",
            required_cap, cipheralg);
        return;
    }

    g_setenv ("GTKHX_NEW_CONNECT", "1", TRUE);
    g_unsetenv ("GTKHX_TLS");
    connect_test_reset_rcv_record ();
    memset (&test_htlc, 0, sizeof (test_htlc));
    g_strlcpy (test_htlc.cipheralg, cipheralg, sizeof (test_htlc.cipheralg));
    g_strlcpy (test_htlc.name, "PhaseGHope", sizeof (test_htlc.name));

    GtkhxSession *gtkhx = gtkhx_session_get_default ();
    test_observer *obs = observer_new (gtkhx,
                                       GTKHX_CONNECTION_HANDSHAKE_DONE);
    g_assert_false (hx_bridge_is_installed ());

    hx_connect (&test_htlc, srv->host, srv->port, "guest", "",
                /*secure=*/1, /*tls=*/0);
    drive_until (obs, 10000);

    g_assert_true (obs->wait_arrived);
    g_assert_true (hx_bridge_is_installed ());

    /* HOPE replays the step-2 reply, which carries HX_LOGIN_TRANS+1
     * (step 1 = HX_LOGIN_TRANS, step 2 = +1). */
    g_assert_cmpuint (connect_test_rcv_count, >=, 1);
    g_assert_cmpuint (connect_test_first_rcv_type, ==, (guint32) HTLS_HDR_TASK);
    g_assert_cmpuint (connect_test_first_rcv_trans, ==, PHASE_G_LOGIN_TRANS + 1);
    g_assert_cmpuint (connect_test_first_rcv_flag & 1u, ==, 0);

    observer_free (obs, gtkhx);
    if (test_htlc.fd) {
        hx_htlc_close (&test_htlc, /*expected=*/1);
    }
    g_assert_false (hx_bridge_is_installed ());
    g_unsetenv ("GTKHX_NEW_CONNECT");
    g_ptr_array_unref (cand);
}

static void
test_orchestrator_hope_blowfish (void)
{
    run_hope_orchestrator_against (HX_TEST_CAP_BLOWFISH, "BLOWFISH");
}

static void
test_orchestrator_hope_chacha20 (void)
{
    run_hope_orchestrator_against (HX_TEST_CAP_CHACHA20, "CHACHA20-POLY1305");
}

/* Drive the production hx_connect TLS path (GTKHX_NEW_CONNECT=1,
 * tls=1, secure=0) against a matrix server's dedicated TLS port —
 * the Mobius/Janus separate-port model: TLS handshake from byte zero,
 * then a plaintext LOGIN over the encrypted stream. Asserts the full
 * sequence reaches HANDSHAKE_DONE through the orchestrator's rustls
 * path and the LOGIN reply was replayed (decrypted over TLS) to the C
 * dispatch with the right trans + success flag.
 *
 * NOTE: the orchestrator TLS layer is WebPKI-first — a cert that
 * chains to a native trust root is accepted silently, and only a cert
 * that fails WebPKI is routed to the TOFU known-hosts callback. The
 * Janus test cert is self-signed, so this test exercises the
 * WebPKI-fails → TOFU-pin path (the assertions below check the pin
 * landed and a later connect is trusted silently). */
static void
test_orchestrator_tls_login (void)
{
    GPtrArray *cand = hx_test_servers_with (HX_TEST_CAP_TLS);
    const hx_test_server *srv = NULL;
    if (cand) {
        for (guint i = 0; i < cand->len; i++) {
            const hx_test_server *s = g_ptr_array_index (cand, i);
            if (s->tls_port != 0) {
                srv = s;
                break;
            }
        }
    }
    if (!srv) {
        if (cand) {
            g_ptr_array_unref (cand);
        }
        g_test_fail_printf (
            "no TLS-capable server in matrix (need HX_TEST_CAP_TLS + a "
            "tls_port; Janus). Start the Janus container with TLS ports.");
        return;
    }

    /* TOFU: isolate the known-hosts store to a tmp dir and auto-accept
     * the prompt (the dialog is stubbed in this headless binary, so
     * the prompt path would assert). With a fresh store the cert is
     * UNKNOWN → auto-accept pins it; we then assert the pin landed,
     * which proves the orchestrator's verify_cert bridge ran the real
     * tls_trust.c TOFU path end-to-end. */
    g_autofree char *tmpdir = g_dir_make_tmp ("gtkhx-phaseg-tofu-XXXXXX", NULL);
    g_assert_nonnull (tmpdir);
    g_autofree char *known_hosts = g_build_filename (tmpdir, "known_hosts", NULL);
    g_setenv ("GTKHX_KNOWN_HOSTS", known_hosts, TRUE);
    g_setenv ("GTKHX_TLS_AUTO_ACCEPT", "1", TRUE);
    g_setenv ("GTKHX_NEW_CONNECT", "1", TRUE);
    g_unsetenv ("GTKHX_TLS");
    connect_test_reset_rcv_record ();
    memset (&test_htlc, 0, sizeof (test_htlc));

    GtkhxSession *gtkhx = gtkhx_session_get_default ();
    test_observer *obs = observer_new (gtkhx,
                                       GTKHX_CONNECTION_HANDSHAKE_DONE);
    g_assert_false (hx_bridge_is_installed ());

    hx_connect (&test_htlc, srv->host, srv->tls_port, "guest", "",
                /*secure=*/0, /*tls=*/1);
    drive_until (obs, 10000);

    g_assert_true (obs->wait_arrived);
    g_assert_true (hx_bridge_is_installed ());

    /* TLS carries a plaintext LOGIN, so the replayed reply is the
     * LOGIN reply (trans HX_LOGIN_TRANS), like the non-TLS plaintext
     * path. */
    g_assert_cmpuint (connect_test_rcv_count, >=, 1);
    g_assert_cmpuint (connect_test_first_rcv_type, ==, (guint32) HTLS_HDR_TASK);
    g_assert_cmpuint (connect_test_first_rcv_trans, ==, PHASE_G_LOGIN_TRANS);
    g_assert_cmpuint (connect_test_first_rcv_flag & 1u, ==, 0);

    /* Flush the deferred pin (schedule_trust_pin → g_idle_add) and
     * assert the cert was pinned to the known-hosts store — the TOFU
     * bridge proof. */
    while (g_main_context_iteration (NULL, FALSE)) {
        /* drain pending idles */
    }
    g_autofree char *kh_contents = NULL;
    g_file_get_contents (known_hosts, &kh_contents, NULL, NULL);
    g_assert_nonnull (kh_contents);
    g_assert_nonnull (g_strstr_len (kh_contents, -1, "sha256:"));

    observer_free (obs, gtkhx);
    if (test_htlc.fd) {
        hx_htlc_close (&test_htlc, /*expected=*/1);
    }
    g_assert_false (hx_bridge_is_installed ());

    /* Second connect with AUTO_ACCEPT OFF: the cert is now pinned, so
     * tls_trust_decide must resolve TRUSTED and accept silently — no
     * prompt. (The dialog is stubbed with g_assert_not_reached in this
     * headless binary, so if the TRUSTED lookup failed and the prompt
     * fired, the test would crash.) This is the end-to-end proof that
     * the orchestrator honours a pinned cert. */
    g_unsetenv ("GTKHX_TLS_AUTO_ACCEPT");
    connect_test_reset_rcv_record ();
    memset (&test_htlc, 0, sizeof (test_htlc));
    test_observer *obs2 = observer_new (gtkhx,
                                        GTKHX_CONNECTION_HANDSHAKE_DONE);
    hx_connect (&test_htlc, srv->host, srv->tls_port, "guest", "",
                /*secure=*/0, /*tls=*/1);
    drive_until (obs2, 10000);
    g_assert_true (obs2->wait_arrived);
    g_assert_true (hx_bridge_is_installed ());
    observer_free (obs2, gtkhx);
    if (test_htlc.fd) {
        hx_htlc_close (&test_htlc, /*expected=*/1);
    }
    g_assert_false (hx_bridge_is_installed ());

    g_unsetenv ("GTKHX_NEW_CONNECT");
    g_unsetenv ("GTKHX_KNOWN_HOSTS");
    g_unlink (known_hosts);
    g_rmdir (tmpdir);
    g_ptr_array_unref (cand);
}

/* Regression guard: a refused connection must surface gracefully as
 * GTKHX_CONNECTION_DISCONNECTED (throbber off, tasks cleared, handle
 * torn down) rather than leaving the UI stuck. This exercises the
 * bridge_on_shutdown_cb → hx_bridge_dispatch_shutdown → hx_htlc_close
 * path: the bug was that the handle was cleared before dispatch, so
 * dispatch's !is_installed() guard skipped hx_htlc_close and the
 * connect/login tasks span forever. Needs no server — port 1 is
 * unbound, so the connect is refused. */
static void
test_orchestrator_connect_refused (void)
{
    g_setenv ("GTKHX_NEW_CONNECT", "1", TRUE);
    g_unsetenv ("GTKHX_TLS");
    connect_test_reset_rcv_record ();
    memset (&test_htlc, 0, sizeof (test_htlc));

    GtkhxSession *gtkhx = gtkhx_session_get_default ();
    test_observer *obs = observer_new (gtkhx, GTKHX_CONNECTION_DISCONNECTED);
    g_assert_false (hx_bridge_is_installed ());

    hx_connect (&test_htlc, "127.0.0.1", 1, "guest", "",
                /*secure=*/0, /*tls=*/0);
    drive_until (obs, 10000);

    /* The orchestrator surfaced the refused connect as DISCONNECTED
     * (hx_htlc_close ran) — not a stuck throbber. */
    g_assert_true (obs->wait_arrived);
    /* hx_htlc_close tore the bridge handle down and reset fd. */
    g_assert_false (hx_bridge_is_installed ());
    g_assert_cmpint (test_htlc.fd, ==, 0);

    observer_free (obs, gtkhx);
    if (test_htlc.fd) {
        hx_htlc_close (&test_htlc, /*expected=*/1);
    }
    g_unsetenv ("GTKHX_NEW_CONNECT");
}

/* HOPE-over-TLS is unsupported on every path. hx_connect must reject
 * it synchronously — no orchestrator install, no connection attempt,
 * no path that quietly does it. Needs no server (rejected before any
 * connect). */
static void
test_hope_tls_rejected (void)
{
    g_setenv ("GTKHX_NEW_CONNECT", "1", TRUE);
    g_unsetenv ("GTKHX_TLS");
    memset (&test_htlc, 0, sizeof (test_htlc));
    g_strlcpy (test_htlc.cipheralg, "BLOWFISH", sizeof (test_htlc.cipheralg));
    g_assert_false (hx_bridge_is_installed ());

    hx_connect (&test_htlc, "127.0.0.1", 5610, "guest", "",
                /*secure=*/1, /*tls=*/1);

    /* Rejected up front: no bridge, no fd, nothing started. */
    g_assert_false (hx_bridge_is_installed ());
    g_assert_cmpint (test_htlc.fd, ==, 0);

    /* Same via the GTKHX_TLS env override (bookmarks/power-user path). */
    g_setenv ("GTKHX_TLS", "1", TRUE);
    memset (&test_htlc, 0, sizeof (test_htlc));
    g_strlcpy (test_htlc.cipheralg, "BLOWFISH", sizeof (test_htlc.cipheralg));
    hx_connect (&test_htlc, "127.0.0.1", 5500, "guest", "",
                /*secure=*/1, /*tls=*/0);
    g_assert_false (hx_bridge_is_installed ());
    g_assert_cmpint (test_htlc.fd, ==, 0);

    g_unsetenv ("GTKHX_TLS");
    g_unsetenv ("GTKHX_NEW_CONNECT");
}

int
main (int argc, char *argv[])
{
    g_test_init (&argc, &argv, NULL);
    connect_test_init_fd_table ();

    g_test_add_func ("/phase_g/orchestrator_login", test_orchestrator_login);
    g_test_add_func ("/phase_g/capabilities_negotiated",
                     test_orchestrator_capabilities_negotiated);
    g_test_add_func ("/phase_g/hope_blowfish", test_orchestrator_hope_blowfish);
    g_test_add_func ("/phase_g/hope_chacha20", test_orchestrator_hope_chacha20);
    g_test_add_func ("/phase_g/tls_login", test_orchestrator_tls_login);
    g_test_add_func ("/phase_g/connect_refused",
                     test_orchestrator_connect_refused);
    g_test_add_func ("/phase_g/hope_tls_rejected", test_hope_tls_rejected);

    return g_test_run ();
}
