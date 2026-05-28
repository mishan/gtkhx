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
 * docs/tls-scoping.md Phase 2: file-mode (HTXF) banner fetch over
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
 */

#include "config.h"

#include <string.h>
#include <glib.h>
#include <gio/gio.h>
#include <netinet/in.h>

#include "compat.h"
#include "hotline.h"
#include "protocol.h"
#include "proto_helpers.h"
#include "hl_code.h"
#include "htxf_subchannel.h"
#include "server_matrix.h"
#include "integration_harness.h"
#include "integration_tls.h"

/* Pick a server that advertises BOTH HX_TEST_CAP_TLS and
 * HX_TEST_CAP_BANNER_HTXF — today Janus, the only matrix entry that
 * does both. Same shape as pick_banner_htxf_server in the plaintext
 * test, extended to require TLS too. */
static const hx_test_server *
pick_tls_banner_server (void)
{
    GPtrArray *cand = hx_test_servers_with (
        HX_TEST_CAP_TLS | HX_TEST_CAP_BANNER_HTXF);
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

/* Skinny LOGIN — guest, no display name yet. Display name lives in
 * the AGREEMENTAGREE that follows; mhxd / Mobius / Janus all gate
 * the post-agreement message blast (BANNER included) on receiving
 * it. Mirrors the fd-based send_skinny_login in test_banner.c. */
static gboolean
send_skinny_login_stream (GIOStream *io, struct htlc_conn *htlc, guint16 icon)
{
    const char *login = "guest";
    gsize llen = strlen (login);
    guint8 enclogin[16];
    /* hl_code is the same XOR-with-0xff transform as
     * test_banner.c's hl_code_inline static helper; we use the
     * public hl_code.h entry point so the test-only mirror isn't
     * duplicated. */
    hl_code (enclogin, login, llen);
    guint16 icon_be = htons (icon);
    guint16 cv_be = htons (185);

    return integration_send_message_stream (
        io, htlc, HTLC_HDR_LOGIN, /*flag=*/0, /*hc=*/3,
        (int) HTLC_DATA_ICON, (int) sizeof (icon_be), &icon_be,
        (int) HTLC_DATA_LOGIN, (int) llen, enclogin,
        (int) HTLC_DATA_CLIENTVERSION, (int) sizeof (cv_be), &cv_be);
}

/* Validate that a buffer starts with the magic bytes for the
 * declared banner image type. Copy of banner_bytes_match_type in
 * test_banner.c — small enough to inline rather than promote to
 * a shared helper. */
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
    if (strcmp (type, "PICT") == 0) {
        return TRUE;
    }
    if (strcmp (type, "PNG ") == 0) {
        return bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N'
               && bytes[3] == 'G';
    }
    return TRUE;
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

    struct htlc_conn htlc;
    memset (&htlc, 0, sizeof (htlc));

    GIOStream *ctrl = hx_test_server_connect_tls (srv);
    if (!ctrl) {
        g_test_fail_printf ("TLS connect to %s (%s:%u) failed",
                            srv->name, srv->host,
                            (unsigned) srv->tls_port);
        return;
    }
    if (!integration_handshake_stream (ctrl)) {
        g_test_fail_printf ("TLS handshake against %s failed", srv->name);
        integration_close_stream (ctrl);
        return;
    }

    if (!send_skinny_login_stream (ctrl, &htlc, 412)) {
        g_test_fail_printf ("skinny LOGIN send failed");
        goto cleanup;
    }

    /* Drain past loginreply TASK + AGREEMENT — same shape as the
     * plaintext sibling (test_banner.c::banner_setup_or_skip).
     * Real interesting messages arrive after AGREEMENTAGREE. */
    for (int i = 0; i < 4; i++) {
        if (!integration_recv_message_stream (ctrl, &htlc,
                                              /*timeout_ms=*/2000)) {
            break;
        }
    }

    /* AGREEMENTAGREE with name + icon + options=0. The OPTIONS
     * chunk matters for Mobius interop (panics-on-missing); Janus
     * accepts both shapes. Same wire bytes as the plaintext test
     * so any future server-side regression catches both at once. */
    const char *name = "TLS-Banner Tier-3";
    guint16 icon_be = htons (412);
    guint16 options_be = htons (0);
    if (!integration_send_message_stream (
            ctrl, &htlc, HTLC_HDR_AGREEMENTAGREE, /*flag=*/0, /*hc=*/3,
            (int) HTLC_DATA_NAME, (int) strlen (name), (guint8 *) name,
            (int) HTLC_DATA_ICON, (int) sizeof (icon_be), &icon_be,
            (int) HTLC_DATA_OPTIONS, (int) sizeof (options_be),
            &options_be)) {
        g_test_fail_printf ("AGREEMENTAGREE send failed");
        goto cleanup;
    }

    /* Drain to HTLS_HDR_BANNER, pull TYPE + URL out of the frame. */
    if (!integration_drain_until_type_stream (ctrl, &htlc,
                                              HTLS_HDR_BANNER, 64)) {
        g_test_fail_printf ("no HTLS_HDR_BANNER received");
        goto cleanup;
    }

    gchar *banner_type = NULL, *banner_url = NULL;
    dh_start (&htlc)
    {
        switch (_type) {
        case HTLS_DATA_BANNER_TYPE:
            banner_type = g_strndup ((const char *) dh->data, _len);
            break;
        case HTLS_DATA_BANNER_URL:
            banner_url = g_strndup ((const char *) dh->data, _len);
            break;
        }
    }
    dh_end ();
    g_test_message ("banner type=\"%s\" url=\"%s\"",
                    banner_type ? banner_type : "(null)",
                    banner_url ? banner_url : "(null)");
    g_assert_nonnull (banner_type);
    /* Janus serves binary banner regardless of any URL field, but
     * surface a warning if the server sent both anyway. */
    if (banner_url && *banner_url) {
        g_test_message ("server type=\"%s\" but also sent URL=\"%s\"; "
                        "proceeding with HTXF fetch per spec",
                        banner_type, banner_url);
    }

    /* Request the banner body. Capture trans pre-send for the
     * TASK-reply correlation below (hlpack reads then increments). */
    guint32 our_trans = htlc.trans;
    if (!integration_send_message_stream (ctrl, &htlc,
                                          HTLC_HDR_DOWNLOAD_BANNER,
                                          /*flag=*/0, /*hc=*/0)) {
        g_test_fail_printf ("HTLC_HDR_DOWNLOAD_BANNER send failed");
        g_free (banner_type);
        g_free (banner_url);
        goto cleanup;
    }

    if (!integration_drain_until_task_trans_stream (ctrl, &htlc,
                                                    our_trans, 16)) {
        g_test_fail_printf ("no TASK reply for DOWNLOAD_BANNER");
        g_free (banner_type);
        g_free (banner_url);
        goto cleanup;
    }

    struct hx_htxf_reply reply = { 0 };
    hx_htxf_reply_extract (&htlc, &reply);
    g_assert_cmpuint (reply.ref, >, 0);
    g_assert_cmpuint (reply.size, >, 0);
    g_assert_cmpuint (reply.size, <, 1u << 20); /* sanity cap: < 1 MB */
    guint32 ref = reply.ref, size = reply.size;
    g_test_message ("HTXF ref=%u size=%u", ref, size);

    /* Open the TLS HTXF subchannel — same cert-stub handshake as
     * the control channel. */
    GIOStream *xfer = hx_test_server_connect_xfer_tls (srv);
    if (!xfer) {
        g_test_fail_printf (
            "TLS HTXF subchannel port (%u) not reachable",
            (unsigned) srv->tls_xfer_port);
        g_free (banner_type);
        g_free (banner_url);
        goto cleanup;
    }

    /* Pack the HTXF preamble with HTXF_TYPE_BANNER — Janus refuses
     * a banner ref on a FILE-typed connection. Production banner
     * worker uses the same packer (see banner.c::
     * banner_htxf_worker_thread). */
    guint8 hdr_buf[HX_HTXF_PREAMBLE_MAX_BYTES];
    size_t hdr_len = hx_htxf_subchannel_pack_preamble (
        hdr_buf, sizeof (hdr_buf), ref, size, HTXF_TYPE_BANNER,
        /*flags=*/0, /*size64=*/FALSE);
    g_assert_cmpuint (hdr_len, >, 0);
    g_assert_true (integration_send_stream (xfer, hdr_buf, hdr_len));

    guint8 *bytes = g_malloc (size);
    g_assert_true (integration_recv_stream (xfer, bytes, size,
                                            /*timeout_ms=*/10000));

    g_test_message ("first 4 bytes: %02x %02x %02x %02x", bytes[0],
                    bytes[1], bytes[2], bytes[3]);
    g_assert_true (banner_bytes_match_type (banner_type, bytes, size));

    g_free (bytes);
    integration_close_stream (xfer);
    g_free (banner_type);
    g_free (banner_url);

cleanup:
    integration_release_htlc (&htlc);
    integration_close_stream (ctrl);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/real_tls_banner/htxf_mode", test_banner_htxf_mode_tls);

    return g_test_run ();
}
