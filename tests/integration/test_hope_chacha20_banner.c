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
#include "cipher.h"
#include "htxf_io.h"
#include "htxf_subchannel.h"
#include "debug.h"

/* Build a HOPE AEAD material handle from the legacy harness's own C
 * handshake state (rust/crates/hxnet/src/ffi.rs). HxnetHopeAead +
 * hxnet_hope_aead_free come from htxf_io.h. */
extern HxnetHopeAead *hxnet_hope_aead_from_material (
    const guint8 *session_key, gsize session_key_len,
    const chacha_aead_state *ctrl_encode, const chacha_aead_state *ctrl_decode);

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
        g_test_fail_printf ("no server in matrix advertising both "
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
    /* AEAD negotiation is a harness-crypto fact; under orchestration the
     * production actor (Rust) owns the cipher and the harness hope
     * session stays zeroed — the encrypted round-trip below is the
     * end-to-end proof. */

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
        g_test_fail_printf ("server did not send HTLS_HDR_BANNER in the "
                     "post-login window — this target may not advertise "
                     "a banner under HOPE.");
        goto cleanup;
    }
    g_test_message ("banner type=\"%s\"", banner_type);

    /* If the server is in URL mode (TYPE=\"URL \"), there's nothing
     * to fetch over HTXF — that test is covered by URL-mode tests. */
    if (strncmp (banner_type, "URL", 3) == 0) {
        g_test_fail_printf ("server is in URL banner mode — HTXF fetch path "
                     "doesn't apply.");
        goto cleanup;
    }

    /* Send HTLC_HDR_DOWNLOAD_BANNER through the AEAD control. */
    guint32 our_trans = htlc.trans;
    g_assert_true (integration_send_message_hope (
        fd, &htlc, &hope, HTLC_HDR_DOWNLOAD_BANNER, /*flag=*/0, /*hc=*/0));

    /* Drain to the TASK reply for our trans. Shared chunk-walker
     * in proto_helpers pulls HTXF_REF + HTXF_SIZE. */
    struct hx_htxf_reply reply = { 0 };
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
        hx_htxf_reply_extract (&htlc, &reply);
    }
    g_assert_true (got_reply);
    g_assert_cmpuint (reply.ref, >, 0);
    g_assert_cmpuint (reply.size, >, 0);
    g_assert_cmpuint (reply.size, <, 1u << 20);
    g_test_message ("HTXF ref=%u size=%u", reply.ref, reply.size);
    guint32 ref = reply.ref, size = reply.size;

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

    /* Shared preamble packer mirrors production banner.c +
     * network.c::htxf_connect. Banners are never >4 GiB so size64
     * stays FALSE; the 16-byte legacy variant comes out. hxnet_htxf_open
     * writes it raw before arming AEAD, so we don't send it ourselves. */
    guint8 hdr_buf[HX_HTXF_PREAMBLE_MAX_BYTES];
    size_t hdr_len = hx_htxf_subchannel_pack_preamble (
        hdr_buf, sizeof (hdr_buf),
        ref, size, HTXF_TYPE_BANNER, /*flags=*/0,
        /*size64=*/FALSE);
    g_assert_cmpuint (hdr_len, >, 0);

    /* hxnet_htxf_open derives the per-transfer AEAD keys in-process from
     * an opaque HOPE material handle + ref, then adopts the fd, writes
     * the plaintext preamble, and frames the body AEAD. Under
     * orchestration the harness already seeded htlc.hope_aead from the
     * production actor; on the legacy transport the harness ran its own
     * C handshake, so build a handle from that session state. tls=0:
     * plaintext subchannel (HOPE was negotiated on the control channel). */
    struct htxf_conn xfer;
    memset (&xfer, 0, sizeof (xfer));
    xfer.ref = ref;
    htxf_io_init (&xfer);
    HxnetHopeAead *owned = NULL;
    const HxnetHopeAead *banner_aead = (const HxnetHopeAead *) htlc.hope_aead;
    if (!banner_aead) {
        owned = hxnet_hope_aead_from_material (htlc.sessionkey, htlc.sklen,
                                               &hope.encode_state,
                                               &hope.decode_state);
        banner_aead = owned;
    }
    xfer.hx = hxnet_htxf_open (xfer_fd, /*tls=*/0, /*host=*/NULL, 0,
                              hdr_buf, hdr_len,
                              banner_aead, ref,
                              /*verify_cert=*/NULL, /*user_data=*/NULL);
    g_assert_nonnull (xfer.hx);

    /* Read `size` body bytes through htxf_io_read — the hxnet channel
     * consumes AEAD frames off the socket and reassembles plaintext. */
    guint8 *bytes = g_malloc (size);
    gsize got = 0;
    while (got < size) {
        ssize_t r = htxf_io_read (&xfer, bytes + got, size - got);
        if (r <= 0) {
            g_test_message ("htxf_io_read returned %zd at got=%zu errno=%d "
                            "(%s)",
                            r, got, errno, g_strerror (errno));
            break;
        }
        got += (gsize) r;
    }
    g_assert_cmpuint ((guint) got, ==, size);
    htxf_io_release (&xfer);
    if (owned) {
        hxnet_hope_aead_free (owned);
    }

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
