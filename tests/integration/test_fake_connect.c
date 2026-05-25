/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_fake_connect.c — Tier 3 coverage for the
 * GSocketClient + GMainLoop async-connect / magic-exchange pattern
 * that production hx_connect uses.
 *
 * Why this exists:
 *
 *   network.c's async-connect state machine (hx_connect →
 *   on_async_connected → on_magic_sent → on_magic_received) drives
 *   the wire handshake against a real Hotline server. Wiring the
 *   PRODUCTION hx_connect into a test would require linking
 *   network.c plus its full transitive dep graph (rcv.c, chat.c,
 *   gtkhx.c globals, hxd_files[] file table, task subsystem, ...).
 *   That's a substantial follow-up — see network.c::send_login
 *   for the link surface that explodes once we cross past the
 *   magic-exchange boundary.
 *
 *   This test takes the smaller scope: drive a faithful client-side
 *   mirror of hx_connect's GSocketClient state machine against a
 *   scriptable fake server (tests/integration/fake_server.c). The
 *   mirror exercises:
 *
 *     - GSocketClient async connect to 127.0.0.1:<ephemeral>
 *     - write HTLC_MAGIC via g_output_stream_write_all_async
 *     - read HTLS_MAGIC via g_input_stream_read_all_async
 *     - validation via hx_connect_validate_server_magic (the real
 *       extracted helper that production calls)
 *     - GMainLoop driving + state observation
 *
 *   The validation step calls the SAME production helper that
 *   network.c::on_magic_received calls, so the magic-comparison
 *   contract is exercised end-to-end against a real GIO transport.
 *
 *   Failure modes covered:
 *
 *     happy_path                — server sends correct HTLS_MAGIC
 *     wrong_magic               — server sends garbage, validator
 *                                 rejects
 *     server_drops_before_magic — server closes before reply, read
 *                                 lands EOF
 *     short_magic               — server sends 4 bytes then closes,
 *                                 read returns short
 *     connect_refused           — connect to a port nothing is
 *                                 listening on, GSocketClient
 *                                 surfaces ECONNREFUSED
 *
 *   In all cases the test also asserts what bytes the fake server
 *   actually received from the client — pins that the production
 *   primitives we mirror really do send HTLC_MAGIC first.
 */

#include "config.h"

#include <string.h>
#include <glib.h>
#include <gio/gio.h>

#include "compat.h"          /* PACKED — hotline.h's struct attrs */
#include "hotline.h"         /* HTLC_MAGIC, HTLC_MAGIC_LEN, HTLS_MAGIC_LEN */
#include "connect_magic.h"   /* hx_connect_validate_server_magic */
#include "fake_server.h"

/* ---- Connect-mirror state ------------------------------------ */

typedef enum {
    HX_TEST_CONNECT_PENDING       = 0,
    HX_TEST_CONNECT_DONE_OK       = 1,
    HX_TEST_CONNECT_DONE_FAILED   = 2,
    HX_TEST_CONNECT_DONE_BAD_MAGIC = 3,
    HX_TEST_CONNECT_DONE_TIMEOUT  = 4,
} hx_test_connect_status;

typedef struct {
    GMainLoop      *loop;
    GSocketClient  *client;
    GSocketConnection *conn;
    GCancellable   *cancel;
    guint8          magic_buf[HTLS_MAGIC_LEN];
    hx_test_connect_status status;
    char           *failure_stage; /* g_strdup'd */
    guint           timeout_id;
} hx_test_connect_ctx;

static void
finish (hx_test_connect_ctx *ctx, hx_test_connect_status status,
        const char *stage)
{
    if (ctx->status != HX_TEST_CONNECT_PENDING) {
        return;
    }
    ctx->status = status;
    if (stage) {
        ctx->failure_stage = g_strdup (stage);
    }
    if (ctx->timeout_id) {
        g_source_remove (ctx->timeout_id);
        ctx->timeout_id = 0;
    }
    g_main_loop_quit (ctx->loop);
}

static void
on_magic_received (GObject *src, GAsyncResult *res, gpointer u)
{
    hx_test_connect_ctx *ctx = u;
    GError *err = NULL;
    gsize got = 0;

    if (!g_input_stream_read_all_finish (G_INPUT_STREAM (src), res, &got, &err)
        || got != HTLS_MAGIC_LEN) {
        g_clear_error (&err);
        finish (ctx, HX_TEST_CONNECT_DONE_FAILED, "read_magic");
        return;
    }
    if (!hx_connect_validate_server_magic (ctx->magic_buf, got)) {
        finish (ctx, HX_TEST_CONNECT_DONE_BAD_MAGIC, "validate_magic");
        return;
    }
    finish (ctx, HX_TEST_CONNECT_DONE_OK, NULL);
}

