/*
 * tests/integration/test_msg_self.c — pin down what mhxd does when
 * a client sends HTLC_HDR_MSG addressed to their own uid.
 *
 * This is an edge case: the GtkHx UI never invokes self-msg, but
 * the wire protocol doesn't forbid it. Servers tend to either:
 *
 *   (a) Echo the MSG back (the "natural" handler walk treats the
 *       sender like any other recipient).
 *   (b) Suppress it (handler explicitly skips when sender == target).
 *   (c) Task-error it.
 *
 * mhxd's rcv_msg dispatches via the same loop as chat broadcast,
 * but (per src/hxd/chat.c) explicitly compares htlc to the target
 * before sending. We test that the connection stays clean — no
 * task-error explosion, no stuck stream — by round-tripping a PING
 * after the self-msg attempt and asserting the pong correlates by
 * trans.
 *
 * The test is tolerant about whether the MSG is echoed back: we
 * drain a small window and let either outcome (echo received OR
 * silently dropped) pass, then probe the stream with a PING. Any
 * follow-up framing breakage shows up as a missed pong-trans.
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

static guint32
hdr_type (const struct htlc_conn *htlc)
{
    const struct hl_hdr *h = (const struct hl_hdr *)htlc->in.buf;
    return ntohl (h->type);
}

static guint32
hdr_trans (const struct htlc_conn *htlc)
{
    const struct hl_hdr *h = (const struct hl_hdr *)htlc->in.buf;
    return ntohl (h->trans);
}

static guint32
hdr_flag (const struct htlc_conn *htlc)
{
    const struct hl_hdr *h = (const struct hl_hdr *)htlc->in.buf;
    return ntohl (h->flag);
}

static void
test_msg_self_doesnt_break_stream (void)
{
    struct htlc_conn htlc;
    int fd = integration_open_login_or_skip (&htlc, "MsgSelf Tier-3", 412);
    if (fd < 0) {
        return;
    }

    /* Send a PM addressed to our own uid. */
    guint16 self_uid_be = htons (htlc.uid);
    const char *body = "tier-3 self-msg edge case";
    g_assert_true (integration_send_message (
        fd, &htlc, HTLC_HDR_MSG, /*flag=*/0, /*hc=*/2, (int)HTLC_DATA_UID,
        (int)sizeof (self_uid_be), &self_uid_be, (int)HTLC_DATA_MSG,
        (int)strlen (body), (guint8 *)body));

    /* Drain a short window. We don't care whether the server echoes
	 * the MSG back, task-errors it, or drops it silently — only that
	 * whatever it does happens cleanly. Stop draining as soon as we
	 * see anything (or after a brief idle). */
    int drained = 0;
    gboolean saw_self_msg = FALSE;
    gboolean saw_task_error = FALSE;
    for (int i = 0; i < 8; i++) {
        if (!integration_recv_message (fd, &htlc, /*timeout_ms=*/500)) {
            break;
        }
        drained++;
        if (hdr_type (&htlc) == HTLS_HDR_MSG) {
            struct hx_msg_msg pm;
            if (hx_msg_extract (&htlc, &pm) && pm.uid == htlc.uid) {
                saw_self_msg = TRUE;
            }
        } else if (hdr_type (&htlc) == HTLS_HDR_TASK
                   && (hdr_flag (&htlc) & 1)) {
            saw_task_error = TRUE;
        }
    }
    g_test_message ("msg-self drain: %d frame(s); echo=%s task_error=%s",
                    drained, saw_self_msg ? "yes" : "no",
                    saw_task_error ? "yes" : "no");

    /* Probe the stream — a PING should still round-trip cleanly.
	 * A protocol-framing slip caused by the self-msg would surface
	 * as either no pong arriving (timeout) or a wrong-trans frame. */
    guint32 ping_trans = htlc.trans;
    g_assert_true (integration_send_message (fd, &htlc, HTLC_HDR_PING,
                                             /*flag=*/0, /*hc=*/0));

    gboolean got_pong = FALSE;
    for (int i = 0; i < 16 && !got_pong; i++) {
        g_assert_true (
            integration_recv_message (fd, &htlc, /*timeout_ms=*/3000));
        if (hdr_type (&htlc) != HTLS_HDR_TASK) {
            continue;
        }
        if (hdr_trans (&htlc) != ping_trans) {
            continue;
        }
        got_pong = TRUE;
        g_assert_cmphex (hdr_flag (&htlc) & 1, ==, 0);
    }
    g_assert_true (got_pong);

    integration_release_htlc (&htlc);
    integration_close (fd);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/integration/msg_self/doesnt_break_stream",
                     test_msg_self_doesnt_break_stream);
    return g_test_run ();
}
