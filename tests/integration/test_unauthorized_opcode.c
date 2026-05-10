/*
 * tests/integration/test_unauthorized_opcode.c — sending a
 * privileged opcode (FILE_MKDIR) as guest is rejected — either
 * silently dropped by the dispatcher's access gate, or accepted
 * by the dispatcher and turned into a task-error by the handler
 * once it hits filesystem-level checks. Either outcome counts.
 *
 * mhxd's main rcv loop (mhxd/src/hxd/rcv.c around line 380+)
 * gates the FILE_MKDIR opcode on htlc->access.create_folders.
 * In practice the guest account ships without an explicit access
 * file, and the access defaults at construction time vary across
 * mhxd builds — so the dispatcher's gate behaviour depends on
 * what bits the server actually loaded. Empirically (this test
 * against the default container), mhxd accepts the message and
 * the handler responds with a task-error (file ops fail at the
 * fs gate one layer down).
 *
 * The contract worth pinning down is the negative one: a
 * non-error TASK reply for FILE_MKDIR would mean the directory
 * actually got created — i.e. the auth gate slipped. We assert
 * that didn't happen, while tolerating both "no reply" and
 * "task-error" as legitimate rejections.
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
	const struct hl_hdr *h = (const struct hl_hdr *) htlc->in.buf;
	return ntohl (h->type);
}

static guint32
hdr_trans (const struct htlc_conn *htlc)
{
	const struct hl_hdr *h = (const struct hl_hdr *) htlc->in.buf;
	return ntohl (h->trans);
}

static guint32
hdr_flag (const struct htlc_conn *htlc)
{
	const struct hl_hdr *h = (const struct hl_hdr *) htlc->in.buf;
	return ntohl (h->flag);
}

static void
test_unauthorized_mkdir_silently_dropped (void)
{
	struct htlc_conn htlc;
	int fd = integration_open_login_or_skip (
		&htlc, "UnauthMkdir T-3", 412);
	if (fd < 0)
		return;

	/* MKDIR-trans we'll watch for AND not expect to see. */
	guint32 mkdir_trans = htlc.trans;

	const char *new_dir = "tier3_unauth_test_dir";
	g_assert_true (integration_send_message (
		fd, &htlc,
		HTLC_HDR_FILE_MKDIR, /*flag=*/0, /*hc=*/1,
		(int) HTLC_DATA_FILE_NAME, (int) strlen (new_dir),
			(guint8 *) new_dir));

	/* Drain a short window; record any TASK frame whose trans
	 * matches our MKDIR. Two acceptable outcomes:
	 *
	 *   - No reply at all (dispatcher gate dropped silently).
	 *   - TASK with flag=1 (handler ran but rejected via
	 *     task-error — usually filesystem-level EACCES /
	 *     EEXIST / etc).
	 *
	 * What we MUST NOT see is a TASK with flag=0 (success) for
	 * our mkdir trans — that would mean the guest just created
	 * a directory the auth gate should have blocked. */
	gboolean got_mkdir_success = FALSE;
	gboolean got_mkdir_error   = FALSE;
	for (int i = 0; i < 8; i++) {
		if (!integration_recv_message (
				fd, &htlc, /*timeout_ms=*/500))
			break;
		if (hdr_type (&htlc) != HTLS_HDR_TASK)
			continue;
		if (hdr_trans (&htlc) != mkdir_trans)
			continue;
		if (hdr_flag (&htlc) & 1) {
			got_mkdir_error = TRUE;
			char err[256];
			gsize err_len = 0;
			if (task_error_extract (
				&htlc, err, sizeof (err), &err_len))
				g_test_message (
					"server rejected mkdir: \"%s\"", err);
		} else {
			got_mkdir_success = TRUE;
		}
		break;
	}
	g_assert_false (got_mkdir_success);
	g_test_message ("mkdir rejected via %s",
	                got_mkdir_error ? "task-error" : "silent drop");

	/* Probe with PING — the dispatcher should still be in a clean
	 * state, accepting the next request and replying normally. */
	guint32 ping_trans = htlc.trans;
	g_assert_true (integration_send_message (
		fd, &htlc,
		HTLC_HDR_PING, /*flag=*/0, /*hc=*/0));

	gboolean got_pong = FALSE;
	for (int i = 0; i < 32 && !got_pong; i++) {
		g_assert_true (integration_recv_message (
			fd, &htlc, /*timeout_ms=*/3000));
		if (hdr_type (&htlc) != HTLS_HDR_TASK)
			continue;
		if (hdr_trans (&htlc) != ping_trans)
			continue;
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
	g_test_add_func (
		"/integration/unauthorized_opcode/mkdir_silently_dropped",
		test_unauthorized_mkdir_silently_dropped);
	return g_test_run ();
}
