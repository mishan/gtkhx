/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_hope_blowfish_banner.c — Tier 3 end-to-end
 * test for the file-mode banner HTXF subchannel under HOPE+Blowfish
 * OFB-64 stream-cipher framing.
 *
 * Sister to test_hope_rc4_banner.c — see that file for the full
 * preamble. Only differences here:
 *
 *   - cipheralg = "BLOWFISH"
 *   - HX_TEST_CAP_BLOWFISH filter
 *
 * Keeping the two as separate binaries (rather than one
 * parameterised test) so individual failures bisect cleanly: a
 * green RC4 test and a red Blowfish test narrows a bug to the
 * Blowfish OFB-64 state machine without further diagnosis. Same
 * convention as test_hope_blowfish / test_hope_rc4 (post-login
 * PING coverage).
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

static const hx_test_server *
pick_banner_blowfish_server (void)
{
    GPtrArray *servers = hx_test_servers_with (HX_TEST_CAP_BLOWFISH
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
    return TRUE;
}

static void
test_hope_blowfish_banner_htxf (void)
{
    const hx_test_server *srv = pick_banner_blowfish_server ();
    if (!srv) {
        g_test_skip ("no server in matrix advertising both "
                     "HX_TEST_CAP_BLOWFISH and HX_TEST_CAP_BANNER_HTXF. "
                     "mhxd advertises both — bring it up with "
                     "BANNER_MODE=JPEG (or GIFf / PNG).");
        return;
    }

    struct htlc_conn htlc;
    integration_hope_session hope;
    int fd = integration_open_login_hope_or_skip (
        srv, &htlc, &hope,
        /*username=*/"guest", /*password=*/"",
        /*display_name=*/"HopeBFBanner Tier-3",
        /*icon=*/412,
        /*cipheralg=*/"BLOWFISH",
        /*compressalg=*/NULL);
    if (fd < 0) {
        return;
    }
    g_assert_true (hope.stream_active);
    g_assert_false (hope.aead_active);

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
                     "post-login window.");
        goto cleanup;
    }
    g_test_message ("banner type=\"%s\"", banner_type);

    if (strncmp (banner_type, "URL", 3) == 0) {
        g_test_skip ("server is in URL banner mode — HTXF fetch path "
                     "doesn't apply.");
        goto cleanup;
    }

    guint32 our_trans = htlc.trans;
    g_assert_true (integration_send_message_hope (
        fd, &htlc, &hope, HTLC_HDR_DOWNLOAD_BANNER, /*flag=*/0, /*hc=*/0));

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
    /* >= 4 so the 4-byte magic log + matcher below can't run off the
     * end of the buffer if a misbehaving server reports a tiny size. */
    g_assert_cmpuint (reply.size, >=, 4);
    g_assert_cmpuint (reply.size, <, 1u << 20);
    g_test_message ("HTXF ref=%u size=%u", reply.ref, reply.size);
    guint32 ref = reply.ref, size = reply.size;

    int xfer_fd = integration_connect_xfer ();
    g_assert_cmpint (xfer_fd, >=, 0);
    g_assert_true (integration_send_xfer_hdr (xfer_fd, ref, size));

    guint8 *bytes = g_malloc (size);
    g_assert_true (integration_recv (xfer_fd, bytes, size));
    integration_close (xfer_fd);

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
    g_test_add_func ("/integration/hope_blowfish/banner/htxf",
                     test_hope_blowfish_banner_htxf);
    return g_test_run ();
}
