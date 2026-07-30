/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_gif_icons.c — Tier 3 coverage for the
 * fogWraith GIF-icons extension (TRAN 1861-1864 / 0x0745-0x0748,
 * DATA_ICON_GIF 0x0300 / DATA_ICON_LIST 0x0301).
 *
 * The extension defines no capability bit, so there's nothing to
 * filter the matrix on — both Docker targets implement it (verified
 * June 2026: mhxd and Janus both round-trip a GIF payload and discard
 * the legacy cicn field). The tests therefore run against the default
 * matrix server (GTKHX_TEST_HOST/PORT), the same pattern the non-cap
 * tests like test_chat_roundtrip / test_two_client_chat use.
 *
 * Coverage map (10.A wire foundation):
 *
 *   set_get_roundtrip   — ICON_SET a GIF, then ICON_GET our own uid;
 *                         the bytes round-trip through the server's
 *                         per-session store. Parsed via the production
 *                         Rust parser gtkhx_proto_parse_icon_get_reply.
 *   getlist_self        — after a set, ICON_GETLIST returns a packed
 *                         entry for our uid carrying our GIF
 *                         (gtkhx_proto_parse_icon_list).
 *   change_broadcast    — two clients: A sets an icon, B receives the
 *                         ICON_CHANGE (1864) broadcast carrying A's uid
 *                         (gtkhx_proto_parse_icon_change).
 *   clear               — ICON_SET with an empty payload clears the
 *                         avatar; a subsequent ICON_GET reports no GIF.
 *
 * The wire send shapes are hand-built here via integration_send_message
 * (the builders are covered by the Tier 2 unit tests in the Rust crate);
 * every reply is parsed by the same production Rust parsers the C rcv
 * handlers call.
 */

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <glib.h>
#include "compat.h"
#include "hotline.h"
#include "protocol.h"
#include "proto_helpers.h"
#include "hotline_proto.h"
#include "integration_harness.h"
#include "server_matrix.h"

/* A minimal valid 1x1 GIF89a — the same blob the manual Janus probe
 * used. Begins with the GIF89a signature the server validates. */
static const guint8 TINY_GIF[] = {
    0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x01, 0x00, 0x01, 0x00, 0x80,
    0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0x21, 0xf9, 0x04,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x01, 0x00, 0x00, 0x02, 0x02, 0x44, 0x01, 0x00, 0x3b,
};

/* Send ICON_SET (1862) with a single DATA_ICON_GIF chunk. gif==NULL /
 * len==0 sends an empty field (clear). Returns the trans the server
 * will echo in its TASK reply, or 0 on send failure. */
static guint32
send_icon_set (int fd, struct htlc_conn *htlc, const guint8 *gif, gsize len)
{
    guint32 trans = htlc->trans;
    if (!integration_send_message (fd, htlc, HTLC_HDR_ICON_SET, /*flag=*/0,
                                   /*hc=*/1, (int)HTLC_DATA_ICON_GIF, (int)len,
                                   gif)) {
        return 0;
    }
    return trans;
}

/* Send ICON_GET (1863) for `uid`. Returns the trans for correlation. */
static guint32
send_icon_get (int fd, struct htlc_conn *htlc, guint16 uid)
{
    guint16 uid_be = g_htons (uid);
    guint32 trans = htlc->trans;
    if (!integration_send_message (fd, htlc, HTLC_HDR_ICON_GET, /*flag=*/0,
                                   /*hc=*/1, (int)HTLC_DATA_UID,
                                   (int)sizeof (uid_be), &uid_be)) {
        return 0;
    }
    return trans;
}

/* Send ICON_GETLIST (1861) — no request fields. */
static guint32
send_icon_getlist (int fd, struct htlc_conn *htlc)
{
    guint32 trans = htlc->trans;
    if (!integration_send_message (fd, htlc, HTLC_HDR_ICON_GETLIST, /*flag=*/0,
                                   /*hc=*/0)) {
        return 0;
    }
    return trans;
}

static void
close_session (int fd, struct htlc_conn *htlc)
{
    integration_release_htlc (htlc);
    integration_close (fd);
}

/* ------------------------------------------------------------------ */

