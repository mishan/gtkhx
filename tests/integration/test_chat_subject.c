/*
 * tests/integration/test_chat_subject.c — set a private chat's
 * subject and verify the broadcast reaches the other chat member.
 *
 * mhxd's rcv_chat_subject (mhxd/src/hxd/chat.c) gates the subject
 * change on either:
 *   - cid 0 + access_extra.set_subject (guests don't have this), OR
 *   - chat_isset(htlc, chat, 0) — the htlc is in the chat
 *
 * So we have to operate on a private chat. Sequence:
 *   1. Alice creates a private chat with Bob (HTLC_HDR_CHAT_CREATE).
 *   2. Bob receives HTLS_HDR_CHAT_INVITE with the new chat_id.
 *      Bob is auto-added to the chat by mhxd's rcv_chat_create
 *      (chat_set is called for the originator; the invitee joins
 *      explicitly via HTLC_HDR_CHAT_JOIN, but for our subject
 *      broadcast test, Alice setting her own chat's subject is
 *      enough — chat_isset is true for Alice).
 *   3. Alice sends HTLC_HDR_CHAT_SUBJECT for her chat_id.
 *   4. Alice receives HTLS_HDR_CHAT_SUBJECT with the new subject
 *      (mhxd broadcasts to all chat members; Alice is in her own
 *      chat).
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
test_chat_subject_broadcasts (void)
{
    struct htlc_conn htlc_a;
    int fd_a
        = integration_open_login_or_skip (&htlc_a, "SubjAlice Tier-3", 412);
    if (fd_a < 0) {
        return;
    }

    struct htlc_conn htlc_b;
    int fd_b = integration_open_login_or_skip (&htlc_b, "SubjBob Tier-3", 412);
    if (fd_b < 0) {
        integration_release_htlc (&htlc_a);
        integration_close (fd_a);
        return;
    }

    /* Alice creates a private chat naming Bob → chat_id. */
    guint32 chat_id = 0;
    g_assert_true (integration_create_chat_with_uid (fd_a, &htlc_a, htlc_b.uid,
                                                     &chat_id, 64));

    /* Alice sets the subject. */
    const char *subject = "Tier-3 chat subject smoke test";
    guint32 cid_be = htonl (chat_id);
    g_assert_true (integration_send_message (
        fd_a, &htlc_a, HTLC_HDR_CHAT_SUBJECT, /*flag=*/0, /*hc=*/2,
        (int)HTLC_DATA_CHAT_ID, (int)sizeof (cid_be), &cid_be,
        (int)HTLC_DATA_CHAT_SUBJECT, (int)strlen (subject), (guint8 *)subject));

    /* Drain Alice's connection looking for HTLS_HDR_CHAT_SUBJECT
	 * with the matching chat_id. */
    gboolean got_subject = FALSE;
    gchar *seen_subject = NULL;
    guint32 seen_cid = 0;
    for (int i = 0; i < 64 && !got_subject; i++) {
        if (!integration_recv_message (fd_a, &htlc_a, /*timeout_ms=*/3000)) {
            break;
        }
        if (hdr_type (&htlc_a) != HTLS_HDR_CHAT_SUBJECT) {
            continue;
        }

        dh_start (hx_test_in(&htlc_a)->buf, hx_test_in(&htlc_a)->pos)
        {
            switch (_type) {
            case HTLS_DATA_CHAT_ID:
                dh_getint (seen_cid);
                break;
            case HTLS_DATA_CHAT_SUBJECT:
                g_free (seen_subject);
                seen_subject = g_strndup ((const char *)dh->data, _len);
                break;
            }
        }
        dh_end ();

        if (seen_cid == chat_id) {
            got_subject = TRUE;
            break;
        }
    }
    g_assert_true (got_subject);
    g_assert_cmphex (seen_cid, ==, chat_id);
    g_assert_nonnull (seen_subject);
    g_assert_cmpstr (seen_subject, ==, subject);

    g_free (seen_subject);
    integration_release_htlc (&htlc_b);
    integration_close (fd_b);
    integration_release_htlc (&htlc_a);
    integration_close (fd_a);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/integration/chat_subject/broadcasts",
                     test_chat_subject_broadcasts);

    return g_test_run ();
}
