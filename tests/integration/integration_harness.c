/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <glib.h>
#include "compat.h"      /* PACKED — required before hotline.h */
#include "hotline.h"
#include "integration_harness.h"

static const char *
test_host (void)
{
	const char *h = g_getenv ("GTKHX_TEST_HOST");
	return h && *h ? h : "127.0.0.1";
}

static int
test_port (void)
{
	const char *p = g_getenv ("GTKHX_TEST_PORT");
	if (!p || !*p) return 5500;
	int v = atoi (p);
	return (v > 0 && v < 65536) ? v : 5500;
}

/* Connect with a short timeout. The naive blocking connect()
 * doesn't take a timeout argument, so we set the socket to non-
 * blocking, kick off the connect, select() on writability with
 * the timeout, then check SO_ERROR. */
static int
connect_with_timeout (const char *host, int port, int timeout_ms)
{
	struct addrinfo hints = { 0 };
	hints.ai_family   = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	char port_str[16];
	g_snprintf (port_str, sizeof (port_str), "%d", port);

	struct addrinfo *res = NULL;
	int rc = getaddrinfo (host, port_str, &hints, &res);
	if (rc != 0)
		return -1;

	int fd = -1;
	for (struct addrinfo *a = res; a; a = a->ai_next) {
		fd = socket (a->ai_family, a->ai_socktype, a->ai_protocol);
		if (fd < 0) continue;

		int flags = fcntl (fd, F_GETFL, 0);
		fcntl (fd, F_SETFL, flags | O_NONBLOCK);

		if (connect (fd, a->ai_addr, a->ai_addrlen) == 0)
			break;     /* connected immediately */

		if (errno != EINPROGRESS) {
			close (fd);
			fd = -1;
			continue;
		}

		fd_set wfds;
		FD_ZERO (&wfds);
		FD_SET (fd, &wfds);
		struct timeval tv = {
			.tv_sec  = timeout_ms / 1000,
			.tv_usec = (timeout_ms % 1000) * 1000,
		};
		int sr = select (fd + 1, NULL, &wfds, NULL, &tv);
		if (sr <= 0) {
			close (fd);
			fd = -1;
			continue;
		}

		int err = 0;
		socklen_t errlen = sizeof (err);
		if (getsockopt (fd, SOL_SOCKET, SO_ERROR, &err, &errlen) < 0
		    || err != 0) {
			close (fd);
			fd = -1;
			continue;
		}

		/* Restore blocking mode for the rest of the harness. */
		fcntl (fd, F_SETFL, flags);
		break;
	}
	freeaddrinfo (res);
	return fd;
}

int
integration_connect (void)
{
	return connect_with_timeout (test_host (), test_port (),
	                             /*timeout_ms=*/2000);
}

gboolean
integration_recv (int fd, void *buf, gsize len)
{
	guint8 *p = buf;
	gsize remaining = len;

	while (remaining > 0) {
		fd_set rfds;
		FD_ZERO (&rfds);
		FD_SET (fd, &rfds);
		struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
		int sr = select (fd + 1, &rfds, NULL, NULL, &tv);
		if (sr <= 0)
			return FALSE;

		ssize_t n = read (fd, p, remaining);
		if (n <= 0)
			return FALSE;
		p += n;
		remaining -= (gsize) n;
	}
	return TRUE;
}

gboolean
integration_send (int fd, const void *buf, gsize len)
{
	const guint8 *p = buf;
	gsize remaining = len;

	while (remaining > 0) {
		ssize_t n = write (fd, p, remaining);
		if (n < 0) {
			if (errno == EINTR) continue;
			return FALSE;
		}
		if (n == 0)
			return FALSE;
		p += n;
		remaining -= (gsize) n;
	}
	return TRUE;
}

gboolean
integration_handshake (int fd)
{
	/* HTLC_MAGIC = "TRTPHOTL\0\1\0\2" (12 bytes). */
	if (!integration_send (fd, HTLC_MAGIC, HTLC_MAGIC_LEN))
		return FALSE;

	/* HTLS_MAGIC = "TRTP\0\0\0\0" (8 bytes). */
	guint8 reply[HTLS_MAGIC_LEN];
	if (!integration_recv (fd, reply, sizeof (reply)))
		return FALSE;

	return memcmp (reply, HTLS_MAGIC, HTLS_MAGIC_LEN) == 0;
}

void
integration_close (int fd)
{
	if (fd >= 0)
		close (fd);
}

int
integration_open_or_skip (void)
{
	int fd = integration_connect ();
	if (fd < 0) {
		gchar *msg = g_strdup_printf (
			"integration server not reachable at %s:%d "
			"(set GTKHX_TEST_HOST / GTKHX_TEST_PORT to change). "
			"Run `docker run -p 5500:5500 gtkhx-mhxd` from "
			"tests/mhxd/ to bring up a server.",
			test_host (), test_port ());
		g_test_skip (msg);
		g_free (msg);
		return -1;
	}

	if (!integration_handshake (fd)) {
		integration_close (fd);
		g_test_fail_printf (
			"connected to %s:%d but the magic-handshake "
			"exchange failed — is this actually a Hotline server?",
			test_host (), test_port ());
		return -1;
	}

	return fd;
}
