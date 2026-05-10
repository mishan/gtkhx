/*
 * tests/integration/test_unauthorized_opcode.c — sending a
 * privileged opcode (FILE_MKDIR) as guest is silently dropped
 * by the mhxd dispatcher.
 *
 * mhxd's main rcv loop (mhxd/src/hxd/rcv.c around line 380+)
 * gates each privileged HTLC_HDR_* on the corresponding access
 * bit BEFORE setting htlc->rcv to a handler. If the bit is unset,
 * the handler never runs and no reply is sent. FILE_MKDIR is
 * gated on htlc->access.create_folders.
 *
 * The default upstream guest UserData has all eight byte-0
 * access bits set, which makes FILE_MKDIR pass through and gives
 * a misleading 'guest can mkdir' result. Our Dockerfile patches
 * guest's binary UserData at byte 4 with a 0x60 mask — keeping
 * download_files (bit 5) and upload_files (bit 6), clearing
 * everything else including create_folders (bit 2). See the
 * comment block in tests/mhxd/Dockerfile next to the dd patch.
 *
 * Test contract:
 *   1. Login as guest.
 *   2. Send HTLC_HDR_FILE_MKDIR for some name.
 *   3. Drain a short window — assert no TASK frame correlated to
 *      our trans arrives. The dispatcher drops it silently.
 *   4. Round-trip a PING; the pong-trans must match. Proves the
 *      dispatcher still consumed our message cleanly without
 *      leaving stale framing behind.
 *
 * Catches a regression where the dispatcher accidentally lets a
 * privileged opcode through when the access bit is clear.
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
	 * matches our MKDIR. With guest's create_folders bit cleared
	 * by the Dockerfile patch, mhxd's dispatcher refuses to set
	 * htlc->rcv for FILE_MKDIR — no handler runs, no reply is
	 * sent. We expect the drain to time out empty. */
	gboolean got_mkdir_reply = FALSE;
	for (int i = 0; i < 8; i++) {
		if (!integration_recv_message (
				fd, &htlc, /*timeout_ms=*/500))
			break;
		if (hdr_type (&htlc) != HTLS_HDR_TASK)
			continue;
		if (hdr_trans (&htlc) != mkdir_trans)
			continue;
		got_mkdir_reply = TRUE;
		break;
	}
	g_assert_false (got_mkdir_reply);

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
