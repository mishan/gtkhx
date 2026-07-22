/*
 * tests/integration/test_user_info.c — fetch our own user info via
 * HTLC_HDR_USER_GETINFO and verify the reply structure.
 *
 * mhxd's rcv_user_getinfo (src/hxd/rcv.c) replies with
 * HTLS_HDR_TASK carrying:
 *   HTLS_DATA_USER_INFO  — multi-line text describing the user
 *   HTLS_DATA_NAME       — the user's display name
 *
 * Send the request with our own uid (mhxd's `self_info` config
 * option default-allows self-info even when the access bit isn't
 * set) and assert both chunks come back, the name matches, and
 * the info text contains our login.
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
test_user_info_self (void)
{
    struct htlc_conn htlc;
    int fd = integration_open_login_or_skip (&htlc, "Info Tier-3", 412);
    if (fd < 0) {
        return;
    }

    guint16 self_uid_be = htons (htlc.uid);
    guint32 our_trans = htlc.trans;

    g_assert_true (integration_send_message (
        fd, &htlc, HTLC_HDR_USER_GETINFO, /*flag=*/0, /*hc=*/1,
        (int)HTLC_DATA_UID, (int)sizeof (self_uid_be), &self_uid_be));

    /* Drain to TASK reply matching our trans. */
    g_assert_true (integration_drain_until_task_trans (
        fd, &htlc, our_trans, 64));
    g_assert_cmphex (hdr_flag (&htlc) & 1, ==, 0);

    /* Walk the chunks; expect HTLS_DATA_USER_INFO + HTLS_DATA_NAME. */
    gchar *info_text = NULL;
    gsize info_len = 0;
    gchar *name_text = NULL;
    gsize name_len = 0;
    dh_start (htlc.in.buf, htlc.in.pos)
    {
        switch (_type) {
        case HTLS_DATA_USER_INFO:
            info_text = g_strndup ((const char *)dh->data, _len);
            info_len = _len;
            break;
        case HTLS_DATA_NAME:
            name_text = g_strndup ((const char *)dh->data, _len);
            name_len = _len;
            break;
        }
    }
    dh_end ();

    g_assert_nonnull (info_text);
    g_assert_nonnull (name_text);
    g_assert_cmpuint (info_len, >, 0);
    g_assert_cmpuint (name_len, >, 0);

    /* Name should match what we sent. */
    g_assert_cmpstr (name_text, ==, "Info Tier-3");

    /* Info text should mention our login (mhxd's format puts
	 * "login: guest" on its own line; case-sensitive substring
	 * is fine). */
    g_assert_nonnull (g_strstr_len (info_text, info_len, "guest"));

    g_free (info_text);
    g_free (name_text);

    integration_release_htlc (&htlc);
    integration_close (fd);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/integration/user_info/self", test_user_info_self);

    return g_test_run ();
}
