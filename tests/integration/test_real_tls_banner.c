/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_real_tls_banner.c — Tier 3 coverage for
 * docs/tls.md "The separate-port model": file-mode (HTXF) banner fetch over
 * TLS.
 *
 * Mirror of tests/integration/test_banner.c::test_banner_htxf_mode
 * but with both connections wrapped in TLS. Exercises the same
 * skinny-LOGIN → AGREEMENTAGREE → HTLS_HDR_BANNER → DOWNLOAD_BANNER
 * → HTXF subchannel flow that file-mode banners drive in
 * production, just over Janus's TLS HTLS / TLS HTXF ports.
 *
 * The plaintext sibling already pins the wire-format mechanics
 * (banner type chunk, HTXF preamble, image-magic match). What this
 * test adds: proof that the larger image body (typically tens of
 * KB) round-trips through the TLS-wrapped HTXF subchannel without
 * truncation or scrambling. The image-magic assertion (JPEG/GIF/PNG
 * prefix bytes) catches any silent byte mangling at the TLS layer
 * — a partial decrypt or framing slip would show up as garbled
 * leading bytes.
 *
 * Both legs run through PRODUCTION rustls: the control channel via
 * the orchestrator (integration_open_login_tls_or_skip drives the
 * production login lifecycle, AGREEMENTAGREE included), and the HTXF
 * subchannel via the same hxnet_htxf_connect(tls=1) path banner.c's
 * worker uses — no parallel GnuTLS harness transport.
 */

#include "config.h"

#include <string.h>
#include <unistd.h>
#include <glib.h>
#include <gio/gio.h>

#include "compat.h"
#include "hotline.h"
#include "protocol.h"
#include "proto_helpers.h"
#include "hxnet_htxf.h"
#include "server_matrix.h"
#include "integration_harness.h"

/* Pick a server that advertises BOTH HX_TEST_CAP_TLS and
 * HX_TEST_CAP_BANNER_HTXF — today Janus, the only matrix entry that
 * does both. Same shape as pick_banner_htxf_server in the plaintext
 * test, extended to require TLS too. */
static const hx_test_server *
pick_tls_banner_server (void)
{
    GPtrArray *cand
        = hx_test_servers_with (HX_TEST_CAP_TLS | HX_TEST_CAP_BANNER_HTXF);
    if (!cand) {
        return NULL;
    }
    const hx_test_server *picked = NULL;
    for (guint i = 0; i < cand->len; i++) {
        const hx_test_server *s = g_ptr_array_index (cand, i);
        if (s->tls_port != 0 && s->tls_xfer_port != 0) {
            picked = s;
            break;
        }
    }
    g_ptr_array_unref (cand);
    return picked;
}

/* Accept-everything cert verify for the TLS HTXF subchannel. Fires
 * only on WebPKI failure (Janus's self-signed cert always fails the
 * public-roots check), where returning 1 pins the connection — the
 * test analogue of banner.c's TOFU verify, which the harness can't
 * use because the test has no known-hosts store. */
static int
tls_banner_xfer_verify_cb (const guint8 *fp, gsize fp_len, void *user_data)
{
    (void)fp;
    (void)fp_len;
    (void)user_data;
    return 1;
}

/* True if the buffer starts with a recognized raster-image magic
 * (GIF / JPEG / PNG). We don't know the server-declared banner type
 * here — the orchestrator's login lifecycle treats the HTLS_HDR_BANNER
 * push as a pre-login frame and consumes it before login completes
 * (see login_reply.rs) — so instead of matching a declared type, we
 * assert the fetched body is a real image. A partial-decrypt or
 * framing slip at the TLS layer would leave the leading bytes garbled
 * and fail every branch. Janus serves a GIF today; JPEG/PNG are
 * accepted too so a config change doesn't break the test. */
static gboolean
banner_bytes_look_like_image (const guint8 *bytes, gsize len)
{
    if (!bytes || len < 4) {
        return FALSE;
    }
    gboolean is_gif = bytes[0] == 'G' && bytes[1] == 'I' && bytes[2] == 'F'
                      && bytes[3] == '8';
    gboolean is_jpeg = bytes[0] == 0xff && bytes[1] == 0xd8;
    gboolean is_png = bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N'
                      && bytes[3] == 'G';
    return is_gif || is_jpeg || is_png;
}

