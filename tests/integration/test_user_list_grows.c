/*
 * tests/integration/test_user_list_grows.c — verify Bob appears
 * in Alice's USER_GETLIST reply once Bob has logged in.
 *
 * The existing test_user_list checks 'we appear in our own
 * USER_GETLIST reply', single-client. This test goes the other
 * direction: prove the reply ACTUALLY enumerates other users
 * when they're present.
 *
 * Sequence:
 *   1. Alice logs in.
 *   2. Bob logs in (with a unique display name we can grep for).
 *   3. Alice requests HTLC_HDR_USER_GETLIST.
 *   4. Walk the reply's HTLS_DATA_USER_LIST chunks and assert at
 *      least one entry's name is Bob's.
 *
 * Why this rather than 'count grows by one': mhxd is a shared
 * server. When the integration suite runs in parallel (or even
 * just back-to-back with previous-test residue still draining),
 * other clients connect and disconnect during the window between
 * Alice's two list requests. A strict +1 invariant is fragile.
 * Looking up Bob by name is a stable, race-free contract.
 *
 * Catches a regression where mhxd's snd_user_list filters too
 * aggressively (returns only the requester) or where the per-htlc
 * list isn't actually walked when serialising.
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

/* Run a USER_GETLIST round-trip and return TRUE if any
 * HTLS_DATA_USER_LIST chunk in the reply has a name matching
 * `wanted_name`. The user-list entry layout (per
 * struct hl_userlist_hdr in mhxd):
 *   2 bytes: uid    (big-endian)
 *   2 bytes: icon   (big-endian)
 *   2 bytes: color  (big-endian)
 *   2 bytes: nlen   (big-endian)
 *   N bytes: name
 */
static gboolean
user_list_contains (int fd, struct htlc_conn *htlc, const char *wanted_name)
{
    guint32 our_trans = htlc->trans;
    if (!integration_send_message (fd, htlc, HTLC_HDR_USER_GETLIST, /*flag=*/0,
                                   /*hc=*/0)) {
        return FALSE;
    }

    if (!integration_drain_until_task_trans (fd, htlc, our_trans, 64)) {
        return FALSE;
    }
    if (hdr_flag (htlc) & 1) {
        return FALSE;
    }

    gsize wlen = strlen (wanted_name);
    gboolean found = FALSE;
    dh_start (htlc->in.buf, htlc->in.pos)
    {
        if (_type != HTLS_DATA_USER_LIST) {
            continue;
        }
        if (_len < 8) {
            continue;
        }
        guint16 nlen;
        memcpy (&nlen, dh->data + 6, 2);
        nlen = ntohs (nlen);
        if (8 + (gsize)nlen > _len) {
            continue;
        }
        if (nlen == wlen && memcmp (dh->data + 8, wanted_name, wlen) == 0) {
            found = TRUE;
            break;
        }
    }
    dh_end ();
    return found;
}

static void
test_user_list_includes_other_client (void)
{
    struct htlc_conn htlc_a;
    int fd_a = integration_open_login_or_skip (&htlc_a, "GrowAlice T-3", 412);
    if (fd_a < 0) {
        return;
    }

    struct htlc_conn htlc_b;
    const char *bob_name = "GrowBob T-3";
    int fd_b = integration_open_login_or_skip (&htlc_b, bob_name, 412);
    if (fd_b < 0) {
        integration_release_htlc (&htlc_a);
        integration_close (fd_a);
        return;
    }

    /* Drain Alice's join broadcast for Bob so the recv_message
	 * loop inside user_list_contains starts on a clean stream. */
    for (int i = 0; i < 8; i++) {
        if (!integration_recv_message (fd_a, &htlc_a, /*timeout_ms=*/500)) {
            break;
        }
        if (hdr_type (&htlc_a) == HTLS_HDR_USER_CHANGE) {
            break;
        }
    }

    g_assert_true (user_list_contains (fd_a, &htlc_a, bob_name));

    integration_release_htlc (&htlc_b);
    integration_close (fd_b);
    integration_release_htlc (&htlc_a);
    integration_close (fd_a);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/integration/user_list/includes_other_client",
                     test_user_list_includes_other_client);
    return g_test_run ();
}