/* ICON_SET then ICON_GET our own uid: the GIF bytes round-trip. */
static void
test_gif_icons_set_get_roundtrip (void)
{
    struct htlc_conn htlc;
    int fd
        = integration_open_login_or_skip (&htlc, "IconRoundtrip Tier-3", 412);
    if (fd < 0) {
        return;
    }

    guint32 set_trans = send_icon_set (fd, &htlc, TINY_GIF, sizeof (TINY_GIF));
    g_assert_cmpuint (set_trans, !=, 0);
    /* The set reply is a bare TASK completion; assert it didn't error. */
    g_assert_true (
        integration_drain_until_task_trans (fd, &htlc, set_trans, 32));
    g_assert_cmphex ((hdr_flag (&htlc) & 1), ==, 0);

    /* Give the server a beat to commit the per-session icon before we
     * fetch — Janus stores it asynchronously, so an immediate ICON_GET
     * can race ahead of the write (mhxd stores synchronously). Mirrors
     * the same guard in test_chat_history. */
    g_usleep (200000); /* 200 ms */

    guint32 get_trans = send_icon_get (fd, &htlc, htlc.uid);
    g_assert_cmpuint (get_trans, !=, 0);
    g_assert_true (
        integration_drain_until_task_trans (fd, &htlc, get_trans, 32));

    struct gtkhx_proto_icon_entry entry;
    g_assert_true (gtkhx_proto_parse_icon_get_reply (
        hx_test_in (&htlc)->buf, hx_test_in (&htlc)->pos, &entry));
    g_assert_cmpuint (entry.uid, ==, htlc.uid);
    g_assert_cmpuint (entry.gif_len, ==, sizeof (TINY_GIF));
    g_assert_cmpmem (entry.gif_ptr, entry.gif_len, TINY_GIF, sizeof (TINY_GIF));

    close_session (fd, &htlc);
}

/* After a set, ICON_GETLIST contains a packed entry for our uid with
 * our GIF. Other concurrent test sessions may also appear in the list,
 * so we search for our own uid rather than asserting a count. */
static void
test_gif_icons_getlist_self (void)
{
    struct htlc_conn htlc;
    int fd = integration_open_login_or_skip (&htlc, "IconList Tier-3", 412);
    if (fd < 0) {
        return;
    }

    guint32 set_trans = send_icon_set (fd, &htlc, TINY_GIF, sizeof (TINY_GIF));
    g_assert_cmpuint (set_trans, !=, 0);
    g_assert_true (
        integration_drain_until_task_trans (fd, &htlc, set_trans, 32));
    g_assert_cmphex ((hdr_flag (&htlc) & 1), ==, 0); /* set not rejected */

    g_usleep (200000); /* 200 ms — let Janus commit before listing */

    guint32 list_trans = send_icon_getlist (fd, &htlc);
    g_assert_cmpuint (list_trans, !=, 0);
    g_assert_true (
        integration_drain_until_task_trans (fd, &htlc, list_trans, 32));

    /* Count, then fill exactly — the same two-pass shape the C rcv
     * handler (rcv_task_icon_getlist) uses. */
    size_t n = gtkhx_proto_parse_icon_list (hx_test_in (&htlc)->buf,
                                            hx_test_in (&htlc)->pos, NULL, 0);
    g_assert_cmpuint (n, >, 0);
    struct gtkhx_proto_icon_entry *entries
        = g_new0 (struct gtkhx_proto_icon_entry, n);
    size_t got = gtkhx_proto_parse_icon_list (
        hx_test_in (&htlc)->buf, hx_test_in (&htlc)->pos, entries, n);
    g_assert_cmpuint (got, ==, n);

    gboolean found = FALSE;
    for (size_t i = 0; i < got; i++) {
        if (entries[i].uid == htlc.uid) {
            found = TRUE;
            g_assert_cmpuint (entries[i].gif_len, ==, sizeof (TINY_GIF));
            g_assert_cmpmem (entries[i].gif_ptr, entries[i].gif_len, TINY_GIF,
                             sizeof (TINY_GIF));
        }
    }
    g_assert_true (found);

    g_free (entries);
    close_session (fd, &htlc);
}

/* Two clients: A sets an icon; B receives the ICON_CHANGE broadcast
 * carrying A's uid. */
