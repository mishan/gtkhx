/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * src/connect_magic.h — HTLS server-magic validator.
 *
 * Only the server-side check is exported here; the client-side
 * "what bytes do I send" is the HTLC_MAGIC constant in hotline.h
 * that production hands directly to g_output_stream_write_all_async
 * (no validation step on the way out). Carved out of network.c's
 * on_magic_received callback so Tier 2 tests can pin the HTLS_MAGIC
 * byte comparison without dragging in the rest of the async connect
 * state machine. tests/integration/test_fake_connect.c additionally
 * drives the helper end-to-end against a scriptable in-process
 * GSocketService fake server (tests/integration/fake_server.c).
 *
 * The hotline.h header defines:
 *
 *   HTLC_MAGIC      "TRTPHOTL\0\1\0\2" — 12 bytes, client→server.
 *                   The "TRTP" identifier + "HOTL" sub-protocol
 *                   tag + a 4-byte version (0001) + sub-version
 *                   (0002) suffix.
 *   HTLS_MAGIC      "TRTP\0\0\0\0"     — 8 bytes, server→client.
 *                   Just the "TRTP" identifier + a 4-byte zero
 *                   reserved trailer.
 *
 * The two magics differ in both length and content — the
 * handshake is NOT symmetric. The validator here only checks the
 * HTLS reply; the client always writes its 12-byte HTLC_MAGIC
 * unconditionally.
 */

#ifndef GTKHX_CONNECT_MAGIC_H
#define GTKHX_CONNECT_MAGIC_H

#include <stddef.h>
#include <glib.h>

/* Returns TRUE iff exactly HTLS_MAGIC_LEN bytes of `buf` match
 * the HTLS_MAGIC byte sequence the spec requires. `len` short of
 * HTLS_MAGIC_LEN is FALSE — the production receive path reads
 * exactly that many bytes before calling, so a short reply
 * (server closed the socket mid-handshake) lands here as FALSE
 * and the connect path surfaces an "invalid hotline server"
 * error. */
extern gboolean hx_connect_validate_server_magic (const guint8 *buf,
                                                  gsize len);

#endif /* GTKHX_CONNECT_MAGIC_H */
