/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_nick_colors.c — Tier 3 coverage for the
 * Colored-Nicknames extension end to end against a real server.
 *
 * Pins down the bug Misha hit on Janus while testing the original
 * implementation: the color picker saved, the outbound USER_CHANGE
 * carried DATA_COLOR, the server processed it — but the local user
 * list still rendered without color until the user touched the
 * picker again. Root cause: Janus carries the per-user color as a
 * 4-byte trailer on every hl_userlist_hdr record in USER_LIST
 * replies, not as a separate DATA_COLOR chunk on a USER_CHANGE
 * broadcast back. Our parser was only reading the standard fixed
 * header.
 *
 * Wire contract under test:
 *   1. Outbound HTLC_HDR_USER_CHANGE with DATA_COLOR (0x0500) is
 *      accepted by the server.
 *   2. The subsequent HTLC_HDR_USER_GETLIST reply carries each
 *      user's record with the standard header AND the 4-byte
 *      trailer, with our own record's trailer matching the color
 *      we just sent.
 *
 * Server gating: requires HX_TEST_CAP_NICK_COLORS, which today
 * only Janus advertises. The test container also needs
 * ColoredNicknames.Enabled: true in config.yaml (the bundled
 * default).
 *
 * Match-by-name (not UID): the harness doesn't extract our session
 * UID from Janus's LOGIN TASK reply (Janus's SELFINFO omits the
 * USER_LIST chunk hx_selfinfo_parse normally reads UID from). The
 * name is something we know up-front and the server preserves
 * round-trip, so matching by name is robust without a uid-discovery
 * dance.
 *
 * Per-process unique color + nick: meson runs Tier 3 binaries in
 * parallel and Janus persists per-account state across reconnects.
 * Pick a process-unique 24-bit random color and a pid+random nick
 * so concurrent test processes don't collide.
 */

#include "config.h"
#include <stdio.h>
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

/* Pick the first nick-color-capable server in the matrix, or NULL
 * if none survived the GTKHX_TEST_SERVERS env filter. */
