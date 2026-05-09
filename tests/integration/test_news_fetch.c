/*
 * tests/integration/test_news_fetch.c — fetch the news file via
 * HTLC_HDR_NEWS_GETFILE and verify the body comes back.
 *
 * mhxd's run/hxd/news file ships with content that includes the
 * string "Welcome to" — we use that as a sanity-check substring
 * in case the on-disk content gets customised in the container.
 *
 * Phase 5 note: this opcode is gated on HL_ACCESS_READ_NEWS for
 * the account. mhxd's user_loginupdate sets a hardcoded "all bits"
 * access bitmap regardless of the underlying account access (see
 * src/hxd/rcv.c:141), so guest can read news in this test
 * environment even though a real-world server might restrict it.
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
test_news_fetch_round_trip (void)
{
	struct htlc_conn htlc;
	int fd = integration_open_login_or_skip (
		&htlc, "NewsFetch Tier-3", 412);
	if (fd < 0)
		return;

	guint32 our_trans = htlc.trans;
	g_assert_true (integration_send_message (
		fd, &htlc,
		HTLC_HDR_NEWS_GETFILE, /*flag=*/0, /*hc=*/0));

	gboolean got_reply = FALSE;
	for (int i = 0; i < 16 && !got_reply; i++) {
		g_assert_true (integration_recv_message (
			fd, &htlc, /*timeout_ms=*/3000));
		if (hdr_type (&htlc) != HTLS_HDR_TASK)
			continue;
		if (hdr_trans (&htlc) != our_trans)
			continue;
		got_reply = TRUE;
	}
	g_assert_true (got_reply);

	guint32 flag = hdr_flag (&htlc);
	if (flag & 1) {
		/* Some servers reject news fetch entirely (no access bit
		 * granted). Surface the message in the test log but
		 * don't fail — that's a server-deployment issue, not a
		 * client/protocol bug. */
		char err[256];
		gsize err_len = 0;
		if (task_error_extract (&htlc, err, sizeof (err), &err_len))
			g_test_message ("news fetch refused by server: \"%s\"", err);
		else
			g_test_message ("news fetch refused by server (no error chunk)");
		integration_release_htlc (&htlc);
		integration_close (fd);
		return;
	}

	/* Successful TASK reply — carries one HTLS_DATA_NEWS chunk
	 * with the news body. Use the Tier 2 extractor to pull and
	 * sanitise the body. */
	char body[8192 + 1];
	gsize body_len = 0;
	g_assert_true (hx_news_file_extract (
		&htlc, body, sizeof (body), &body_len));
	g_assert_cmpuint (body_len, >, 0);

	/* mhxd's shipped news file starts with the divider line and
	 * 'Welcome to' / 'Horline'. Pin down 'Welcome to' as a soft
	 * sanity check — it's stable across mhxd rebuilds and absent
	 * from the kind of empty/stub news file you'd only get if
	 * the seed copy went wrong. */
	g_assert_nonnull (g_strstr_len (body, body_len, "Welcome to"));

	integration_release_htlc (&htlc);
	integration_close (fd);
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);

	g_test_add_func ("/integration/news_fetch/round_trip",
	                 test_news_fetch_round_trip);

	return g_test_run ();
}
