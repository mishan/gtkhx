/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/fake_server.h — minimal scriptable TCP listener
 * for testing the GtkHx async connect / handshake / transfer flows
 * without depending on a live mhxd or Janus container.
 *
 * The server runs on the GMainLoop's default context — same loop
 * the GSocketClient async callbacks drive on. Bind 127.0.0.1:0
 * for an ephemeral port, register a per-test script telling the
 * server how to behave once a client connects, and then call the
 * production code (or a faithful mirror — see test_fake_connect.c)
 * pointing at the listener's port.
 *
 * Failure modes the script can exercise:
 *
 *   HX_FAKE_BEHAVIOR_SEND_MAGIC        — happy path: accept,
 *                                         read N bytes from client,
 *                                         send HTLS_MAGIC back.
 *   HX_FAKE_BEHAVIOR_SEND_WRONG_MAGIC  — accept, read N bytes,
 *                                         send 8 bytes of garbage.
 *                                         Client should reject the
 *                                         handshake.
 *   HX_FAKE_BEHAVIOR_DROP_BEFORE_MAGIC — accept, close immediately.
 *                                         Client's read-magic call
 *                                         hits EOF.
 *   HX_FAKE_BEHAVIOR_SEND_SHORT_MAGIC  — accept, read N bytes, send
 *                                         only the first 4 bytes of
 *                                         HTLS_MAGIC, then close.
 *                                         Client's
 *                                         read_all_async-of-8 call
 *                                         either errors or returns
 *                                         a short read.
 *   HX_FAKE_BEHAVIOR_HANG              — accept, read N bytes, then
 *                                         do nothing. Client's read
 *                                         hangs; the test verifies
 *                                         the production
 *                                         MAGIC_TIMEOUT_SEC fires
 *                                         (or simulates it with a
 *                                         shorter test-only
 *                                         cancellable).
 *
 * After the connection completes (or fails), the test can inspect
 * hx_fake_server_get_received_bytes() to see exactly what the
 * client sent before reaching whatever state we're testing. This
 * is how we pin "the production code wrote HTLC_MAGIC first" without
 * having to introduce a hook into network.c.
 */

#ifndef GTKHX_FAKE_SERVER_H
#define GTKHX_FAKE_SERVER_H

#include <glib.h>
#include <gio/gio.h>

typedef enum {
    HX_FAKE_BEHAVIOR_SEND_MAGIC = 0,
    HX_FAKE_BEHAVIOR_SEND_WRONG_MAGIC = 1,
    HX_FAKE_BEHAVIOR_DROP_BEFORE_MAGIC = 2,
    HX_FAKE_BEHAVIOR_SEND_SHORT_MAGIC = 3,
    HX_FAKE_BEHAVIOR_HANG = 4,
} hx_fake_server_behavior;

typedef struct hx_fake_server hx_fake_server;

/* Create a fake server bound to 127.0.0.1 on an ephemeral port,
 * listening on the default GMainContext. The `expected_client_bytes`
 * field tells the server how many bytes to consume from the
 * client before reacting per `behavior` — typically HTLC_MAGIC_LEN
 * (12). On error returns NULL and sets *err. */
extern hx_fake_server *hx_fake_server_new (hx_fake_server_behavior behavior,
                                           gsize expected_client_bytes,
                                           GError **err);

/* Returns the host-byte-order port the server is listening on. */
extern guint16 hx_fake_server_get_port (hx_fake_server *srv);

/* Returns the bytes the server has received from the client since
 * accept, as a GBytes (caller owns). NULL if no connection has
 * arrived yet. */
extern GBytes *hx_fake_server_get_received_bytes (hx_fake_server *srv);

/* TRUE if a client has connected to the listener. */
extern gboolean hx_fake_server_was_accepted (hx_fake_server *srv);

/* Tear down the listener, cancel any in-flight async reads/writes
 * on the accepted connection, free the bytes accumulator. Safe to
 * call before or after a client connects. */
extern void hx_fake_server_free (hx_fake_server *srv);

#endif /* GTKHX_FAKE_SERVER_H */
