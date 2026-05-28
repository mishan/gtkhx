/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * GIOStream-based TLS variant of the Tier 3 harness — see
 * integration_tls.h for the rationale.
 *
 * Each `_stream` helper here mirrors its fd-based sibling in
 * integration_harness.c almost line for line; the only difference
 * is the transport. When the fd-based API eventually gets
 * collapsed into a unified hx_io abstraction, these implementations
 * collapse to one-liners that delegate. Until then the duplication
 * is the conscious trade-off (small blast radius vs. perfectly DRY).
 */

#include "config.h"

#include <stdarg.h>
#include <string.h>
#include <glib.h>
#include <gio/gio.h>

#include "compat.h"
#include "hotline.h"
#include "protocol.h"
#include "proto_helpers.h"
#include "login_packet.h"
#include "integration_harness.h" /* hdr_type/hdr_flag inlines + release helper */
#include "integration_tls.h"

/* ---- Accept-everything cert handler ----------------------------- */
/*
 * Same shape as production hx_connect Phase 1's
 * tls_accept_certificate_phase1_stub (see src/network.c). Returns
 * TRUE for any cert; the Janus container ships a self-signed
 * CN=localhost cert that GnuTLS would otherwise refuse. Phase 3
 * replaces both this stub and the production one with a real TOFU
 * trust DB.
 */
static gboolean
test_tls_accept_certificate (GTlsConnection *conn G_GNUC_UNUSED,
                             GTlsCertificate *peer_cert G_GNUC_UNUSED,
                             GTlsCertificateFlags errors G_GNUC_UNUSED,
                             gpointer user_data G_GNUC_UNUSED)
{
    return TRUE;
}

/* GSocketClient::event fires through every phase of the connect.
 * The TLS_HANDSHAKING phase is the only one where `connection` is a
 * GTlsClientConnection we can attach accept-certificate to. Mirrors
 * src/network.c::on_socket_client_event. */
static void
on_test_socket_client_event (GSocketClient *client G_GNUC_UNUSED,
                             GSocketClientEvent event,
                             GSocketConnectable *connectable G_GNUC_UNUSED,
                             GIOStream *connection,
                             gpointer user_data G_GNUC_UNUSED)
{
    if (event != G_SOCKET_CLIENT_TLS_HANDSHAKING) {
        return;
    }
    if (!connection || !G_IS_TLS_CONNECTION (connection)) {
        return;
    }
    g_signal_connect (connection, "accept-certificate",
                      G_CALLBACK (test_tls_accept_certificate), NULL);
}

/* ---- Underlying socket access ----------------------------------- */
/*
 * Walk through a GTlsClientConnection's base IO stream to reach the
 * underlying GSocket, which we set timeouts on so blocking reads
 * give up cleanly after `timeout_ms`. Returns NULL if the stream
 * shape isn't what we expect (shouldn't happen for our connect path
 * but worth defending against). */
static GSocket *
stream_underlying_socket (GIOStream *io)
{
    if (!io) {
        return NULL;
    }
    if (G_IS_SOCKET_CONNECTION (io)) {
        return g_socket_connection_get_socket (G_SOCKET_CONNECTION (io));
    }
    if (G_IS_TLS_CONNECTION (io)) {
        /* g_tls_connection_get_base_io_stream isn't ABI-stable
         * across the GLib versions we still build against; pull the
         * base stream via the property accessor instead, which is
         * always available. The returned GIOStream is owned by the
         * GTlsConnection — no extra ref management beyond the
         * borrowed-pointer scope below. */
        GIOStream *base = NULL;
        g_object_get (io, "base-io-stream", &base, NULL);
        if (base) {
            GSocket *sock = NULL;
            if (G_IS_SOCKET_CONNECTION (base)) {
                sock = g_socket_connection_get_socket (
                    G_SOCKET_CONNECTION (base));
            }
            /* g_object_get gave us a new ref via the property API;
             * drop it. The socket pointer stays valid because the
             * GSocketConnection still owns it. */
            g_object_unref (base);
            return sock;
        }
    }
    return NULL;
}

/* ---- Connect / close -------------------------------------------- */

GIOStream *
hx_test_server_connect_tls (const hx_test_server *srv)
{
    g_return_val_if_fail (srv != NULL, NULL);
    g_return_val_if_fail (srv->tls_port != 0, NULL);

    GSocketClient *client = g_socket_client_new ();
    g_socket_client_set_tls (client, TRUE);
    g_socket_client_set_timeout (client, /*seconds=*/5);
    g_signal_connect (client, "event",
                      G_CALLBACK (on_test_socket_client_event), NULL);

    GError *error = NULL;
    GSocketConnection *conn = g_socket_client_connect_to_host (
        client, srv->host, srv->tls_port, /*cancellable=*/NULL, &error);
    g_object_unref (client);

    if (!conn) {
        g_warning ("hx_test_server_connect_tls: %s:%u — %s",
                   srv->host, (unsigned) srv->tls_port,
                   error ? error->message : "(no error)");
        g_clear_error (&error);
        return NULL;
    }

    /* g_socket_client_connect_to_host with set_tls=TRUE returns a
     * GSocketConnection whose superclass is a GTlsClientConnection
     * — the GSocketClient wrapped + handshook for us. The returned
     * GIOStream is read/write-ready. */
    return G_IO_STREAM (conn);
}

