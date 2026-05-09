/*
 * tests/integration/test_news_post.c — post to news, then fetch
 * the news file back, verify our post is in the body.
 *
 * Sequence:
 *   1. Login.
 *   2. Send HTLC_HDR_NEWS_POST with HTLC_DATA_NEWS_POST chunk
 *      carrying our post text. mhxd's rcv_news_post prepends the
 *      post (with a "From <name>" header) to the news file on
 *      disk.
 *   3. Send HTLC_HDR_NEWS_GETFILE.
 *   4. Drain to the TASK reply matching the GETFILE trans, parse
 *      the body, assert it contains our post text.
 *
 * Also (separately) drains for the unsolicited HTLS_HDR_NEWS_POST
 * broadcast that mhxd sends to all logged-in clients when someone
 * posts.
 *
 * The on-disk news file is shared across the container's lifetime,
 * so this test mutates server state. Each test run uses a unique
 * marker string (with the test name + a random suffix) so
 * re-running in the same container doesn't false-positive on
 * earlier runs' content.
 */

#include "config.h"
#include <string.h>
#include <stdio.h>
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

static void
test_news_post_then_fetch (void)
{
	struct htlc_conn htlc;
	int fd = integration_open_login_or_skip (
		&htlc, "Poster Tier-3", 412);
	if (fd < 0)
		return;

	/* Build a unique marker per run so re-running this test
	 * doesn't false-positive on the previous run's residue
	 * already in the news file. */
	gchar *marker = g_strdup_printf (
		"Tier-3 news-post marker %u",
		(guint) g_random_int ());

	g_assert_true (integration_send_message (
		fd, &htlc,
		HTLC_HDR_NEWS_POST, /*flag=*/0, /*hc=*/1,
		(int) HTLC_DATA_NEWS_POST,
		(int) strlen (marker), (guint8 *) marker));

	/* Now fetch the news file back. mhxd's NEWS_POST handler
	 * sends an empty TASK ack first, then broadcasts
	 * HTLS_HDR_NEWS_POST to all clients. We drain through those
	 * via our trans match on the GETFILE reply. */
	guint32 fetch_trans = htlc.trans;
	g_assert_true (integration_send_message (
		fd, &htlc,
		HTLC_HDR_NEWS_GETFILE, /*flag=*/0, /*hc=*/0));

	gboolean got_reply = FALSE;
	for (int i = 0; i < 64 && !got_reply; i++) {
		g_assert_true (integration_recv_message (
			fd, &htlc, /*timeout_ms=*/3000));
		if (hdr_type (&htlc) != HTLS_HDR_TASK)
			continue;
		if (hdr_trans (&htlc) != fetch_trans)
			continue;
		got_reply = TRUE;
	}
	g_assert_true (got_reply);

	char body[8192 + 1];
	gsize body_len = 0;
	g_assert_true (hx_news_file_extract (
		&htlc, body, sizeof (body), &body_len));

	/* Our marker should be somewhere in the body. mhxd prepends
	 * a "From <name>" header line per post, so the body grew. */
	g_assert_nonnull (g_strstr_len (body, body_len, marker));

	g_free (marker);
	integration_release_htlc (&htlc);
	integration_close (fd);
}

/* Verify the unsolicited NEWS_POST broadcast: when client A posts,
 * client B (logged in) receives HTLS_HDR_NEWS_POST with the post
 * body. */
static void
test_news_post_broadcasts_to_other_clients (void)
{
	struct htlc_conn htlc_a;
	int fd_a = integration_open_login_or_skip (
		&htlc_a, "PosterAlice Tier-3", 412);
	if (fd_a < 0)
		return;

	struct htlc_conn htlc_b;
	int fd_b = integration_open_login_or_skip (
		&htlc_b, "PosterBob Tier-3", 412);
	if (fd_b < 0) {
		integration_release_htlc (&htlc_a);
		integration_close (fd_a);
		return;
	}

	gchar *marker = g_strdup_printf (
		"Tier-3 news-broadcast marker %u",
		(guint) g_random_int ());

	g_assert_true (integration_send_message (
		fd_a, &htlc_a,
		HTLC_HDR_NEWS_POST, /*flag=*/0, /*hc=*/1,
		(int) HTLC_DATA_NEWS_POST,
		(int) strlen (marker), (guint8 *) marker));

	/* On B's connection, drain looking for HTLS_HDR_NEWS_POST. */
	gboolean got_post = FALSE;
	for (int i = 0; i < 64 && !got_post; i++) {
		if (!integration_recv_message (
				fd_b, &htlc_b, /*timeout_ms=*/3000))
			break;
		if (hdr_type (&htlc_b) != HTLS_HDR_NEWS_POST)
			continue;
		got_post = TRUE;
	}
	g_assert_true (got_post);

	/* Walk the chunks via dh_start to find the NEWS body. */
	gboolean found_marker = FALSE;
	dh_start (&htlc_b) {
		if (_type != HTLS_DATA_NEWS)
			continue;
		if (g_strstr_len ((const char *) dh->data, _len, marker))
			found_marker = TRUE;
	} dh_end ();
	g_assert_true (found_marker);

	g_free (marker);
	integration_release_htlc (&htlc_b);
	integration_close (fd_b);
	integration_release_htlc (&htlc_a);
	integration_close (fd_a);
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);

	g_test_add_func ("/integration/news_post/then_fetch",
	                 test_news_post_then_fetch);
	g_test_add_func ("/integration/news_post/broadcasts_to_other_clients",
	                 test_news_post_broadcasts_to_other_clients);

	return g_test_run ();
}
