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
#include <stdarg.h>
#include <glib.h>
#include "compat.h" /* PACKED — required before hotline.h */
#include "hotline.h"
#include "protocol.h"
#include "proto_helpers.h"
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
    if (!p || !*p) {
        return 5500;
    }
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
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    char port_str[16];
    g_snprintf (port_str, sizeof (port_str), "%d", port);

    struct addrinfo *res = NULL;
    int rc = getaddrinfo (host, port_str, &hints, &res);
    if (rc != 0) {
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        fd = socket (a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) {
            continue;
        }

        int flags = fcntl (fd, F_GETFL, 0);
        fcntl (fd, F_SETFL, flags | O_NONBLOCK);

        if (connect (fd, a->ai_addr, a->ai_addrlen) == 0) {
            break; /* connected immediately */
        }

        if (errno != EINPROGRESS) {
            close (fd);
            fd = -1;
            continue;
        }

        fd_set wfds;
        FD_ZERO (&wfds);
        FD_SET (fd, &wfds);
        struct timeval tv = {
            .tv_sec = timeout_ms / 1000,
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
        if (sr <= 0) {
            return FALSE;
        }

        ssize_t n = read (fd, p, remaining);
        if (n <= 0) {
            return FALSE;
        }
        p += n;
        remaining -= (gsize)n;
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
            if (errno == EINTR) {
                continue;
            }
            return FALSE;
        }
        if (n == 0) {
            return FALSE;
        }
        p += n;
        remaining -= (gsize)n;
    }
    return TRUE;
}

gboolean
integration_handshake (int fd)
{
    /* HTLC_MAGIC = "TRTPHOTL\0\1\0\2" (12 bytes). */
    if (!integration_send (fd, HTLC_MAGIC, HTLC_MAGIC_LEN)) {
        return FALSE;
    }

    /* HTLS_MAGIC = "TRTP\0\0\0\0" (8 bytes). */
    guint8 reply[HTLS_MAGIC_LEN];
    if (!integration_recv (fd, reply, sizeof (reply))) {
        return FALSE;
    }

    return memcmp (reply, HTLS_MAGIC, HTLS_MAGIC_LEN) == 0;
}

void
integration_close (int fd)
{
    if (fd >= 0) {
        close (fd);
    }
}

/* ---- High-level message helpers --------------------------------- */

gboolean
integration_send_message (int fd, struct htlc_conn *htlc, guint32 type,
                          guint32 flag, int hc, ...)
{
    /* Reset the out buffer so successive sends each pack into a
	 * fresh buffer (otherwise hlpack appends, which would confuse
	 * our 'now write that out' step below). */
    g_free (htlc->out.buf);
    htlc->out.buf = NULL;
    htlc->out.pos = 0;
    htlc->out.len = 0;

    va_list ap;
    va_start (ap, hc);
    hlpack (htlc, type, flag, hc, ap);
    va_end (ap);

    gboolean ok = integration_send (fd, htlc->out.buf, htlc->out.len);

    g_free (htlc->out.buf);
    htlc->out.buf = NULL;
    htlc->out.pos = 0;
    htlc->out.len = 0;
    return ok;
}

