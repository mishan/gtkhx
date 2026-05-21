/*
 * tests/integration/test_msg_roundtrip.c — private-message
 * round-trip between two clients.
 *
 * Open two simultaneous connections (Alice and Bob), Alice sends
 * HTLC_HDR_MSG addressed to Bob's uid, Bob's connection receives
 * HTLS_HDR_MSG carrying Alice's uid + name + the message body.
 *
 * Mirrors the chat_roundtrip test's two-client setup but exercises
 * the private-message wire path (HTLC_HDR_MSG / HTLS_HDR_MSG)
 * which is parsed by the Tier 2 hx_msg_extract helper rather than
 * hx_chat_extract.
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

/* Drain looking for an HTLS_HDR_MSG addressed from `wanted_uid`. */
static gboolean
drain_until_msg_from_uid (int fd, struct htlc_conn *htlc, guint16 wanted_uid,
                          struct hx_msg_msg *out, int max_messages)
{
    for (int i = 0; i < max_messages; i++) {
        if (!integration_recv_message (fd, htlc, /*timeout_ms=*/3000)) {
            return FALSE;
        }
        if (hdr_type (htlc) != HTLS_HDR_MSG) {
            continue;
        }
        if (!hx_msg_extract (htlc, out)) {
            continue;
        }
        if (out->uid == wanted_uid) {
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean
send_msg (int fd, struct htlc_conn *htlc, guint16 to_uid, const char *body)
{
    guint16 uid_be = htons (to_uid);
    return integration_send_message (
        fd, htlc, HTLC_HDR_MSG, /*flag=*/0, /*hc=*/2, (int)HTLC_DATA_UID,
        (int)sizeof (uid_be), &uid_be, (int)HTLC_DATA_MSG, (int)strlen (body),
        (guint8 *)body);
}

static void
test_msg_roundtrip_a_to_b (void)
{
    struct htlc_conn htlc_a;
    int fd_a = integration_open_login_or_skip (&htlc_a, "MsgAlice Tier-3", 412);
    if (fd_a < 0) {
        return;
    }

    struct htlc_conn htlc_b;
    int fd_b = integration_open_login_or_skip (&htlc_b, "MsgBob Tier-3", 412);
    if (fd_b < 0) {
        integration_release_htlc (&htlc_a);
        integration_close (fd_a);
        return;
    }

    const char *body = "hi bob — integration suite";
    g_assert_true (send_msg (fd_a, &htlc_a, htlc_b.uid, body));

    struct hx_msg_msg pm;
    g_assert_true (drain_until_msg_from_uid (fd_b, &htlc_b, htlc_a.uid, &pm,
                                             /*max_messages=*/64));

    g_assert_cmphex (pm.uid, ==, htlc_a.uid);
    g_assert_cmpstr (pm.name, ==, "MsgAlice Tier-3");
    g_assert_cmpstr (pm.msg, ==, body);

    integration_release_htlc (&htlc_b);
    integration_close (fd_b);
    integration_release_htlc (&htlc_a);
    integration_close (fd_a);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/integration/msg/roundtrip_a_to_b",
                     test_msg_roundtrip_a_to_b);

    return g_test_run ();
}
