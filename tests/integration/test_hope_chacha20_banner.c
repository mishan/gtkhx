/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_hope_chacha20_banner.c — Tier 3 end-to-end
 * test for the file-mode banner HTXF subchannel under HOPE-Secure-
 * Login + ChaCha20-Poly1305 AEAD framing.
 *
 * What the test exercises end-to-end:
 *
 *   1. HOPE handshake → AEAD-active control channel.
 *   2. Drain post-login messages until HTLS_HDR_BANNER arrives, then
 *      remember the binary TYPE (banner is in file mode if the
 *      server sent a binary type like "JPEG" / "GIFf").
 *   3. Send HTLC_HDR_DOWNLOAD_BANNER via integration_send_message
 *      _hope (AEAD-sealed on the wire).
 *   4. Drain to the TASK reply (AEAD-opened); parse HTLS_DATA_HTXF
 *      _REF and HTLS_DATA_HTXF_SIZE.
 *   5. Open the HTXF subchannel TCP socket.
 *   6. Set up a per-transfer AEAD pair via cipher_aead_derive
 *      _transfer_keys, mixing the ref into the derivation just like
 *      production's htxf_connect path does for regular file
 *      transfers.
 *   7. Write the 16-byte HTXF preamble via htxf_io_write (sealed
 *      as one AEAD frame on the wire).
 *   8. Read `size` body bytes via htxf_io_read (consumes AEAD
 *      frames off the socket and reassembles plaintext).
 *   9. Assert the body starts with the magic bytes for the declared
 *      image type.
 *
 * This is the wire shape Janus expects after a HOPE+ChaCha20
 * handshake — the same shape production gtkhx's xfers.c follows for
 * regular file downloads. The bug Misha reported ("banner doesn't
 * fetch with ChaCha20") manifests because production's banner.c
 * uses raw read()/write() on the HTXF subchannel instead of
 * htxf_io_read / htxf_io_write, so the AEAD frames the server
 * sends are read as raw image bytes and the JPEG/GIF magic doesn't
 * match.
 *
 * The matrix filter requires both HX_TEST_CAP_CHACHA20 and
 * HX_TEST_CAP_BANNER_HTXF (file-mode banner). Today that's Janus
 * with BANNER_MODE serving a binary type. Skips silently otherwise.
 */

#include "config.h"
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <glib.h>
#include "compat.h"
#include "hotline.h"
#include "protocol.h"
#include "proto_helpers.h"
#include "integration_harness.h"
#include "server_matrix.h"
#include "cipher_aead.h"
#include "htxf_io.h"
#include "debug.h"

static const hx_test_server *
pick_banner_chacha20_server (void)
{
    GPtrArray *servers = hx_test_servers_with (HX_TEST_CAP_CHACHA20
                                               | HX_TEST_CAP_BANNER_HTXF);
    if (!servers) {
        return NULL;
    }
    const hx_test_server *srv = NULL;
    if (servers->len > 0) {
        srv = g_ptr_array_index (servers, 0);
    }
    g_ptr_array_unref (servers);
    return srv;
}

/* Validate that a buffer starts with the magic bytes for the
 * declared banner image type. Mirrors the same check
 * test_banner.c::banner_bytes_match_type does for the cleartext
 * banner test — if the AEAD-framed body decoded correctly, the
 * first 4 bytes will be the image-format signature; if AEAD got
 * skipped or desynced, they'll be the 4-byte length prefix of an
 * AEAD frame and the magic check will fail. */
static gboolean
banner_bytes_match_type (const char *type, const guint8 *bytes, gsize len)
{
    if (!type || !bytes || len < 4) {
        return FALSE;
    }
    if (strcmp (type, "JPEG") == 0) {
        return bytes[0] == 0xff && bytes[1] == 0xd8;
    }
    if (strcmp (type, "GIFf") == 0 || strcmp (type, "GIF ") == 0) {
        return bytes[0] == 'G' && bytes[1] == 'I' && bytes[2] == 'F'
               && bytes[3] == '8';
    }
    if (strcmp (type, "PNG ") == 0) {
        return bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N'
               && bytes[3] == 'G';
    }
    /* Unknown / PICT etc: accept rather than fail on an unfamiliar
     * magic — keeps the test forward-compatible with new image
     * types. */
    return TRUE;
}

static void
test_hope_chacha20_banner_htxf (void)
{
    const hx_test_server *srv = pick_banner_chacha20_server ();
    if (!srv) {
        g_test_skip ("no server in matrix advertising both "
                     "HX_TEST_CAP_CHACHA20 and HX_TEST_CAP_BANNER_HTXF. "
                     "Janus advertises both — bring it up with "
                     "`docker run -p 5510:5500 -p 5511:5501 gtkhx-janus`.");
        return;
    }

    struct htlc_conn htlc;
    integration_hope_session hope;
    int fd = integration_open_login_hope_or_skip (
        srv, &htlc, &hope,
        /*username=*/"guest", /*password=*/"",
        /*display_name=*/"HopeChaChaBanner Tier-3",
        /*icon=*/412,
        /*cipheralg=*/"CHACHA20-POLY1305",
        /*compressalg=*/NULL);
    if (fd < 0) {
        return;
    }
    g_assert_true (hope.aead_active);

    /* Janus (and any 1.5-spec-compliant server) only fires
     * HTLS_HDR_BANNER after the client sends AGREEMENTAGREE — the
     * post-login push sequence is gated on the "we're fully
     * joined" boundary. integration_open_login_hope_or_skip stops
     * at SELFINFO, so we have to drive the AGREE step ourselves
     * before the drain loop below. Mirrors production's
     * gtkhx.c::concurrence path that calls hx_send_agreement_agree
     * when the user clicks Agree on the agreement window. */
    g_assert_true (integration_send_agreementagree_hope (
        fd, &htlc, &hope, /*display_name=*/"HopeChaChaBanner Tier-3",
        /*icon=*/412));

    /* Drain a few messages waiting for HTLS_HDR_BANNER. */
    gchar *banner_type = NULL;
    for (int i = 0; i < 8 && !banner_type; i++) {
        if (!integration_recv_message_hope (fd, &htlc, &hope,
                                            /*timeout_ms=*/2000)) {
            break;
        }
        if (hdr_type (&htlc) != HTLS_HDR_BANNER) {
            continue;
        }
        dh_start (&htlc)
        {
            if (_type == HTLS_DATA_BANNER_TYPE) {
                banner_type = g_strndup ((const char *) dh->data, _len);
            }
        }
        dh_end ();
    }
    if (!banner_type) {
        g_test_skip ("server did not send HTLS_HDR_BANNER in the "
                     "post-login window — this target may not advertise "
                     "a banner under HOPE.");
        goto cleanup;
    }
    g_test_message ("banner type=\"%s\"", banner_type);

    /* If the server is in URL mode (TYPE=\"URL \"), there's nothing
     * to fetch over HTXF — that test is covered by URL-mode tests. */
    if (strncmp (banner_type, "URL", 3) == 0) {
        g_test_skip ("server is in URL banner mode — HTXF fetch path "
                     "doesn't apply.");
        goto cleanup;
    }

    /* Send HTLC_HDR_DOWNLOAD_BANNER through the AEAD control. */
    guint32 our_trans = htlc.trans;
    g_assert_true (integration_send_message_hope (
        fd, &htlc, &hope, HTLC_HDR_DOWNLOAD_BANNER, /*flag=*/0, /*hc=*/0));

    /* Drain to the TASK reply for our trans. */
    guint32 ref = 0, size = 0;
    gboolean got_reply = FALSE;
    for (int i = 0; i < 16 && !got_reply; i++) {
        if (!integration_recv_message_hope (fd, &htlc, &hope,
                                            /*timeout_ms=*/3000)) {
            break;
        }
        if (hdr_type (&htlc) != HTLS_HDR_TASK
            || hdr_trans (&htlc) != our_trans) {
            continue;
        }
        got_reply = TRUE;
        dh_start (&htlc)
        {
            switch (_type) {
            case HTLS_DATA_HTXF_REF:
                dh_getint (ref);
                break;
            case HTLS_DATA_HTXF_SIZE:
                dh_getint (size);
                break;
            }
        }
        dh_end ();
    }
    g_assert_true (got_reply);
    g_assert_cmpuint (ref, >, 0);
    g_assert_cmpuint (size, >, 0);
    g_assert_cmpuint (size, <, 1u << 20);
    g_test_message ("HTXF ref=%u size=%u", ref, size);

    /* Open HTXF subchannel. The 16-byte preamble travels PLAINTEXT
     * on the wire so the server can match this subchannel against
     * the queued transfer by ref before any cipher state is
     * available (matches production network.c::htxf_connect).
     *
     * Connect to THIS test's chosen server (`srv`) rather than the
     * matrix default — when both mhxd and janus are in the matrix
     * the chacha20 banner test routes to janus for login but the
     * default xfer port can still be mhxd's, opening the subchannel
     * against the wrong server. */
    int xfer_fd = hx_integration_connect_to (srv->host, srv->xfer_port,
                                              /*timeout_ms=*/2000);
    g_assert_cmpint (xfer_fd, >=, 0);

    guint8 hdr_buf[SIZEOF_HTXF_HDR];
    hl_htxf_hdr_pack (hdr_buf, ref, size, HTXF_TYPE_BANNER, 0);
    g_assert_true (integration_send (xfer_fd, hdr_buf, sizeof (hdr_buf)));

    /* Now arm the per-transfer AEAD state for the body. Derivation
     * mixes the ref into the salt so each subchannel gets its own
     * key pair. */
    struct htxf_conn xfer;
    memset (&xfer, 0, sizeof (xfer));
    xfer.ref = ref;
    htxf_io_init (&xfer);
    cipher_aead_derive_transfer_keys (
        &xfer.xfer_encode, &xfer.xfer_decode,
        htlc.sessionkey, htlc.sklen,
        &hope.encode_state, &hope.decode_state,
        ref);
    xfer.aead_active = TRUE;

    /* Read `size` body bytes through htxf_io_read — consumes AEAD
     * frames off the socket and reassembles the plaintext payload. */
    guint8 *bytes = g_malloc (size);
    gsize got = 0;
    while (got < size) {
        ssize_t r = htxf_io_read (&xfer, xfer_fd, bytes + got, size - got);
        if (r <= 0) {
            g_test_message ("htxf_io_read returned %zd at got=%zu errno=%d "
                            "(%s)",
                            r, got, errno, g_strerror (errno));
            break;
        }
        got += (gsize) r;
    }
    g_assert_cmpuint ((guint) got, ==, size);
    integration_close (xfer_fd);
    htxf_io_release (&xfer);

    g_test_message ("first 4 bytes: %02x %02x %02x %02x",
                    bytes[0], bytes[1], bytes[2], bytes[3]);
    g_assert_true (banner_bytes_match_type (banner_type, bytes, size));

    g_free (bytes);

cleanup:
    g_free (banner_type);
    integration_release_htlc (&htlc);
    integration_hope_session_release (&hope);
    integration_close (fd);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    /* Wire up the production debug logger so GTKHX_DEBUG=xfer-aead
     * (etc) surfaces internal AEAD framing diagnostics during test
     * runs. Production calls this from gtkhx.c::main; the test main
     * has to call it itself. */
    debug_init ();
    g_test_add_func ("/integration/hope_chacha20/banner/htxf",
                     test_hope_chacha20_banner_htxf);
    return g_test_run ();
}