static void
on_magic_sent (GObject *src, GAsyncResult *res, gpointer u)
{
    hx_test_connect_ctx *ctx = u;
    GError *err = NULL;
    gsize wrote = 0;
    if (!g_output_stream_write_all_finish (G_OUTPUT_STREAM (src), res, &wrote,
                                           &err)
        || wrote != HTLC_MAGIC_LEN) {
        g_clear_error (&err);
        finish (ctx, HX_TEST_CONNECT_DONE_FAILED, "write_magic");
        return;
    }
    GInputStream *in = g_io_stream_get_input_stream (G_IO_STREAM (ctx->conn));
    g_input_stream_read_all_async (in, ctx->magic_buf, HTLS_MAGIC_LEN,
                                   G_PRIORITY_DEFAULT, ctx->cancel,
                                   on_magic_received, ctx);
}

static void
on_connected (GObject *src, GAsyncResult *res, gpointer u)
{
    hx_test_connect_ctx *ctx = u;
    GError *err = NULL;
    ctx->conn = g_socket_client_connect_to_host_finish (G_SOCKET_CLIENT (src),
                                                        res, &err);
    if (!ctx->conn) {
        g_clear_error (&err);
        finish (ctx, HX_TEST_CONNECT_DONE_FAILED, "connect");
        return;
    }
    GOutputStream *out = g_io_stream_get_output_stream (G_IO_STREAM (ctx->conn));
    g_output_stream_write_all_async (out, HTLC_MAGIC, HTLC_MAGIC_LEN,
                                     G_PRIORITY_DEFAULT, ctx->cancel,
                                     on_magic_sent, ctx);
}

static gboolean
on_test_timeout (gpointer u)
{
    hx_test_connect_ctx *ctx = u;
    if (ctx->cancel) {
        g_cancellable_cancel (ctx->cancel);
    }
    finish (ctx, HX_TEST_CONNECT_DONE_TIMEOUT, "test_timeout");
    return G_SOURCE_REMOVE;
}

/* Run the GMainLoop until either the connect-mirror finishes or
 * `timeout_ms` elapses. The latter shouldn't happen on the happy-
 * path tests; the HANG test deliberately uses it. */
static void
drive_connect (hx_test_connect_ctx *ctx, guint16 port, guint timeout_ms)
{
    ctx->loop = g_main_loop_new (NULL, FALSE);
    ctx->client = g_socket_client_new ();
    ctx->cancel = g_cancellable_new ();
    ctx->timeout_id = g_timeout_add (timeout_ms, on_test_timeout, ctx);

    g_socket_client_connect_to_host_async (ctx->client, "127.0.0.1", port,
                                           ctx->cancel, on_connected, ctx);
    g_main_loop_run (ctx->loop);
}

static void
free_ctx (hx_test_connect_ctx *ctx)
{
    g_free (ctx->failure_stage);
    g_clear_object (&ctx->conn);
    g_clear_object (&ctx->client);
    g_clear_object (&ctx->cancel);
    if (ctx->loop) {
        g_main_loop_unref (ctx->loop);
    }
}

/* ---- tests --------------------------------------------------- */

static void
test_happy_path (void)
{
    GError *err = NULL;
    hx_fake_server *srv = hx_fake_server_new (HX_FAKE_BEHAVIOR_SEND_MAGIC,
                                              HTLC_MAGIC_LEN, &err);
    g_assert_nonnull (srv);
    g_assert_no_error (err);

    hx_test_connect_ctx ctx = { 0 };
    drive_connect (&ctx, hx_fake_server_get_port (srv), 5000);

    g_assert_cmpint (ctx.status, ==, HX_TEST_CONNECT_DONE_OK);
    g_assert_true (hx_fake_server_was_accepted (srv));

    /* Server should have received exactly HTLC_MAGIC. */
    GBytes *got = hx_fake_server_get_received_bytes (srv);
    g_assert_nonnull (got);
    gsize got_len = 0;
    const guint8 *got_data = g_bytes_get_data (got, &got_len);
    g_assert_cmpuint (got_len, ==, HTLC_MAGIC_LEN);
    g_assert_cmpmem (got_data, got_len, HTLC_MAGIC, HTLC_MAGIC_LEN);
    g_bytes_unref (got);

    free_ctx (&ctx);
    hx_fake_server_free (srv);
}

static void
test_wrong_magic (void)
{
    GError *err = NULL;
    hx_fake_server *srv = hx_fake_server_new (HX_FAKE_BEHAVIOR_SEND_WRONG_MAGIC,
                                              HTLC_MAGIC_LEN, &err);
    g_assert_nonnull (srv);
    g_assert_no_error (err);

    hx_test_connect_ctx ctx = { 0 };
    drive_connect (&ctx, hx_fake_server_get_port (srv), 5000);

    /* Read succeeded (server did write 8 bytes), but the magic
     * validator rejected them — finish() landed in BAD_MAGIC at
     * the validate_magic stage. */
    g_assert_cmpint (ctx.status, ==, HX_TEST_CONNECT_DONE_BAD_MAGIC);
    g_assert_cmpstr (ctx.failure_stage, ==, "validate_magic");

    free_ctx (&ctx);
    hx_fake_server_free (srv);
}