static void
test_gif_icons_change_broadcast (void)
{
    struct htlc_conn htlc_a;
    int fd_a
        = integration_open_login_or_skip (&htlc_a, "IconChangeA Tier-3", 412);
    if (fd_a < 0) {
        return;
    }
    struct htlc_conn htlc_b;
    int fd_b
        = integration_open_login_or_skip (&htlc_b, "IconChangeB Tier-3", 412);
    if (fd_b < 0) {
        close_session (fd_a, &htlc_a);
        return;
    }

    guint32 set_trans
        = send_icon_set (fd_a, &htlc_a, TINY_GIF, sizeof (TINY_GIF));
    g_assert_cmpuint (set_trans, !=, 0);
    /* Confirm A's set was accepted before waiting on B — otherwise a
     * rejected upload would surface as an opaque ICON_CHANGE timeout. */
    g_assert_true (
        integration_drain_until_task_trans (fd_a, &htlc_a, set_trans, 32));
    g_assert_cmphex ((hdr_flag (&htlc_a) & 1), ==, 0);

    /* B drains until an ICON_CHANGE broadcast carrying A's uid arrives.
     * The matrix runs binaries in parallel, so filter by uid to skip
     * change notices triggered by other concurrent test sessions. */
    gboolean saw = FALSE;
    for (int i = 0; i < 64 && !saw; i++) {
        if (!integration_recv_message (fd_b, &htlc_b, /*timeout_ms=*/4000)) {
            break;
        }
        if (hdr_type (&htlc_b) != HTLS_HDR_ICON_CHANGE) {
            continue;
        }
        guint16 changed = 0;
        if (gtkhx_proto_parse_icon_change (hx_test_in (&htlc_b)->buf,
                                           hx_test_in (&htlc_b)->pos, &changed)
            && changed == htlc_a.uid) {
            saw = TRUE;
        }
    }
    g_assert_true (saw);

    close_session (fd_a, &htlc_a);
    close_session (fd_b, &htlc_b);
}

/* ICON_SET with an empty payload clears the avatar; the subsequent
 * ICON_GET must report no GIF. Servers differ in how they encode
 * "no avatar" (Janus omits DATA_ICON_GIF entirely; a server that
 * echoed a zero-length field would parse as gif_len == 0), so the
 * assertion is "no non-empty avatar comes back" either way. */
static void
test_gif_icons_clear (void)
{
    struct htlc_conn htlc;
    int fd = integration_open_login_or_skip (&htlc, "IconClear Tier-3", 412);
    if (fd < 0) {
        return;
    }

    /* Set, then clear. */
    guint32 set_trans = send_icon_set (fd, &htlc, TINY_GIF, sizeof (TINY_GIF));
    g_assert_cmpuint (set_trans, !=, 0);
    g_assert_true (
        integration_drain_until_task_trans (fd, &htlc, set_trans, 32));
    g_assert_cmphex ((hdr_flag (&htlc) & 1), ==, 0); /* set not rejected */

    g_usleep (200000); /* 200 ms */

    guint32 clr_trans = send_icon_set (fd, &htlc, NULL, 0);
    g_assert_cmpuint (clr_trans, !=, 0);
    g_assert_true (
        integration_drain_until_task_trans (fd, &htlc, clr_trans, 32));
    g_assert_cmphex ((hdr_flag (&htlc) & 1), ==, 0); /* clear not rejected */

    g_usleep (200000); /* 200 ms — let the clear commit before the get */

    guint32 get_trans = send_icon_get (fd, &htlc, htlc.uid);
    g_assert_cmpuint (get_trans, !=, 0);
    g_assert_true (
        integration_drain_until_task_trans (fd, &htlc, get_trans, 32));

    /* A cleared reply still parses: UID is present, the GIF is empty
     * (whether the server omitted DATA_ICON_GIF, like Janus, or echoed
     * a zero-length field). parse_icon_get_reply only fails on a
     * missing UID, so a false return here would mean a malformed reply
     * — we require success and an empty avatar. */
    struct gtkhx_proto_icon_entry entry;
    g_assert_true (gtkhx_proto_parse_icon_get_reply (
        hx_test_in (&htlc)->buf, hx_test_in (&htlc)->pos, &entry));
    g_assert_cmpuint (entry.uid, ==, htlc.uid);
    g_assert_cmpuint (entry.gif_len, ==, 0);

    close_session (fd, &htlc);
}

/* ------------------------------------------------------------------ */
int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/integration/gif_icons/set_get_roundtrip",
                     test_gif_icons_set_get_roundtrip);
    g_test_add_func ("/integration/gif_icons/getlist_self",
                     test_gif_icons_getlist_self);
    g_test_add_func ("/integration/gif_icons/change_broadcast",
                     test_gif_icons_change_broadcast);
    g_test_add_func ("/integration/gif_icons/clear", test_gif_icons_clear);

    return g_test_run ();
}
