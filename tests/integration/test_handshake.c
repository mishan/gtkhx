/*
 * tests/integration/test_handshake.c — Tier 3 first integration
 * test. Open a TCP connection to the configured Hotline server
 * (mhxd Docker container on localhost:5500 by default), run the
 * 12-byte / 8-byte magic handshake, close the connection.
 *
 * Skip behaviour: if no server is reachable within the connect
 * timeout, the test calls g_test_skip and exits cleanly. That way
 * the suite is useful on dev machines with mhxd running and stays
 * silent on machines without Docker.
 */

#include "config.h"
#include <unistd.h>
#include <glib.h>
#include "integration_harness.h"

static void
test_handshake_smoke (void)
{
	int fd = integration_open_or_skip ();
	if (fd < 0)
		return;
	/* integration_open_or_skip already ran the magic handshake; if
	 * we got fd >= 0 the exchange succeeded. Closing here exercises
	 * the disconnect path on the server too — mhxd should accept
	 * the close cleanly without error logs. */
	integration_close (fd);
}

/* Sanity check: a second connection on the same fd lifecycle works
 * (catches accidental "leak the listening socket" / "the harness's
 * connect leaves something half-open" bugs). */
static void
test_handshake_two_connections_in_a_row (void)
{
	int fd1 = integration_open_or_skip ();
	if (fd1 < 0)
		return;
	integration_close (fd1);

	int fd2 = integration_open_or_skip ();
	if (fd2 < 0)
		return;
	integration_close (fd2);
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);

	g_test_add_func ("/integration/handshake/smoke",
	                 test_handshake_smoke);
	g_test_add_func ("/integration/handshake/two_connections_in_a_row",
	                 test_handshake_two_connections_in_a_row);

	return g_test_run ();
}