static const hx_test_server *
pick_nick_color_server (void)
{
    GPtrArray *servers = hx_test_servers_with (HX_TEST_CAP_NICK_COLORS);
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

/* Process-unique 24-bit RGB color. Avoid 0 and 0x00ffffff (the
 * lower 24 bits of HX_NICK_COLOR_NONE) so an assertion on the
 * "color was set" side can't false-positive on the sentinel. */
static guint32
make_unique_color (void)
{
    guint32 r = g_random_int () & 0x00ffffffu;
    if (r == 0 || r == 0x00ffffffu) {
        r ^= 0x00abc123u;
    }
    return r;
}

/* Send HTLC_HDR_USER_CHANGE with NAME + ICON + DATA_COLOR — the
 * 3-chunk shape src/users.c::hx_change_name_icon emits. */
static gboolean
send_user_change_with_color (int fd, struct htlc_conn *htlc,
                             const char *display_name, guint16 icon,
                             guint32 nick_color)
{
    guint16 icon_be = htons (icon);
    guint32 color_be = htonl (nick_color);
    return integration_send_message (
        fd, htlc, HTLC_HDR_USER_CHANGE, /*flag=*/0, /*hc=*/3,
        (int) HTLC_DATA_ICON, (int) sizeof (icon_be), &icon_be,
        (int) HTLC_DATA_NAME, (int) strlen (display_name),
        (guint8 *) display_name,
        (int) HTLC_DATA_COLOR, (int) sizeof (color_be), &color_be);
}

/* End-to-end: login, set a unique color, ask the server for the
 * user list, assert our own record carries the color in the
 * trailing 4 bytes. This is the wire path production's render
 * relies on for the on-login color paint — the bug Misha hit was
 * exactly this: the server was including the trailer all along,
 * we just weren't reading it. */
static void
test_nick_color_user_list_trailer (void)
{
    const hx_test_server *srv = pick_nick_color_server ();
    if (!srv) {
        g_test_fail_printf ("no Colored-Nicknames-capable server in matrix "
                     "(GTKHX_TEST_SERVERS filter excluded all).");
        return;
    }

    /* Pid + random suffix so concurrent test processes can't trample
	 * each other's records under Janus's persistent per-account
	 * storage. Cap at 31 bytes (the on-wire HTLC_DATA_NAME limit). */
    char nick[32];
    g_snprintf (nick, sizeof (nick), "NickClr-%d-%04x",
                (int) getpid (), g_random_int () & 0xffff);

    struct htlc_conn htlc;
    int fd = integration_open_login_to_caps_or_skip (srv, &htlc, nick, 412,
                                                     /*caps=*/0);
    if (fd < 0) {
        return;
    }

    /* Cross the "officially joined" boundary. AGREEMENTAGREE helper
	 * accepts NULL `hope` and falls through to plain send. Janus
	 * doesn't persist USER_CHANGE updates from a session until
	 * AGREEMENTAGREE lands. */
    g_assert_true (integration_send_agreementagree_hope (
        fd, &htlc, /*hope=*/NULL, nick, 412));

    /* Set our color. The spec's auto-opt-in fires on the server's
	 * first DATA_COLOR receipt from this session. */
    guint32 wanted = make_unique_color ();
    g_assert_true (send_user_change_with_color (fd, &htlc, nick, 412, wanted));

    /* Brief settle so Janus persists AGREEMENTAGREE + USER_CHANGE
	 * before the GETLIST reads back. 200 ms is what the
	 * chat-history tests use for the same flake-avoidance against
	 * Janus's SQLite write queue. */
    g_usleep (200000);

    /* Ask for the user list. Zero-chunk opcode, so inline the
	 * integration_send_message call. Capture trans BEFORE the send
	 * — hlpack stamps htlc->trans into the header then increments,
	 * so the pre-send value matches the wire. */
    guint32 list_trans = htlc.trans;
    g_assert_true (integration_send_message (
        fd, &htlc, HTLC_HDR_USER_GETLIST, /*flag=*/0, /*hc=*/0));

    g_assert_true (integration_drain_until_task_trans (fd, &htlc, list_trans,
                                                       /*max_messages=*/32));

    /* Walk the TASK reply chunks; find our own record by name;
	 * verify the trailer carries our color.
	 *
	 * Three outcomes:
	 *   - Our record + trailer present + color matches → PASS.
	 *   - Our record + trailer present + color mismatches → FAIL
	 *     (production regression: parser would read wrong color).
	 *   - Our record present but NO trailer on any record → SKIP.
	 *     The Colored-Nicknames extension is server-side opt-in
	 *     (Janus config.yaml: ColoredNicknames.Enabled). When
	 *     disabled, records come back without the 4-byte trailer.
	 *     The container image ships with it enabled; this skip
	 *     only fires if someone runs against a server that has it
	 *     off.
	 *   - Our record missing entirely → FAIL (join didn't land). */
    gboolean any_record_had_trailer = FALSE;
    gboolean found_us_with_trailer = FALSE;
    gboolean found_us_at_all = FALSE;
    guint32 read_color = 0;
    int n_records = 0;
    dh_start (&htlc)
    {
        if (_type != HTLS_DATA_USER_LIST) {
            continue;
        }
        n_records++;
        if (_len < 8) {
            continue;
        }
        guint16 uid_be, nlen_be;
        memcpy (&uid_be, dh->data + 0, 2);
        memcpy (&nlen_be, dh->data + 6, 2);
        guint16 uid = g_ntohs (uid_be);
        guint16 nlen = g_ntohs (nlen_be);
        if (nlen > 31) nlen = 31;
        char name[33] = { 0 };
        if (_len >= (gsize)(8 + nlen)) {
            memcpy (name, dh->data + 8, nlen);
        }
        gboolean has_trailer = (_len >= (gsize)(8 + nlen + 4));
        g_test_message ("USER_LIST record #%d uid=%u nlen=%u name='%s' "
                        "len=%u trailer=%s",
                        n_records, (unsigned) uid, (unsigned) nlen, name,
                        (unsigned) _len, has_trailer ? "yes" : "no");
        if (has_trailer) {
            any_record_had_trailer = TRUE;
        }
        if (strlen (nick) == nlen && memcmp (name, nick, nlen) == 0) {
            found_us_at_all = TRUE;
            if (has_trailer) {
                found_us_with_trailer = TRUE;
                guint32 col_be;
                memcpy (&col_be, dh->data + 8 + nlen, 4);
                read_color = g_ntohl (col_be);
                break;
            }
        }
    }
    dh_end ();
    g_test_message ("walked %d USER_LIST records total", n_records);

    g_assert_true (found_us_at_all);

    if (!any_record_had_trailer) {
        g_test_fail_printf ("server didn't include the Colored-Nicknames trailer "
                     "in any USER_LIST record — server-side extension is "
                     "disabled (Janus: ColoredNicknames.Enabled in "
                     "config.yaml).");
        integration_release_htlc (&htlc);
        integration_close (fd);
        return;
    }

    g_assert_true (found_us_with_trailer);
    g_assert_cmphex (read_color, ==, wanted);

    integration_release_htlc (&htlc);
    integration_close (fd);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/integration/nick_colors/user_list_trailer",
                     test_nick_color_user_list_trailer);

    return g_test_run ();
}
