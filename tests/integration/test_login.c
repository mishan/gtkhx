/*
 * tests/integration/test_login.c — drive a real login flow against
 * an mhxd-style Hotline server.
 *
 * Sequence:
 *   1. TCP connect + magic handshake (via integration_open_or_skip).
 *   2. Send HTLC_HDR_LOGIN as guest (hc=3: ICON, LOGIN, NAME).
 *   3. Read messages from the server, looking for one of:
 *        - HTLS_HDR_TASK with task-error flag → login refused
 *        - HTLS_HDR_USER_SELFINFO            → login accepted
 *        - HTLS_HDR_TASK loginreply          → version + name
 *        - HTLS_HDR_AGREEMENT                → server agreement
 *      Most servers send several of these in sequence; we keep
 *      reading until we either see a SELFINFO (success) or hit a
 *      task-error / timeout (failure).
 *
 * Skip-if-unavailable: if no server is reachable at the configured
 * host:port, the test calls g_test_skip and exits cleanly.
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

/* Pull the message type field out of the just-received header. */
static guint32
hdr_type (const struct htlc_conn *htlc)
{
    const struct hl_hdr *h = (const struct hl_hdr *)htlc->in.buf;
    return ntohl (h->type);
}

static void
test_login_guest_succeeds (void)
{
    struct htlc_conn htlc;
    int fd = integration_open_login_or_skip (&htlc, "GtkHx Tier-3 test", 412);
    if (fd < 0) {
        return;
    }

    /* integration_open_login_or_skip already parsed SELFINFO into
	 * htlc.access / uid / icon / name. Sanity-check the session
	 * state. */

    /* Some access bit must be set — even a guest account has
	 * download / chat permissions. */
    g_assert_cmphex (htlc.access, !=, 0);

    /* Our display name should round-trip back unchanged. */
    g_assert_cmpstr ((const char *)htlc.name, ==, "GtkHx Tier-3 test");

    integration_release_htlc (&htlc);
    integration_close (fd);
}

/* Negative case: send a malformed login (no LOGIN chunk) and expect
 * either a task-error or a clean disconnect. We don't assert on
 * which because mhxd, hlserver, and Badmoon all behave slightly
 * differently — but a SELFINFO reply would be definitively wrong. */
static void
test_login_malformed_does_not_succeed (void)
{
    int fd = integration_open_or_skip ();
    if (fd < 0) {
        return;
    }

    struct htlc_conn htlc;
    memset (&htlc, 0, sizeof (htlc));

    guint16 icon_be = htons (0);
    g_assert_true (integration_send_message (fd, &htlc, HTLC_HDR_LOGIN, 0,
                                             /*hc=*/1, (int)HTLC_DATA_ICON,
                                             (int)sizeof (icon_be), &icon_be));

    /* Read whatever the server responds with (or doesn't). If it
	 * ever sends SELFINFO that's a server bug; otherwise either
	 * task-error or socket-close is acceptable. */
    if (integration_recv_message (fd, &htlc, /*timeout_ms=*/3000)) {
        guint32 type = hdr_type (&htlc);
        g_assert_cmphex (type, !=, HTLS_HDR_USER_SELFINFO);
    }

    integration_release_htlc (&htlc);
    integration_close (fd);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/integration/login/guest_succeeds",
                     test_login_guest_succeeds);
    g_test_add_func ("/integration/login/malformed_does_not_succeed",
                     test_login_malformed_does_not_succeed);

    return g_test_run ();
}