static void
test_banner_htxf_mode_tls (void)
{
    const hx_test_server *srv = pick_tls_banner_server ();
    if (!srv) {
        g_test_fail_printf (
            "no server in matrix advertising both HX_TEST_CAP_TLS and "
            "HX_TEST_CAP_BANNER_HTXF; need Janus with TLS ports mapped.");
        return;
    }

    /* Production login over rustls: the orchestrator drives the full
     * LOGIN + AGREEMENTAGREE (name + icon + OPTIONS) lifecycle and
     * sets htlc.tls = 1, so any HTXF subchannel we open mirrors TLS.
     *
     * We deliberately do NOT wait for the HTLS_HDR_BANNER push here.
     * The orchestrator's login lifecycle classifies the banner push
     * (alongside SELFINFO / AGREEMENT) as a pre-login frame and
     * consumes it before login completes (login_reply.rs), so it never
     * reaches the post-login drain. That's fine: the push is only an
     * advertisement — HTLC_HDR_DOWNLOAD_BANNER fetches the configured
     * banner regardless, which is what production's banner worker
     * relies on too. */
    struct htlc_conn htlc;
    int ctrl = integration_open_login_tls_or_skip (srv, &htlc,
                                                   "TLS-Banner Tier-3", 412);
    if (ctrl < 0) {
        return;
    }

    /* Request the banner body. Capture trans pre-send for the
     * TASK-reply correlation below (hlpack reads then increments). */
    guint32 our_trans = htlc.trans;
    if (!integration_send_message (ctrl, &htlc, HTLC_HDR_DOWNLOAD_BANNER,
                                   /*flag=*/0, /*hc=*/0)) {
        g_test_fail_printf ("HTLC_HDR_DOWNLOAD_BANNER send failed");
        goto cleanup;
    }

    if (!integration_drain_until_task_trans (ctrl, &htlc, our_trans, 16)) {
        g_test_fail_printf ("no TASK reply for DOWNLOAD_BANNER");
        goto cleanup;
    }

    struct hx_htxf_reply reply = { 0 };
    hx_htxf_reply_extract (hx_test_in (&htlc)->buf, hx_test_in (&htlc)->pos,
                           &reply);
    g_assert_cmpuint (reply.ref, >, 0);
    g_assert_cmpuint (reply.size, >, 0);
    g_assert_cmpuint (reply.size, <, 1u << 20); /* sanity cap: < 1 MB */
    guint32 ref = reply.ref, size = reply.size;
    g_test_message ("HTXF ref=%u size=%u", ref, size);

    /* Open the TLS HTXF subchannel the SAME way banner.c's worker
     * does: hxnet_htxf_connect connects to the xfer port and TLS-wraps
     * it (tls=1), all in Rust — no fd crosses the FFI. No parallel
     * GnuTLS harness transport — the body bytes ride the production
     * rustls subchannel. */

    /* Pack the HTXF preamble with HTXF_TYPE_BANNER — Janus refuses
     * a banner ref on a FILE-typed connection. Production banner
     * worker uses the same packer (see banner.c). The preamble carries
     * no HTXF/HOPE AEAD framing — hxnet_htxf_connect writes it before any
     * such per-transfer cipher state is armed, so the server can match
     * the subchannel to the queued transfer by ref. (It's still inside
     * the TLS record layer here: on this TLS-from-byte-zero subchannel
     * every application byte, preamble included, is encrypted by TLS.) */
    guint8 hdr_buf[HX_HTXF_PREAMBLE_MAX_BYTES];
    size_t hdr_len = hxnet_htxf_pack_preamble (hdr_buf, sizeof (hdr_buf), ref,
                                               size, HTXF_TYPE_BANNER,
                                               /*flags=*/0, /*size64=*/FALSE);
    g_assert_cmpuint (hdr_len, >, 0);

    struct htxf_conn xfer;
    memset (&xfer, 0, sizeof (xfer));
    xfer.ref = ref;
    /* hope_aead = NULL: plaintext-TLS banner, no HOPE AEAD framing. */
    xfer.hx = hxnet_htxf_connect ((const guint8 *)srv->host, strlen (srv->host),
                                  srv->tls_xfer_port, NULL, 0, /*tls=*/1,
                                  hdr_buf, hdr_len, /*hope_aead=*/NULL, ref,
                                  tls_banner_xfer_verify_cb, NULL);
    if (!xfer.hx) {
        g_test_fail_printf ("TLS HTXF subchannel (port %u) connect/open failed",
                            (unsigned)srv->tls_xfer_port);
        goto cleanup;
    }

    /* Drain the full image body through production hxnet_htxf_read
     * (passthrough leg — no AEAD). */
    guint8 *bytes = g_malloc (size);
    gsize got = 0;
    while (got < size) {
        ssize_t r
            = hxnet_htxf_read ((HtxfConn *)xfer.hx, bytes + got, size - got);
        if (r <= 0) {
            g_test_message ("hxnet_htxf_read returned %zd at got=%zu", r, got);
            break;
        }
        got += (gsize)r;
    }
    g_assert_cmpuint ((guint)got, ==, size);

    g_test_message ("first 4 bytes: %02x %02x %02x %02x", bytes[0], bytes[1],
                    bytes[2], bytes[3]);
    g_assert_true (banner_bytes_look_like_image (bytes, size));

    g_free (bytes);
    hxnet_htxf_close ((HtxfConn *)xfer.hx);

cleanup:
    integration_release_htlc (&htlc);
    integration_close (ctrl);
}

int
main (int argc, char **argv)
{
    /* Accept the self-signed Janus cert on the TLS HTXF subchannel.
     * Set before g_test_init / any thread spawns — g_setenv after
     * thread creation is not thread-safe. The xfer verify callback
     * already returns 1; this keeps parity with the htxf_connect TLS
     * sibling and covers any env-gated path inside hxnet. */
    g_setenv ("GTKHX_TLS_AUTO_ACCEPT", "1", TRUE);

    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/real_tls_banner/htxf_mode", test_banner_htxf_mode_tls);

    return g_test_run ();
}