void
integration_close_stream (GIOStream *io)
{
    if (!io) {
        return;
    }
    /* g_io_stream_close flushes any pending TLS close-notify
     * alerts. Discard errors — we're tearing down. */
    GError *err = NULL;
    g_io_stream_close (io, NULL, &err);
    g_clear_error (&err);
    g_object_unref (io);
}

/* ---- Low-level I/O ---------------------------------------------- */

gboolean
integration_send_stream (GIOStream *io, const void *buf, gsize len)
{
    g_return_val_if_fail (io != NULL, FALSE);

    GOutputStream *out = g_io_stream_get_output_stream (io);
    gsize written = 0;
    GError *err = NULL;
    gboolean ok = g_output_stream_write_all (out, buf, len, &written,
                                             NULL, &err);
    if (!ok || written != len) {
        if (err) {
            /* g_debug, not g_warning — under g_test_init the latter
             * becomes fatal and would kill drain loops that
             * legitimately expect short failures (timeouts on
             * "drain until X" past available messages). Callers
             * already surface real failures via g_test_fail_printf
             * with the right diagnostic. */
            g_debug ("integration_send_stream: %s", err->message);
        }
        g_clear_error (&err);
        return FALSE;
    }
    return TRUE;
}

gboolean
integration_recv_stream (GIOStream *io, void *buf, gsize len, int timeout_ms)
{
    g_return_val_if_fail (io != NULL, FALSE);

    /* Set a socket-level timeout so blocking reads give up cleanly.
     * 5 s default matches integration_recv's hardcoded select
     * timeout. Stash + restore so this call doesn't leak state to
     * later calls on the same stream. */
    GSocket *sock = stream_underlying_socket (io);
    guint prev_timeout = 0;
    if (sock) {
        prev_timeout = g_socket_get_timeout (sock);
        guint new_timeout = timeout_ms > 0
                                ? (guint) ((timeout_ms + 999) / 1000)
                                : 5;
        g_socket_set_timeout (sock, new_timeout);
    }

    GInputStream *in = g_io_stream_get_input_stream (io);
    gsize got = 0;
    GError *err = NULL;
    gboolean ok = g_input_stream_read_all (in, buf, len, &got, NULL, &err);

    if (sock) {
        g_socket_set_timeout (sock, prev_timeout);
    }

    if (!ok || got != len) {
        if (err) {
            /* g_debug for the same reason send_stream uses it —
             * timeouts under g_test_init's fatal-warning mask
             * would bail out of legitimate drain loops. */
            g_debug ("integration_recv_stream: %s", err->message);
        }
        g_clear_error (&err);
        return FALSE;
    }
    return TRUE;
}

/* ---- Hotline protocol ------------------------------------------- */

gboolean
integration_handshake_stream (GIOStream *io)
{
    if (!integration_send_stream (io, HTLC_MAGIC, HTLC_MAGIC_LEN)) {
        return FALSE;
    }
    guint8 reply[HTLS_MAGIC_LEN];
    if (!integration_recv_stream (io, reply, sizeof (reply), 5000)) {
        return FALSE;
    }
    return memcmp (reply, HTLS_MAGIC, HTLS_MAGIC_LEN) == 0;
}

gboolean
integration_send_message_stream (GIOStream *io, struct htlc_conn *htlc,
                                 guint32 type, guint32 flag, int hc, ...)
{
    /* Same reset-before-pack contract as integration_send_message
     * so successive sends each get a fresh htlc->out buffer. */
    g_free (htlc->out.buf);
    htlc->out.buf = NULL;
    htlc->out.pos = 0;
    htlc->out.len = 0;

    va_list ap;
    va_start (ap, hc);
    hlpack (htlc, type, flag, hc, ap);
    va_end (ap);

    gboolean ok = integration_send_stream (io, htlc->out.buf, htlc->out.len);

    g_free (htlc->out.buf);
    htlc->out.buf = NULL;
    htlc->out.pos = 0;
    htlc->out.len = 0;
    return ok;
}