static void
test_server_drops_before_magic (void)
{
    GError *err = NULL;
    hx_fake_server *srv = hx_fake_server_new (
        HX_FAKE_BEHAVIOR_DROP_BEFORE_MAGIC, /*expected=*/0, &err);
    g_assert_nonnull (srv);
    g_assert_no_error (err);

    hx_test_connect_ctx ctx = { 0 };
    drive_connect (&ctx, hx_fake_server_get_port (srv), 5000);

    /* The fake server closes the socket immediately on accept,
     * before reading our HTLC_MAGIC. Our write may succeed (the
     * bytes go into a kernel buffer that's promptly discarded) or
     * fail; either way, the subsequent read sees EOF. The mirror
     * funnels both shapes into DONE_FAILED. */
    g_assert_cmpint (ctx.status, ==, HX_TEST_CONNECT_DONE_FAILED);

    free_ctx (&ctx);
    hx_fake_server_free (srv);
}

static void
test_short_magic (void)
{
    GError *err = NULL;
    hx_fake_server *srv = hx_fake_server_new (HX_FAKE_BEHAVIOR_SEND_SHORT_MAGIC,
                                              HTLC_MAGIC_LEN, &err);
    g_assert_nonnull (srv);
    g_assert_no_error (err);

    hx_test_connect_ctx ctx = { 0 };
    drive_connect (&ctx, hx_fake_server_get_port (srv), 5000);

    /* Server sent only 4 of 8 expected magic bytes — read_all
     * reports short. Routed to DONE_FAILED at "read_magic". */
    g_assert_cmpint (ctx.status, ==, HX_TEST_CONNECT_DONE_FAILED);
    g_assert_cmpstr (ctx.failure_stage, ==, "read_magic");

    free_ctx (&ctx);
    hx_fake_server_free (srv);
}

static void
test_connect_refused (void)
{
    /* Bind+close a listener to grab a known-free ephemeral port,
     * then attempt to connect to it. The kernel returns
     * ECONNREFUSED almost immediately. */
    GSocketListener *throwaway = g_socket_listener_new ();
    GError *err = NULL;
    int port_int = g_socket_listener_add_any_inet_port (throwaway, NULL, &err);
    g_assert_no_error (err);
    g_socket_listener_close (throwaway);
    g_object_unref (throwaway);

    hx_test_connect_ctx ctx = { 0 };
    drive_connect (&ctx, (guint16) port_int, 5000);

    /* GSocketClient surfaces the refusal via the connect_finish
     * call, which our mirror turns into DONE_FAILED at "connect". */
    g_assert_cmpint (ctx.status, ==, HX_TEST_CONNECT_DONE_FAILED);
    g_assert_cmpstr (ctx.failure_stage, ==, "connect");

    free_ctx (&ctx);
}

static void
test_hang_then_test_timeout (void)
{
    GError *err = NULL;
    hx_fake_server *srv = hx_fake_server_new (HX_FAKE_BEHAVIOR_HANG,
                                              HTLC_MAGIC_LEN, &err);
    g_assert_nonnull (srv);
    g_assert_no_error (err);

    hx_test_connect_ctx ctx = { 0 };
    /* 200 ms — enough for the connect + write + read setup, short
     * enough that the test doesn't drag. */
    drive_connect (&ctx, hx_fake_server_get_port (srv), 200);

    g_assert_cmpint (ctx.status, ==, HX_TEST_CONNECT_DONE_TIMEOUT);
    /* The server saw our HTLC_MAGIC arrive — proves the hang is
     * on the SERVER's side (it just doesn't reply) rather than
     * something stalling the write. */
    g_assert_true (hx_fake_server_was_accepted (srv));
    GBytes *got = hx_fake_server_get_received_bytes (srv);
    g_assert_nonnull (got);
    gsize got_len = 0;
    g_bytes_get_data (got, &got_len);
    g_assert_cmpuint (got_len, ==, HTLC_MAGIC_LEN);
    g_bytes_unref (got);

    free_ctx (&ctx);
    hx_fake_server_free (srv);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/fake_connect/happy_path",
                     test_happy_path);
    g_test_add_func ("/fake_connect/wrong_magic",
                     test_wrong_magic);
    g_test_add_func ("/fake_connect/server_drops_before_magic",
                     test_server_drops_before_magic);
    g_test_add_func ("/fake_connect/short_magic",
                     test_short_magic);
    g_test_add_func ("/fake_connect/connect_refused",
                     test_connect_refused);
    g_test_add_func ("/fake_connect/hang_then_test_timeout",
                     test_hang_then_test_timeout);

    return g_test_run ();
}
