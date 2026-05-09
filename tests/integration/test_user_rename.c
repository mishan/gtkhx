/*
 * tests/integration/test_user_rename.c — verify a HTLC_HDR_USER_CHANGE
 * (rename / icon change) broadcasts to other connected clients.
 *
 * Sequence:
 *   1. Alice and Bob both login (Alice as "RenameAlice Tier-3").
 *   2. Alice sends HTLC_HDR_USER_CHANGE with a new icon and a new
 *      name ("RenameAlice 2.0").
 *   3. Bob's connection receives HTLS_HDR_USER_CHANGE carrying
 *      Alice's uid + the new name + new icon.
 *
 * The Tier 2 hx_user_change_extract helper does the chunk parse;
 * we filter the broadcast by uid (Alice's session uid) to walk past
 * any cross-talk from other concurrent integration test binaries.
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

/* Drain looking for an HTLS_HDR_USER_CHANGE matching `wanted_uid`
 * AND with the matching name (so we ignore Alice's join broadcast
 * which arrived earlier with her old name). */
static gboolean
drain_for_rename (int fd, struct htlc_conn *htlc,
                  guint16 wanted_uid, const char *wanted_name,
                  struct hx_user_change_msg *out,
                  int max_messages)
{
	for (int i = 0; i < max_messages; i++) {
		if (!integration_recv_message (fd, htlc, /*timeout_ms=*/3000))
			return FALSE;
		if (hdr_type (htlc) != HTLS_HDR_USER_CHANGE)
			continue;
		if (!hx_user_change_extract (htlc, out))
			continue;
		if (out->uid != wanted_uid)
			continue;
		/* Distinguish the rename USER_CHANGE from the on-join
		 * USER_CHANGE Alice received when Bob logged in (whose
		 * name field is Bob's, not the renamed Alice's). Match
		 * on the wanted name. */
		if (strcmp (out->name, wanted_name) != 0)
			continue;
		return TRUE;
	}
	return FALSE;
}

static void
test_user_rename_broadcasts (void)
{
	struct htlc_conn htlc_a;
	int fd_a = integration_open_login_or_skip (
		&htlc_a, "RenameAlice Tier-3", 412);
	if (fd_a < 0)
		return;

	struct htlc_conn htlc_b;
	int fd_b = integration_open_login_or_skip (
		&htlc_b, "RenameBob Tier-3", 412);
	if (fd_b < 0) {
		integration_release_htlc (&htlc_a);
		integration_close (fd_a);
		return;
	}

	/* Alice changes her name and icon. mhxd's rcv_user_change
	 * compares the chunk values to her current htlc state; if
	 * either differs it sets `diff` and broadcasts via
	 * snd_user_change. */
	const char *new_name = "RenameAlice 2.0";
	guint16 new_icon_be = htons (999);   /* differs from 412 */
	g_assert_true (integration_send_message (
		fd_a, &htlc_a,
		HTLC_HDR_USER_CHANGE, /*flag=*/0, /*hc=*/2,
		(int) HTLC_DATA_ICON, (int) sizeof (new_icon_be),
			&new_icon_be,
		(int) HTLC_DATA_NAME, (int) strlen (new_name),
			(guint8 *) new_name));

	/* Bob's connection should receive HTLS_HDR_USER_CHANGE with
	 * Alice's uid + the new name + new icon. */
	struct hx_user_change_msg uc;
	g_assert_true (drain_for_rename (
		fd_b, &htlc_b, htlc_a.uid, new_name, &uc,
		/*max_messages=*/64));

	g_assert_cmphex  (uc.uid,  ==, htlc_a.uid);
	g_assert_cmpstr  (uc.name, ==, new_name);
	g_assert_cmphex  (uc.icon, ==, 999);

	integration_release_htlc (&htlc_b);
	integration_close (fd_b);
	integration_release_htlc (&htlc_a);
	integration_close (fd_a);
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);

	g_test_add_func ("/integration/user_rename/broadcasts",
	                 test_user_rename_broadcasts);

	return g_test_run ();
}