gboolean
integration_recv_message (int fd, struct htlc_conn *htlc, int timeout_ms)
{
    /* Reset in buffer. */
    g_free (htlc->in.buf);
    htlc->in.buf = NULL;
    htlc->in.pos = 0;
    htlc->in.len = 0;

    /* Wait for the first header byte to be available. */
    fd_set rfds;
    FD_ZERO (&rfds);
    FD_SET (fd, &rfds);
    struct timeval tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    int sr = select (fd + 1, &rfds, NULL, NULL, &tv);
    if (sr <= 0) {
        return FALSE;
    }

    /* Read the 22-byte hl_hdr first to learn the message length. */
    guint8 hdr_bytes[SIZEOF_HL_HDR];
    if (!integration_recv (fd, hdr_bytes, SIZEOF_HL_HDR)) {
        return FALSE;
    }

    const struct hl_hdr *h = (const struct hl_hdr *)hdr_bytes;
    guint32 wire_len = ntohl (h->len);
    guint16 hc = ntohs (h->hc);

    /* h->len is "data section bytes minus (SIZEOF_HL_HDR -
	 * sizeof(hc))"; back it out to the body byte count.
	 *
	 * Cap at 1 MiB — same MAX_HOTLINE_PACKET_LEN that network.c
	 * enforces — to avoid accidental DoS against the harness when
	 * pointed at a misbehaving server. */
    if (wire_len > MAX_HOTLINE_PACKET_LEN) {
        return FALSE;
    }

    guint32 body_len = 0;
    if (wire_len + (SIZEOF_HL_HDR - sizeof (h->hc)) >= SIZEOF_HL_HDR) {
        body_len = wire_len + (SIZEOF_HL_HDR - sizeof (h->hc)) - SIZEOF_HL_HDR;
    }
    (void)hc; /* hc is read by dh_start via the header bytes. */

    /* Allocate the full message buffer, copy the header in, read
	 * the rest. */
    gsize total = SIZEOF_HL_HDR + body_len;
    htlc->in.buf = g_malloc (total);
    memcpy (htlc->in.buf, hdr_bytes, SIZEOF_HL_HDR);
    if (body_len > 0) {
        if (!integration_recv (fd, htlc->in.buf + SIZEOF_HL_HDR, body_len)) {
            g_free (htlc->in.buf);
            htlc->in.buf = NULL;
            return FALSE;
        }
    }
    htlc->in.pos = total;
    htlc->in.len = total;
    return TRUE;
}

void
integration_release_htlc (struct htlc_conn *htlc)
{
    g_free (htlc->in.buf);
    g_free (htlc->out.buf);
    htlc->in.buf = NULL;
    htlc->out.buf = NULL;
}

/* hl_code is an XOR-with-0xff cipher used by Hotline for the
 * obfuscated login / password fields. Inline copy here so the
 * harness doesn't have to link network.c. */
static void
hl_code_inline (void *dst, const void *src, gsize len)
{
    const guint8 *s = src;
    guint8 *d = dst;
    for (gsize i = 0; i < len; i++) {
        d[i] = ~s[i];
    }
}

gboolean
integration_login_guest (int fd, struct htlc_conn *htlc,
                         const char *display_name, guint16 icon)
{
    const char *login = "guest";
    gsize llen = strlen (login);
    guint8 enclogin[64];
    g_assert_cmpuint (llen, <=, sizeof (enclogin));
    hl_code_inline (enclogin, login, llen);

    guint16 icon_be = htons (icon);
    gsize nlen = strlen (display_name);
    /* Advertise ourselves as Hotline 1.8.5. mhxd uses this in
	 * rcv_login to decide whether to set htlc->access_extra.can_ping
	 * (gated on clientversion >= 150 at src/hxd/rcv.c:1600). Without
	 * this chunk, mhxd silently rejects HTLC_HDR_PING with a task-
	 * error. GtkHx's own login path doesn't send it today, but
	 * sending it here makes the integration suite cover the modern
	 * client behaviour mhxd was designed against — and lets the
	 * ping test exercise its actual contract. */
    guint16 clientversion_be = htons (185);

    return integration_send_message (
        fd, htlc, HTLC_HDR_LOGIN, /*flag=*/0, /*hc=*/4, (int)HTLC_DATA_ICON,
        (int)sizeof (icon_be), &icon_be, (int)HTLC_DATA_LOGIN, (int)llen,
        enclogin, (int)HTLC_DATA_NAME, (int)nlen, (guint8 *)display_name,
        (int)HTLC_DATA_CLIENTVERSION, (int)sizeof (clientversion_be),
        &clientversion_be);
}

/* ---- HTXF subchannel helpers ----------------------------------- */

static int
test_xfer_port (void)
{
    const char *p = g_getenv ("GTKHX_TEST_XFER_PORT");
    if (p && *p) {
        int v = atoi (p);
        if (v > 0 && v < 65536) {
            return v;
        }
    }
    return test_port () + 1;
}

