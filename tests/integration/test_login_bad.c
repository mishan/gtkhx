/*
 * tests/integration/test_login_bad.c — login with a non-existent
 * account name. Verify the server rejects the login.
 *
 * mhxd has two ways it can reject:
 *
 *   (a) Task-error: HTLS_HDR_TASK + flag=1 + HTLS_DATA_TASKERROR.
 *       Some forks / future servers might do this.
 *   (b) Connection close: mhxd's actual behaviour (rcv_login at
 *       src/hxd/rcv.c:1420-1423 — account_read failure goes
 *       straight to htlc_close, no error frame). The client sees
 *       a zero-byte read on the next recv.
 *
 * The contract worth pinning down is the NEGATIVE one:
 * HTLS_HDR_USER_SELFINFO must NOT arrive. SELFINFO is the
 * "login accepted, here's your session" milestone; if it shows up
 * for a non-existent account, the server's auth gate has slipped.
 *
 * The default mhxd container ships only `admin` and `guest`. We
 * send a login for "doesnotexist-tier3" — a name that won't ever
 * collide with a real account.
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
hdr_flag (const struct htlc_conn *htlc)
{
    const struct hl_hdr *h = (const struct hl_hdr *)htlc->in.buf;
    return ntohl (h->flag);
}

/* hl_code is bitwise NOT — duplicate it locally rather than dragging
 * the helper out of integration_harness.c. The encode is trivial so
 * a 4-line inline version isn't worth a header export. */
static void
hl_encode_local (guint8 *dst, const char *src, gsize len)
{
    for (gsize i = 0; i < len; i++) {
        dst[i] = (guint8)~src[i];
    }
}

static void
test_login_bad_account_rejected (void)
{
    int fd = integration_connect ();
    if (fd < 0) {
        g_test_skip ("server unreachable");
        return;
    }
    g_assert_true (integration_handshake (fd));

    struct htlc_conn htlc;
    memset (&htlc, 0, sizeof htlc);

    const char *bogus = "doesnotexist-tier3";
    guint8 encbogus[64];
    gsize blen = strlen (bogus);
    g_assert_cmpuint (blen, <=, sizeof (encbogus));
    hl_encode_local (encbogus, bogus, blen);

    const char *display = "BadLoginTier-3";
    guint16 icon_be = htons (412);

    guint32 our_trans = htlc.trans;
    g_assert_true (integration_send_message (
        fd, &htlc, HTLC_HDR_LOGIN, /*flag=*/0, /*hc=*/3, (int)HTLC_DATA_ICON,
        (int)sizeof (icon_be), &icon_be, (int)HTLC_DATA_LOGIN, (int)blen,
        encbogus, (int)HTLC_DATA_NAME, (int)strlen (display),
        (guint8 *)display));

    (void)our_trans; /* not asserted; mhxd doesn't always echo */

    /* Drain everything the server hands us before either:
	 *   - a task-error frame (rejection mode (a)), or
	 *   - the recv loop returns FALSE because the server closed
	 *     the connection (rejection mode (b) — mhxd's real path).
	 * Whichever happens first counts as a successful rejection.
	 *
	 * Crucially we record whether SELFINFO ever appeared; if it
	 * did, the server let us in despite the bad account, and that
	 * is the failure case worth catching. */
    gboolean got_error = FALSE;
    gboolean conn_closed = FALSE;
    gboolean got_selfinfo = FALSE;

    for (int i = 0; i < 32 && !got_error && !conn_closed; i++) {
        if (!integration_recv_message (fd, &htlc, /*timeout_ms=*/3000)) {
            conn_closed = TRUE;
            break;
        }
        if (hdr_type (&htlc) == HTLS_HDR_USER_SELFINFO) {
            got_selfinfo = TRUE;
            break;
        }
        if (hdr_type (&htlc) != HTLS_HDR_TASK) {
            continue;
        }
        if ((hdr_flag (&htlc) & 1) == 0) {
            continue;
        }
        got_error = TRUE;

        char err[256];
        gsize err_len = 0;
        g_assert_true (task_error_extract (&htlc, err, sizeof (err), &err_len));
        g_assert_cmpuint (err_len, >, 0);
        g_test_message ("server rejected bad login (task-error): \"%s\"", err);
    }

    if (conn_closed) {
        g_test_message ("server rejected bad login (connection close)");
    }

    g_assert_false (got_selfinfo);
    g_assert_true (got_error || conn_closed);

    integration_release_htlc (&htlc);
    integration_close (fd);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/integration/login_bad/rejected",
                     test_login_bad_account_rejected);
    return g_test_run ();
}
