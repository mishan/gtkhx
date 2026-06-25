/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#include "config.h"

#include <string.h>
#include <glib.h>
#include <gio/gio.h>

#include "compat.h"          /* PACKED — hotline.h's struct attrs */
#include "hotline.h"         /* HTLS_MAGIC, HTLS_MAGIC_LEN */
#include "fake_server.h"

struct hx_fake_server {
    GSocketService *service;
    GSocketListener *listener;
    guint16 port;

    hx_fake_server_behavior behavior;
    gsize expected_client_bytes;

    /* Set when a client connects. The bytes received from the
     * client accumulate here so the test can inspect what the
     * production code (or its mirror) sent during the handshake. */
    gboolean accepted;
    GByteArray *received;
    guint8 read_scratch[64]; /* enough for HTLC_MAGIC (12) + slack */
    GCancellable *cancel;
    GSocketConnection *active;
};

/* ---- write-side handlers ------------------------------------- */

static void
on_write_done (GObject *src, GAsyncResult *res, gpointer u)
{
    hx_fake_server *srv = u;
    GError *err = NULL;
    g_output_stream_write_all_finish (G_OUTPUT_STREAM (src), res, NULL, &err);
    g_clear_error (&err);
    /* Drop the connection after the scripted write — the test
     * client decides whether to keep the connection open. We just
     * want clean teardown on our side. */
    if (srv->active) {
        g_io_stream_close (G_IO_STREAM (srv->active), NULL, NULL);
    }
}

static void
react_after_read (hx_fake_server *srv)
{
    if (!srv->active) {
        return;
    }
    GOutputStream *out = g_io_stream_get_output_stream (G_IO_STREAM (srv->active));

    switch (srv->behavior) {
    case HX_FAKE_BEHAVIOR_SEND_MAGIC: {
        /* Happy path: send the real HTLS_MAGIC. */
        g_output_stream_write_all_async (out, HTLS_MAGIC, HTLS_MAGIC_LEN,
                                         G_PRIORITY_DEFAULT, srv->cancel,
                                         on_write_done, srv);
        break;
    }
    case HX_FAKE_BEHAVIOR_SEND_WRONG_MAGIC: {
        /* 8 bytes that aren't HTLS_MAGIC — first byte matches
         * 'T' so a TCP-level-noise classifier would pass but the
         * orchestrator's full-length magic compare (hxnet's
         * magic.rs) catches the mismatch. */
        static const guint8 garbage[8]
            = { 'T', 'R', 'O', 'L', 'L', 0xff, 0xfe, 0xfd };
        g_output_stream_write_all_async (out, garbage, sizeof (garbage),
                                         G_PRIORITY_DEFAULT, srv->cancel,
                                         on_write_done, srv);
        break;
    }
    case HX_FAKE_BEHAVIOR_SEND_SHORT_MAGIC: {
        /* Only 4 bytes — client's read-exactly-8 call sees a
         * partial read and either errors or returns short. */
        g_output_stream_write_all_async (out, "TRTP", 4, G_PRIORITY_DEFAULT,
                                         srv->cancel, on_write_done, srv);
        break;
    }
    case HX_FAKE_BEHAVIOR_DROP_BEFORE_MAGIC: {
        /* Close without writing anything — client read sees EOF. */
        g_io_stream_close (G_IO_STREAM (srv->active), NULL, NULL);
        break;
    }
    case HX_FAKE_BEHAVIOR_HANG: {
        /* Do absolutely nothing — client hangs on the read until
         * its own timeout fires. */
        break;
    }
    }
}

/* ---- read-side handler --------------------------------------- */

static void
on_read_done (GObject *src, GAsyncResult *res, gpointer u)
{
    hx_fake_server *srv = u;
    GError *err = NULL;
    gsize got = 0;
    /* Capture the finish boolean explicitly — a read can return
     * `got == expected_client_bytes` AND report failure (cancelled,
     * EOF synthesised by GIO with the partial byte count, etc.).
     * Gating react_after_read on a clean finish keeps the test rig
     * deterministic: a failed read never triggers a scripted server
     * reply. */
    gboolean ok = g_input_stream_read_all_finish (G_INPUT_STREAM (src), res,
                                                  &got, &err);
    if (got > 0) {
        g_byte_array_append (srv->received, srv->read_scratch, got);
    }
    g_clear_error (&err);

    if (ok && got == srv->expected_client_bytes) {
        react_after_read (srv);
    } else {
        /* Short read, cancellation, or other failure — client
         * closed or errored. Nothing to react with; the test will
         * discover this via was_accepted + get_received_bytes. */
        if (srv->active) {
            g_io_stream_close (G_IO_STREAM (srv->active), NULL, NULL);
        }
    }
}