int
integration_connect_xfer (void)
{
    return connect_with_timeout (test_host (), test_xfer_port (),
                                 /*timeout_ms=*/2000);
}

gboolean
integration_send_xfer_hdr (int fd, guint32 ref, guint32 total_size)
{
    guint32 wire[4] = {
        htonl (HTXF_MAGIC_INT),
        htonl (ref),
        htonl (total_size),
        0, /* unknown — always zero */
    };
    return integration_send (fd, wire, sizeof (wire));
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

/* Pull the message type field out of the just-received header. */
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

guint32
integration_drain_until_selfinfo_or_error (int fd, struct htlc_conn *htlc,
                                           int max_messages)
{
    if (max_messages <= 0) {
        max_messages = 8;
    }

    for (int i = 0; i < max_messages; i++) {
        if (!integration_recv_message (fd, htlc, /*timeout_ms=*/3000)) {
            return 0;
        }

        guint32 type = hdr_type (htlc);
        guint32 flag = hdr_flag (htlc);

        if (type == HTLS_HDR_TASK && (flag & 1)) {
            return type; /* task-error: login refused */
        }
        if (type == HTLS_HDR_USER_SELFINFO) {
            return type; /* success */
        }

        /* Otherwise loop — TASK loginreply with version+name,
		 * AGREEMENT, BANNER, etc. */
    }
    return 0;
}

int
integration_open_login_or_skip (struct htlc_conn *htlc,
                                const char *display_name, guint16 icon)
{
    memset (htlc, 0, sizeof (*htlc));

    int fd = integration_open_or_skip ();
    if (fd < 0) {
        return -1;
    }

    if (!integration_login_guest (fd, htlc, display_name, icon)) {
        integration_release_htlc (htlc);
        integration_close (fd);
        g_test_fail_printf ("integration_login_guest failed");
        return -1;
    }

    guint32 type = integration_drain_until_selfinfo_or_error (fd, htlc, 8);

    if (type == HTLS_HDR_TASK) {
        char err[256];
        gsize err_len = 0;
        if (task_error_extract (htlc, err, sizeof (err), &err_len)) {
            g_test_fail_printf ("server rejected guest login: \"%s\". "
                                "Check the test server's accounts/ for a "
                                "`guest` account with no password.",
                                err);
        } else {
            g_test_fail_printf (
                "server rejected guest login (no error chunk).");
        }
        integration_release_htlc (htlc);
        integration_close (fd);
        return -1;
    }
    if (type != HTLS_HDR_USER_SELFINFO) {
        g_test_fail_printf (
            "timed out waiting for SELFINFO after guest login.");
        integration_release_htlc (htlc);
        integration_close (fd);
        return -1;
    }

    /* Parse SELFINFO into htlc->access / uid / icon so the caller
	 * can read its session state directly. */
    hx_selfinfo_parse (htlc);

    /* hx_selfinfo_parse intentionally does NOT write htlc->name
	 * (Phase 5 policy: server-supplied nick is display-only and
	 * never persisted into the client's name field, to avoid
	 * corrupt-bytes-from-cached-server feedback loops). For test
	 * harness convenience we re-walk the SELFINFO chunks here
	 * and stuff the server's name into htlc->name so the login
	 * test can still assert "name we sent round-trips back
	 * unchanged". This is test-harness-only state poking, not
	 * production behaviour. */
    dh_start (htlc)
    {
        if (_type == HTLS_DATA_USER_LIST
            && _len >= (SIZEOF_HL_USERLIST_HDR - SIZEOF_HL_DATA_HDR)) {
            struct hl_userlist_hdr *uh = (struct hl_userlist_hdr *)dh;
            guint16 nlen;
            HN16 (&nlen, &uh->nlen);
            if (nlen > sizeof (htlc->name) - 1) {
                nlen = sizeof (htlc->name) - 1;
            }
            memcpy (htlc->name, uh->name, nlen);
            htlc->name[nlen] = '\0';
            break;
        }
    }
    dh_end ();

    return fd;
}
