/*
 * tests/integration/test_user_list.c — fetch the user list from a
 * real Hotline server and verify our own session shows up in it.
 *
 * Sequence:
 *   1. Login (we're now in mhxd's user table).
 *   2. Send HTLC_HDR_USER_GETLIST.
 *   3. Receive HTLS_HDR_TASK reply containing one or more
 *      HTLS_DATA_USER_LIST chunks (one per user).
 *   4. Walk the chunks, find the one matching our own UID, and
 *      verify the name and icon round-trip.
 *
 * Since we're the only client logged in, the user list contains
 * exactly one row. The single-row case is the simplest assertion
 * but still proves the request/reply flow + chunk parse work end-
 * to-end.
 */

#include "config.h"
#include <string.h>
#include <netinet/in.h>
#include <unistd.h>
#include <glib.h>
#include "compat.h"
#include "hotline.h"
#include "protocol.h"
#include "proto_helpers.h"
#include "integration_harness.h"

static void
test_user_list_contains_self (void)
{
    struct htlc_conn htlc;
    int fd = integration_open_login_or_skip (&htlc, "UserList Tier-3", 412);
    if (fd < 0) {
        return;
    }

    /* Capture our own UID before we issue the request — the
	 * subsequent recv_message will overwrite htlc->in but
	 * htlc->uid stays. */
    guint16 self_uid = htlc.uid;

    /* Save the trans value hlpack will assign to our request, so
	 * we can match the reply against it. mhxd's reply uses the
	 * SAME trans the request carried — that's how task replies
	 * are correlated to their originating request. */
    guint32 our_trans = htlc.trans;

    /* Send HTLC_HDR_USER_GETLIST with no chunks (the request
	 * carries no payload — server replies with the list). */
    g_assert_true (integration_send_message (fd, &htlc, HTLC_HDR_USER_GETLIST,
                                             /*flag=*/0, /*hc=*/0));

    /* Drain looking for the TASK reply matching our trans. We
	 * filter by trans because meson runs integration test
	 * binaries in parallel: USER_CHANGE / CHAT broadcasts from
	 * other concurrent test processes (each logged in as a
	 * different user) hit our connection too, and we need to
	 * walk past them.
	 *
	 * Budget: 16 messages × 3 s timeout. The matching TASK
	 * normally arrives within 1-2 messages even under contention. */
    gboolean got_user_list = FALSE;
    for (int i = 0; i < 64 && !got_user_list; i++) {
        g_assert_true (
            integration_recv_message (fd, &htlc, /*timeout_ms=*/3000));
        if (hdr_type (&htlc) != HTLS_HDR_TASK) {
            continue;
        }
        if (hdr_trans (&htlc) != our_trans) {
            continue;
        }

        dh_start (&htlc)
        {
            if (_type == HTLS_DATA_USER_LIST) {
                got_user_list = TRUE;
            }
        }
        dh_end ();
    }
    g_assert_true (got_user_list);

    /* Walk every USER_LIST chunk, find the one whose uid matches
	 * our own session uid, assert name + icon round-trip. */
    struct hl_userlist_hdr *uh;
    guint16 chunk_uid, chunk_icon, chunk_nlen;
    gboolean found_self = FALSE;
    dh_start (&htlc)
    {
        if (_type != HTLS_DATA_USER_LIST) {
            continue;
        }
        if (_len < (SIZEOF_HL_USERLIST_HDR - SIZEOF_HL_DATA_HDR)) {
            continue;
        }

        uh = (struct hl_userlist_hdr *)dh;
        HN16 (&chunk_uid, &uh->uid);
        HN16 (&chunk_icon, &uh->icon);
        HN16 (&chunk_nlen, &uh->nlen);
        if (chunk_uid != self_uid) {
            continue;
        }

        /* Found us. Verify the name + icon round-trip. */
        char name_buf[33];
        gsize copy_len = chunk_nlen > 31 ? 31 : chunk_nlen;
        memcpy (name_buf, uh->name, copy_len);
        name_buf[copy_len] = '\0';

        g_assert_cmpstr (name_buf, ==, "UserList Tier-3");
        g_assert_cmphex (chunk_icon, ==, 412);
        found_self = TRUE;
    }
    dh_end ();

    g_assert_true (found_self);

    integration_release_htlc (&htlc);
    integration_close (fd);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/integration/user_list/contains_self",
                     test_user_list_contains_self);

    return g_test_run ();
}