/* ---- accept handler ------------------------------------------ */

static gboolean
on_incoming (GSocketService *service, GSocketConnection *conn,
             GObject *source, gpointer u)
{
    hx_fake_server *srv = u;
    (void) service;
    (void) source;

    /* Single-client only. The test rig always drives one
     * GSocketClient at a time; a second accept means the test
     * environment did something unexpected (a stray retry, a
     * second async connect we didn't realise was in flight, a
     * port-scan). Reject by closing the second connection
     * without disturbing the first one's state — get_received
     * _bytes / was_accepted still reflect the original client. */
    if (srv->active) {
        g_io_stream_close (G_IO_STREAM (conn), NULL, NULL);
        return TRUE;
    }

    srv->accepted = TRUE;
    srv->active = g_object_ref (conn);

    /* Kick off the read for `expected_client_bytes` bytes. The
     * production hx_connect writes HTLC_MAGIC (12 bytes) right
     * after the TCP connect succeeds — test passes 12 to consume
     * that. expected_client_bytes is bounds-checked at constructor
     * time against sizeof(read_scratch), so this read is safe. */
    if (srv->expected_client_bytes == 0) {
        react_after_read (srv);
    } else {
        GInputStream *in = g_io_stream_get_input_stream (G_IO_STREAM (conn));
        g_input_stream_read_all_async (in, srv->read_scratch,
                                       srv->expected_client_bytes,
                                       G_PRIORITY_DEFAULT, srv->cancel,
                                       on_read_done, srv);
    }

    /* Returning TRUE keeps the service from closing the
     * connection itself — we own it via srv->active. */
    return TRUE;
}

/* ---- public API ---------------------------------------------- */

hx_fake_server *
hx_fake_server_new (hx_fake_server_behavior behavior,
                    gsize expected_client_bytes, GError **err)
{
    /* read_scratch is a fixed 64-byte buffer sized for HTLC_MAGIC
     * (12 bytes) plus comfortable slack. Reject anything that
     * would overflow it — the test rig is supposed to ask for
     * fixed-size client frames, not arbitrary payloads. A future
     * test that needs a bigger handshake should grow the scratch
     * buffer deliberately, not have the kernel write past the end
     * of the array. */
    hx_fake_server scratch_probe;
    gsize scratch_cap = sizeof (scratch_probe.read_scratch);
    if (expected_client_bytes > scratch_cap) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                     "hx_fake_server: expected_client_bytes=%zu exceeds "
                     "scratch buffer (%zu); grow read_scratch[] first",
                     expected_client_bytes, scratch_cap);
        return NULL;
    }

    hx_fake_server *srv = g_new0 (hx_fake_server, 1);
    srv->behavior = behavior;
    srv->expected_client_bytes = expected_client_bytes;
    srv->received = g_byte_array_new ();
    srv->cancel = g_cancellable_new ();

    srv->service = g_socket_service_new ();
    srv->listener = G_SOCKET_LISTENER (srv->service);

    /* Bind 127.0.0.1:0 → kernel picks an ephemeral port. We don't
     * use g_socket_listener_add_inet_port because we need the
     * actual port back, and add_any_inet_port gives us that. */
    GError *local_err = NULL;
    srv->port = (guint16)
        g_socket_listener_add_any_inet_port (srv->listener, NULL, &local_err);
    if (srv->port == 0) {
        if (err) {
            *err = local_err;
        } else {
            g_clear_error (&local_err);
        }
        hx_fake_server_free (srv);
        return NULL;
    }

    g_signal_connect (srv->service, "incoming", G_CALLBACK (on_incoming), srv);
    g_socket_service_start (srv->service);
    return srv;
}

guint16
hx_fake_server_get_port (hx_fake_server *srv)
{
    return srv ? srv->port : 0;
}

GBytes *
hx_fake_server_get_received_bytes (hx_fake_server *srv)
{
    if (!srv || srv->received->len == 0) {
        return NULL;
    }
    return g_bytes_new (srv->received->data, srv->received->len);
}

gboolean
hx_fake_server_was_accepted (hx_fake_server *srv)
{
    return srv ? srv->accepted : FALSE;
}

void
hx_fake_server_free (hx_fake_server *srv)
{
    if (!srv) {
        return;
    }
    if (srv->cancel) {
        g_cancellable_cancel (srv->cancel);
        g_clear_object (&srv->cancel);
    }
    if (srv->service) {
        g_socket_service_stop (srv->service);
        g_clear_object (&srv->service);
    }
    if (srv->active) {
        g_io_stream_close (G_IO_STREAM (srv->active), NULL, NULL);
        g_clear_object (&srv->active);
    }
    if (srv->received) {
        g_byte_array_unref (srv->received);
    }
    g_free (srv);
}