gboolean
integration_recv_message_stream (GIOStream *io, struct htlc_conn *htlc,
                                 int timeout_ms)
{
    g_free (htlc->in.buf);
    htlc->in.buf = NULL;
    htlc->in.pos = 0;
    htlc->in.len = 0;

    guint8 hdr_bytes[SIZEOF_HL_HDR];
    if (!integration_recv_stream (io, hdr_bytes, SIZEOF_HL_HDR, timeout_ms)) {
        return FALSE;
    }

    guint32 wire_len = 0, body_len = 0;
    if (!hl_hdr_decode (hdr_bytes, NULL, NULL, NULL, NULL, &wire_len,
                        &body_len)) {
        return FALSE;
    }
    if (wire_len > MAX_HOTLINE_PACKET_LEN) {
        return FALSE;
    }

    gsize total = SIZEOF_HL_HDR + body_len;
    htlc->in.buf = g_malloc (total);
    memcpy (htlc->in.buf, hdr_bytes, SIZEOF_HL_HDR);
    if (body_len > 0) {
        /* Use a generous per-body timeout — the header arrived, the
         * body is on the way; treat this as the same logical read.
         * Mirrors the way integration_recv_message keeps reading
         * after the header without a new select. */
        if (!integration_recv_stream (io, htlc->in.buf + SIZEOF_HL_HDR,
                                      body_len, /*timeout_ms=*/5000)) {
            g_free (htlc->in.buf);
            htlc->in.buf = NULL;
            return FALSE;
        }
    }
    htlc->in.pos = total;
    htlc->in.len = total;
    return TRUE;
}

gboolean
integration_login_guest_stream (GIOStream *io, struct htlc_conn *htlc,
                                const char *display_name, guint16 icon)
{
    /* Same legacy guest LOGIN shape as integration_login_guest —
     * we deliberately send HTLC_DATA_NAME inline so the round-trip
     * assertions match. See the fd-based sibling's docstring. */
    const hx_login_request req = {
        .mode = HX_LOGIN_MODE_LEGACY,
        .icon = icon,
        .login_name = "guest",
        .password = NULL,
        .display_name = display_name,
        .client_version = 185,
        .send_caps = 0,
    };

    g_free (htlc->out.buf);
    htlc->out.buf = NULL;
    htlc->out.pos = 0;
    htlc->out.len = 0;

    struct hx_chunk chunks[HX_LOGIN_MAX_CHUNKS];
    guint8 scratch[HX_LOGIN_SCRATCH_SIZE];
    int hc = hx_login_build_chunks (&req, chunks, HX_LOGIN_MAX_CHUNKS,
                                    scratch, sizeof (scratch));
    if (hc <= 0) {
        return FALSE;
    }
    hlpack_chunks (htlc, HTLC_HDR_LOGIN, 0, chunks, hc);

    gboolean ok = integration_send_stream (io, htlc->out.buf, htlc->out.len);

    g_free (htlc->out.buf);
    htlc->out.buf = NULL;
    htlc->out.pos = 0;
    htlc->out.len = 0;
    return ok;
}

guint32
integration_drain_until_selfinfo_or_error_stream (GIOStream *io,
                                                  struct htlc_conn *htlc,
                                                  int max_messages)
{
    if (max_messages <= 0) {
        max_messages = 8;
    }
    for (int i = 0; i < max_messages; i++) {
        if (!integration_recv_message_stream (io, htlc,
                                              /*timeout_ms=*/3000)) {
            return 0;
        }
        guint32 type = hdr_type (htlc);
        guint32 flag = hdr_flag (htlc);
        if (type == HTLS_HDR_TASK && (flag & 1)) {
            return type;
        }
        if (type == HTLS_HDR_USER_SELFINFO) {
            return type;
        }
    }
    return 0;
}

gboolean
integration_drain_until_type_stream (GIOStream *io, struct htlc_conn *htlc,
                                     guint16 wanted_type, int max_messages)
{
    for (int i = 0; i < max_messages; i++) {
        if (!integration_recv_message_stream (io, htlc,
                                              /*timeout_ms=*/3000)) {
            return FALSE;
        }
        if (hdr_type (htlc) == wanted_type) {
            return TRUE;
        }
    }
    return FALSE;
}

gboolean
integration_send_chat_stream (GIOStream *io, struct htlc_conn *htlc,
                              const char *text)
{
    /* style=1 = plain text; same hardcoding as integration_send_chat. */
    guint16 style = htons (1);
    return integration_send_message_stream (
        io, htlc, HTLC_HDR_CHAT, /*flag=*/0, /*hc=*/2,
        (int) HTLC_DATA_STYLE, (int) sizeof (style), &style,
        (int) HTLC_DATA_CHAT, (int) strlen (text), (guint8 *) text);
}

gboolean
integration_drain_until_chat_stream (GIOStream *io, struct htlc_conn *htlc,
                                     guint16 wanted_uid,
                                     struct hx_chat_msg *out,
                                     int max_messages)
{
    for (int i = 0; i < max_messages; i++) {
        if (!integration_recv_message_stream (io, htlc,
                                              /*timeout_ms=*/3000)) {
            return FALSE;
        }
        if (hdr_type (htlc) != HTLS_HDR_CHAT) {
            continue;
        }
        if (!hx_chat_extract (htlc, out)) {
            continue;
        }
        if (out->uid == wanted_uid) {
            return TRUE;
        }
    }
    return FALSE;
}

gboolean
integration_drain_until_task_trans_stream (GIOStream *io,
                                           struct htlc_conn *htlc,
                                           guint32 wanted_trans,
                                           int max_messages)
{
    for (int i = 0; i < max_messages; i++) {
        if (!integration_recv_message_stream (io, htlc,
                                              /*timeout_ms=*/3000)) {
            return FALSE;
        }
        if (hdr_type (htlc) != HTLS_HDR_TASK) {
            continue;
        }
        if (hdr_trans (htlc) != wanted_trans) {
            continue;
        }
        return TRUE;
    }
    return FALSE;
}

GIOStream *
hx_test_server_connect_xfer_tls (const hx_test_server *srv)
{
    g_return_val_if_fail (srv != NULL, NULL);
    g_return_val_if_fail (srv->tls_xfer_port != 0, NULL);

    GSocketClient *client = g_socket_client_new ();
    g_socket_client_set_tls (client, TRUE);
    g_socket_client_set_timeout (client, /*seconds=*/5);
    g_signal_connect (client, "event",
                      G_CALLBACK (on_test_socket_client_event), NULL);

    GError *error = NULL;
    GSocketConnection *conn = g_socket_client_connect_to_host (
        client, srv->host, srv->tls_xfer_port, /*cancellable=*/NULL, &error);
    g_object_unref (client);

    if (!conn) {
        g_warning ("hx_test_server_connect_xfer_tls: %s:%u — %s",
                   srv->host, (unsigned) srv->tls_xfer_port,
                   error ? error->message : "(no error)");
        g_clear_error (&error);
        return NULL;
    }
    return G_IO_STREAM (conn);
}

GIOStream *
integration_open_login_tls_or_skip (const hx_test_server *srv,
                                    struct htlc_conn *htlc,
                                    const char *display_name, guint16 icon)
{
    memset (htlc, 0, sizeof (*htlc));

    if (!srv || srv->tls_port == 0) {
        g_test_fail_printf (
            "integration_open_login_tls_or_skip: no TLS-capable server. "
            "Run with GTKHX_TEST_SERVERS=janus or start the Janus "
            "container (tests/janus/Dockerfile) with -p 5610:5600 -p "
            "5611:5601.");
        return NULL;
    }

    GIOStream *io = hx_test_server_connect_tls (srv);
    if (!io) {
        g_test_fail_printf (
            "hx_test_server_connect_tls failed for %s:%u (is the "
            "container running with TLS ports mapped?)",
            srv->host, (unsigned) srv->tls_port);
        return NULL;
    }

    if (!integration_handshake_stream (io)) {
        g_test_fail_printf (
            "TLS handshake completed but HTLS magic exchange failed "
            "against %s:%u — is this actually a Hotline server?",
            srv->host, (unsigned) srv->tls_port);
        integration_close_stream (io);
        return NULL;
    }

    if (!integration_login_guest_stream (io, htlc, display_name, icon)) {
        g_test_fail_printf ("integration_login_guest_stream failed");
        integration_close_stream (io);
        return NULL;
    }

    guint32 type = integration_drain_until_selfinfo_or_error_stream (
        io, htlc, 8);

    if (type == HTLS_HDR_TASK) {
        char err[256];
        gsize err_len = 0;
        if (task_error_extract (htlc, err, sizeof (err), &err_len)) {
            g_test_fail_printf ("server rejected guest login: \"%s\"",
                                err);
        } else {
            g_test_fail_printf (
                "server rejected guest login (no error chunk).");
        }
        integration_release_htlc (htlc);
        integration_close_stream (io);
        return NULL;
    }
    if (type != HTLS_HDR_USER_SELFINFO) {
        g_test_fail_printf (
            "timed out waiting for SELFINFO after guest login over TLS.");
        integration_release_htlc (htlc);
        integration_close_stream (io);
        return NULL;
    }

    /* Parse SELFINFO into htlc->uid / icon / access. The display
     * name doesn't get written to htlc->name (Phase 5 policy —
     * see integration_open_login_or_skip's comment), but the
     * uid is what the chat broadcast filter needs. */
    hx_selfinfo_parse (htlc);

    return io;
}
