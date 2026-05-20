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
#include "hl_code.h"
#include "login_packet.h"
#include "chat_history.h"
#include "hope.h"
#ifdef CONFIG_CIPHER
#include "cipher_aead.h"
#endif
#include "integration_harness.h"
#include "server_matrix.h"

/* Connect with a short timeout. The naive blocking connect()
 * doesn't take a timeout argument, so we set the socket to non-
 * blocking, kick off the connect, select() on writability with
 * the timeout, then check SO_ERROR.
 *
 * Phase A multi-server work: this used to be `static` and called
 * only from integration_connect, but server_matrix.c also wants
 * to dial arbitrary host:port pairs (one per matrix entry). Made
 * non-static with the hx_integration_ prefix so server_matrix
 * doesn't have to duplicate the addrinfo dance. The legacy
 * integration_connect() still routes through here. */
/* Stub for the production hlwrite_chunks defined in network.c.
 * chat_history.c references it for hx_get_chat_history's production
 * send path, but Tier 3 binaries don't link network.c (it would
 * drag in the whole GIOChannel / cipher / compress / signal stack).
 *
 * The harness's own send path uses hlpack_chunks + integration_send
 * directly (see integration_send_get_chat_history); production-only
 * code paths that go through hlwrite_chunks shouldn't be reachable
 * here. If a test ever hits this, we want a loud failure rather
 * than a silent empty send.
 *
 * Re-declare the extern locally rather than include network.h —
 * that header pulls in pthread + GTK-side state we don't want
 * leaking into the harness link surface. */
extern void hlwrite_chunks (struct htlc_conn *htlc, guint32 type, guint32 flag,
                            const struct hx_chunk *chunks, int hc);
void
hlwrite_chunks (struct htlc_conn *htlc, guint32 type, guint32 flag,
                const struct hx_chunk *chunks, int hc)
{
    (void) htlc;
    (void) type;
    (void) flag;
    (void) chunks;
    (void) hc;
    g_critical ("hlwrite_chunks called from a Tier 3 binary — production-"
                "only code path leaked into the harness. Use the equivalent "
                "integration_send_* helper instead.");
}

int
hx_integration_connect_to (const char *host, int port, int timeout_ms)
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
    /* Phase A multi-server work: route through the matrix so that
     * GTKHX_TEST_SERVERS env filtering applies to the legacy
     * harness entry points too. hx_test_server_default() honours
     * GTKHX_TEST_HOST / GTKHX_TEST_PORT for backwards compat with
     * pre-matrix CI configs. */
    const hx_test_server *srv = hx_test_server_default ();
    if (!srv) {
        return -1;
    }
    return hx_integration_connect_to (srv->host, srv->port,
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

    /* Production and the harness used to have separate wire-length-
	 * to-body-length math (equivalent formulas, written differently);
	 * hl_hdr_decode centralises it in proto_helpers. The harness
	 * additionally treats oversize input (raw wire_len past
	 * MAX_HOTLINE_PACKET_LEN) as a fatal protocol error and bails —
	 * accidental DoS guard against a misbehaving server. Production
	 * clamps and continues, which is the right behaviour for the
	 * end-user client. */
    guint32 wire_len = 0, body_len = 0;
    if (!hl_hdr_decode (hdr_bytes, NULL, NULL, NULL, NULL, &wire_len,
                        &body_len)) {
        return FALSE;
    }
    if (wire_len > MAX_HOTLINE_PACKET_LEN) {
        return FALSE;
    }

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

/* Pack + synchronously send one LOGIN packet built by the shared
 * login_packet.c module. Used by both integration_login_guest and
 * integration_login_guest_caps so the wire-format details stay in
 * one place — the same place production uses (src/network.c calls
 * hx_login_build_chunks too, via hlwrite_chunks). */
static gboolean
send_login_packet (int fd, struct htlc_conn *htlc, const hx_login_request *req)
{
    /* Reset the out buffer so successive calls each pack into a
	 * fresh buffer (otherwise hlpack_chunks would append to whatever
	 * the previous integration_send_message left behind). */
    g_free (htlc->out.buf);
    htlc->out.buf = NULL;
    htlc->out.pos = 0;
    htlc->out.len = 0;

    struct hx_chunk chunks[HX_LOGIN_MAX_CHUNKS];
    guint8 scratch[HX_LOGIN_SCRATCH_SIZE];
    int hc = hx_login_build_chunks (req, chunks, HX_LOGIN_MAX_CHUNKS,
                                    scratch, sizeof (scratch));
    if (hc <= 0) {
        return FALSE;
    }
    hlpack_chunks (htlc, HTLC_HDR_LOGIN, 0, chunks, hc);

    gboolean ok = integration_send (fd, htlc->out.buf, htlc->out.len);

    g_free (htlc->out.buf);
    htlc->out.buf = NULL;
    htlc->out.pos = 0;
    htlc->out.len = 0;
    return ok;
}

gboolean
integration_login_guest (int fd, struct htlc_conn *htlc,
                         const char *display_name, guint16 icon)
{
    /* The harness sends HTLC_DATA_NAME inline so test assertions can
	 * check "the name we asserted round-trips back unchanged" without
	 * driving the full AGREEMENTAGREE flow. Production deliberately
	 * does NOT send NAME at LOGIN time; the shared builder gates the
	 * chunk on display_name being non-empty so both paths share the
	 * same module. */
    const hx_login_request req = {
        .mode = HX_LOGIN_MODE_LEGACY,
        .icon = icon,
        .login_name = "guest",
        .password = NULL,
        .display_name = display_name,
        /* Advertise ourselves as Hotline 1.8.5. mhxd uses this in
		 * rcv_login to decide whether to set
		 * htlc->access_extra.can_ping (gated on clientversion >= 150
		 * at src/hxd/rcv.c:1600). Without this chunk, mhxd silently
		 * rejects HTLC_HDR_PING with a task-error. */
        .client_version = 185,
        .send_caps = 0,
    };
    return send_login_packet (fd, htlc, &req);
}

gboolean
integration_login_guest_caps (int fd, struct htlc_conn *htlc,
                              const char *display_name, guint16 icon,
                              guint16 caps)
{
    /* DATA_CAPABILITIES is "variable-width big-endian" per spec; two
	 * bytes covers bits 0..15 which is everything we have today
	 * (CHAT_HISTORY is bit 4). Matches the wire layout produced by
	 * src/network.c's production LOGIN path. */
    const hx_login_request req = {
        .mode = HX_LOGIN_MODE_LEGACY,
        .icon = icon,
        .login_name = "guest",
        .password = NULL,
        .display_name = display_name,
        .client_version = 185,
        .caps = caps,
        .send_caps = 1,
    };
    return send_login_packet (fd, htlc, &req);
}

gboolean
integration_drain_until_task_trans (int fd, struct htlc_conn *htlc,
                                    guint32 wanted_trans, int max_messages)
{
    for (int i = 0; i < max_messages; i++) {
        if (!integration_recv_message (fd, htlc, /*timeout_ms=*/3000)) {
            return FALSE;
        }
        if (hdr_type (htlc) != HTLS_HDR_TASK) {
            continue;
        }
        if (hdr_trans (htlc) != wanted_trans) {
            continue;
        }
        return TRUE;
    }
    return FALSE;
}

gboolean
integration_drain_until_chat (int fd, struct htlc_conn *htlc,
                              guint16 wanted_uid, struct hx_chat_msg *out,
                              int max_messages)
{
    for (int i = 0; i < max_messages; i++) {
        if (!integration_recv_message (fd, htlc, /*timeout_ms=*/3000)) {
            return FALSE;
        }
        if (hdr_type (htlc) != HTLS_HDR_CHAT) {
            continue;
        }
        if (!hx_chat_extract (htlc, out)) {
            continue;
        }
        if (out->uid == wanted_uid) {
            return TRUE;
        }
    }
    return FALSE;
}

gsize
integration_encode_hldir_one (guint8 *out, const char *name)
{
    gsize nlen = strlen (name);
    guint16 count_be = htons (1);
    guint16 nlen_be = htons ((guint16) nlen);

    memcpy (out + 0, &count_be, 2); /* component count */
    out[2] = 0;                     /* unknown / reserved */
    memcpy (out + 3, &nlen_be, 2);  /* name length */
    memcpy (out + 5, name, nlen);
    return 5 + nlen;
}

guint32
integration_send_ping (int fd, struct htlc_conn *htlc)
{
    guint32 trans = htlc->trans;
    if (!integration_send_message (fd, htlc, HTLC_HDR_PING, /*flag=*/0,
                                   /*hc=*/0)) {
        return 0;
    }
    return trans;
}

gboolean
integration_create_chat_with_uid (int fd, struct htlc_conn *htlc,
                                  guint16 target_uid, guint32 *chat_id_out,
                                  int max_messages)
{
    if (!chat_id_out) {
        return FALSE;
    }
    *chat_id_out = 0;
    guint16 uid_be = htons (target_uid);
    guint32 create_trans = htlc->trans;
    if (!integration_send_message (fd, htlc, HTLC_HDR_CHAT_CREATE,
                                   /*flag=*/0, /*hc=*/1, (int) HTLC_DATA_UID,
                                   (int) sizeof (uid_be), &uid_be)) {
        return FALSE;
    }
    if (!integration_drain_until_task_trans (fd, htlc, create_trans,
                                             max_messages)) {
        return FALSE;
    }
    /* Server's TASK reply carries HTLS_DATA_CHAT_ID. Walk it out. */
    dh_start (htlc)
    {
        if (_type == HTLS_DATA_CHAT_ID) {
            dh_getint (*chat_id_out);
        }
    }
    dh_end ();
    return *chat_id_out != 0;
}

gboolean
integration_send_chat (int fd, struct htlc_conn *htlc, const char *text)
{
    /* HTLC_DATA_STYLE = 1 is the only value GtkHx + mhxd recognise:
	 * "plain text" (vs. the unused "0 = sub-room" variant in the
	 * original spec). Every chat-sending test in the suite uses
	 * style=1, so we hardcode it here. */
    guint16 style = htons (1);
    return integration_send_message (
        fd, htlc, HTLC_HDR_CHAT, /*flag=*/0, /*hc=*/2, (int) HTLC_DATA_STYLE,
        (int) sizeof (style), &style, (int) HTLC_DATA_CHAT,
        (int) strlen (text), (guint8 *) text);
}

guint32
integration_send_get_chat_history (int fd, struct htlc_conn *htlc,
                                   guint32 channel_id, guint64 before,
                                   guint64 after, guint16 limit)
{
    /* Drive the same chunk builder production uses (src/chat_history.c
	 * via hx_get_chat_history_build_chunks). The harness skips the
	 * cap-gate (so tests can deliberately exercise a server's task-
	 * error response when the extension isn't negotiated) and uses
	 * hlpack_chunks + integration_send instead of hlwrite_chunks
	 * — the former is fire-and-forget against htlc->out, the latter
	 * is production's queue-via-FDW path. */
    guint32 trans = htlc->trans;

    /* Reset htlc->out so the pack starts at offset 0; otherwise
	 * hlpack_chunks would append to whatever an earlier
	 * integration_send_message left behind. */
    g_free (htlc->out.buf);
    htlc->out.buf = NULL;
    htlc->out.pos = 0;
    htlc->out.len = 0;

    struct hx_chunk chunks[4];
    struct hx_get_chat_history_scratch scratch;
    int hc = hx_get_chat_history_build_chunks (channel_id, before, after,
                                               limit, chunks, 4, &scratch);
    if (hc <= 0) {
        return 0;
    }
    hlpack_chunks (htlc, HTLC_HDR_GET_CHAT_HISTORY, 0, chunks, hc);

    gboolean ok = integration_send (fd, htlc->out.buf, htlc->out.len);

    g_free (htlc->out.buf);
    htlc->out.buf = NULL;
    htlc->out.pos = 0;
    htlc->out.len = 0;

    return ok ? trans : 0;
}

/* ---- HTXF subchannel helpers ----------------------------------- */

int
integration_connect_xfer (void)
{
    /* Phase A multi-server: route through the matrix. The default
     * server's xfer_port is HTLS port + 1 in the static table;
     * GTKHX_TEST_XFER_PORT override is handled by
     * hx_test_server_default(). */
    const hx_test_server *srv = hx_test_server_default ();
    if (!srv) {
        return -1;
    }
    return hx_integration_connect_to (srv->host, srv->xfer_port,
                                      /*timeout_ms=*/2000);
}

gboolean
integration_send_xfer_hdr (int fd, guint32 ref, guint32 total_size)
{
    /* Default to type=0 (HTXF_TYPE_FILE), flags=0 — the legacy
	 * single-file 16-byte handshake that mhxd's integration tests
	 * have always exercised. Routes through the same packer
	 * production uses (proto_helpers.c::hl_htxf_hdr_pack), so a
	 * future tweak to the wire layout shows up everywhere at once. */
    guint8 hdr_buf[SIZEOF_HTXF_HDR];
    hl_htxf_hdr_pack (hdr_buf, ref, total_size, HTXF_TYPE_FILE, 0);
    return integration_send (fd, hdr_buf, sizeof (hdr_buf));
}

int
integration_open_or_skip (void)
{
    const hx_test_server *srv = hx_test_server_default ();
    if (!srv) {
        g_test_skip ("GTKHX_TEST_SERVERS env filter excluded every "
                     "entry in the test-server matrix — no target "
                     "to connect to.");
        return -1;
    }

    int fd = integration_connect ();
    if (fd < 0) {
        gchar *msg = g_strdup_printf (
            "integration server %s not reachable at %s:%d "
            "(set GTKHX_TEST_HOST / GTKHX_TEST_PORT to change). "
            "Run `docker run -p 5500:5500 gtkhx-mhxd` from "
            "tests/mhxd/ to bring up a server.",
            srv->name, srv->host, (int) srv->port);
        g_test_skip (msg);
        g_free (msg);
        return -1;
    }

    if (!integration_handshake (fd)) {
        integration_close (fd);
        g_test_fail_printf (
            "connected to %s (%s:%d) but the magic-handshake "
            "exchange failed — is this actually a Hotline server?",
            srv->name, srv->host, (int) srv->port);
        return -1;
    }

    return fd;
}

/* hdr_type / hdr_flag / hdr_trans live as static inlines in
 * integration_harness.h so every Tier 3 test sees them without a
 * link symbol. The harness uses them via the header too. */

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

        /* Opportunistic NAME + CAPABILITIES stash. On 1.9-style
		 * servers (Janus, MacSecret-family) the server echoes the
		 * client's display name back inside the TASK login reply
		 * rather than the SELFINFO that follows. The CAPABILITIES
		 * echo also lives in the TASK reply on every cap-aware
		 * server. integration_recv_message overwrites htlc->in
		 * on every call, so by the time SELFINFO arrives the
		 * earlier TASK is gone — we'd lose both chunks entirely.
		 * Walk every drained message and stash the bits we care
		 * about as we go; mhxd-style servers also send NAME in
		 * SELFINFO so we still pick it up there. The CAPABILITIES
		 * stash mirrors src/rcv.c::rcv_task_login's variable-width
		 * big-endian decode (1..8 bytes) into htlc->caps. */
        {
            dh_start (htlc)
            {
                if (_type == HTLS_DATA_NAME && _len > 0
                    && htlc->name[0] == 0) {
                    gsize nlen = _len > sizeof (htlc->name) - 1
                                     ? sizeof (htlc->name) - 1
                                     : _len;
                    memcpy (htlc->name, dh->data, nlen);
                    htlc->name[nlen] = '\0';
                } else if (_type == HTLS_DATA_CAPABILITIES && _len > 0) {
                    htlc->caps = hl_capabilities_decode (dh->data, _len);
                }
            }
            dh_end ();
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
	 * production behaviour.
	 *
	 * Skipped when integration_drain_until_selfinfo_or_error
	 * already grabbed a NAME chunk from an earlier message
	 * (Janus / 1.9-style flow — name lives in the TASK login
	 * reply, not in SELFINFO). */
    if (htlc->name[0] == 0) {
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
    }

    /* Last-ditch fallback: if neither the drain loop nor SELFINFO
	 * carried a NAME chunk, the server didn't echo our display
	 * name at all (Janus does this — its SELFINFO has access bits
	 * only). Fill htlc->name with the display_name we sent in
	 * the LOGIN, mirroring what gtkhx itself does post-Phase-150
	 * (treat our local copy as authoritative when the server is
	 * silent). Otherwise integration_open_login_or_skip's callers
	 * see "" and asserts on round-tripped name fail spuriously. */
    if (htlc->name[0] == 0 && display_name && *display_name) {
        g_strlcpy ((char *)htlc->name, display_name, sizeof (htlc->name));
    }

    return fd;
}

int
integration_open_login_to_caps_or_skip (const hx_test_server *srv,
                                        struct htlc_conn *htlc,
                                        const char *display_name, guint16 icon,
                                        guint16 caps)
{
    g_return_val_if_fail (srv != NULL, -1);
    memset (htlc, 0, sizeof (*htlc));

    int fd = hx_test_server_connect (srv);
    if (fd < 0) {
        gchar *msg = g_strdup_printf (
            "integration server %s not reachable at %s:%d — start the "
            "container first (see tests/%s/README.md).",
            srv->name, srv->host, (int) srv->port, srv->name);
        g_test_skip (msg);
        g_free (msg);
        return -1;
    }
    if (!integration_handshake (fd)) {
        integration_close (fd);
        g_test_fail_printf (
            "connected to %s (%s:%d) but the magic-handshake "
            "exchange failed — is this actually a Hotline server?",
            srv->name, srv->host, (int) srv->port);
        return -1;
    }

    if (!integration_login_guest_caps (fd, htlc, display_name, icon, caps)) {
        integration_release_htlc (htlc);
        integration_close (fd);
        g_test_fail_printf ("integration_login_guest_caps failed");
        return -1;
    }

    guint32 type = integration_drain_until_selfinfo_or_error (fd, htlc, 12);

    if (type == HTLS_HDR_TASK) {
        char err[256];
        gsize err_len = 0;
        if (task_error_extract (htlc, err, sizeof (err), &err_len)) {
            g_test_fail_printf ("%s rejected guest login: \"%s\"", srv->name,
                                err);
        } else {
            g_test_fail_printf ("%s rejected guest login (no error chunk).",
                                srv->name);
        }
        integration_release_htlc (htlc);
        integration_close (fd);
        return -1;
    }
    if (type != HTLS_HDR_USER_SELFINFO) {
        g_test_fail_printf ("%s: timed out waiting for SELFINFO after "
                            "guest login.",
                            srv->name);
        integration_release_htlc (htlc);
        integration_close (fd);
        return -1;
    }

    hx_selfinfo_parse (htlc);

    /* Same NAME-recovery cascade as integration_open_login_or_skip:
	 * drain captured a HTLS_DATA_NAME if present; else SELFINFO's
	 * USER_LIST chunk; else fall back to the display_name we sent.
	 * Janus skips both server-side paths so the fallback fires. */
    if (htlc->name[0] == 0) {
        dh_start (htlc)
        {
            if (_type == HTLS_DATA_USER_LIST
                && _len >= (SIZEOF_HL_USERLIST_HDR - SIZEOF_HL_DATA_HDR)) {
                struct hl_userlist_hdr *uh = (struct hl_userlist_hdr *) dh;
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
    }
    if (htlc->name[0] == 0 && display_name && *display_name) {
        g_strlcpy ((char *) htlc->name, display_name, sizeof (htlc->name));
    }

    return fd;
}

/* ------------------------------------------------------------------ */
/* HOPE-Secure-Login + ChaCha20-Poly1305                              */
/* ------------------------------------------------------------------ */

void
integration_hope_session_release (integration_hope_session *hope)
{
    if (!hope) {
        return;
    }
#ifdef CONFIG_CIPHER
    g_free (hope->rx_accum);
    hope->rx_accum = NULL;
    hope->rx_accum_len = 0;
    hope->rx_accum_cap = 0;
    hope->aead_active = 0;
#else
    hope->aead_active = 0;
#endif
}

#ifdef CONFIG_CIPHER

/* Build the HOPE Step 2 LOGIN packet and send it over `fd`. Returns
 * TRUE on a full send. Same chunk shape as production's
 * rcv_task_login Step 2 branch in rcv.c. */
static gboolean
send_hope_step2 (int fd, struct htlc_conn *htlc, const char *username,
                 const char *password, const char *display_name,
                 guint16 icon, guint16 caps, gboolean secure_login,
                 const char *server_cipheralg, const char *server_compressalg)
{
    /* HMAC LOGIN field (HMAC of login_name under sessionkey/macalg
     * if secure_login, else hl_code XOR). */
    guint8 login_field[64];
    size_t llen = hope_build_login_field (username, secure_login,
                                          htlc->sessionkey, htlc->sklen,
                                          htlc->macalg, login_field,
                                          sizeof (login_field));
    /* Mirror the production fix in rcv.c::rcv_task_login: a 0-byte
	 * return is only a failure for the HMAC variant. Empty XOR
	 * output is the legitimate "anonymous guest" shape. */
    if (secure_login && !llen) {
        return FALSE;
    }

    /* HMAC chain: password_mac is what we send as the PASSWORD chunk;
     * encode_key/decode_key go into the AEAD derivation. */
    uint8_t password_mac[HOPE_MAC_MAX];
    uint8_t spec_encode_key[HOPE_MAC_MAX];
    uint8_t spec_decode_key[HOPE_MAC_MAX];
    size_t pmaclen
        = hope_compute_chain (password ? password : "", htlc->sessionkey,
                              htlc->sklen, htlc->macalg, password_mac,
                              spec_encode_key, spec_decode_key);
    if (!pmaclen) {
        return FALSE;
    }

    /* Stash the spec-aligned encode/decode keys onto htlc — the
     * caller derives AEAD session keys from them after the Step 2
     * send. Mirrors production's legacy-storage convention (encode
     * spec-key → decode slot, decode spec-key → encode slot). */
    memcpy (htlc->cipher_decode_key, spec_encode_key, pmaclen);
    htlc->cipher_decode_keylen = (uint16_t) pmaclen;
    memcpy (htlc->cipher_encode_key, spec_decode_key, pmaclen);
    htlc->cipher_encode_keylen = (uint16_t) pmaclen;

    /* Reply-list chunks for cipher / compress confirmation. */
    guint8 cipherreply[64];
    size_t cipherreply_n = 0;
    if (server_cipheralg && *server_cipheralg) {
        cipherreply_n = hope_build_alg_reply (server_cipheralg,
                                              cipherreply,
                                              sizeof (cipherreply));
    }
    guint8 compressreply[64];
    size_t compressreply_n = 0;
    if (server_compressalg && *server_compressalg) {
        compressreply_n = hope_build_alg_reply (server_compressalg,
                                                compressreply,
                                                sizeof (compressreply));
    }

    guint16 icon_be = htons (icon);
    guint16 caps_be = htons (caps);

    /* Reset out buffer for the synchronous send below. */
    g_free (htlc->out.buf);
    htlc->out.buf = NULL;
    htlc->out.pos = 0;
    htlc->out.len = 0;

    /* Mirror the chunk order production sends in rcv.c. */
    int hc = 4; /* LOGIN, PASSWORD, NAME, ICON, CAPABILITIES — caps
                 * always sent; counts below adjust. */
    hc++;       /* CAPABILITIES */
    if (cipherreply_n) {
        hc++;
    }
    if (compressreply_n) {
        hc++;
    }

    struct hx_chunk chunks[HX_LOGIN_MAX_CHUNKS];
    int n = 0;
    chunks[n++] = (struct hx_chunk) {
        HTLC_DATA_LOGIN, (guint16) llen, login_field
    };
    chunks[n++] = (struct hx_chunk) {
        HTLC_DATA_PASSWORD, (guint16) pmaclen, password_mac
    };
    if (cipherreply_n) {
        chunks[n++] = (struct hx_chunk) {
            HTLS_DATA_CIPHER_ALG, (guint16) cipherreply_n, cipherreply
        };
    }
    if (compressreply_n) {
        chunks[n++] = (struct hx_chunk) {
            HTLS_DATA_COMPRESS_ALG, (guint16) compressreply_n, compressreply
        };
    }
    chunks[n++] = (struct hx_chunk) {
        HTLC_DATA_NAME,
        (guint16) (display_name ? strlen (display_name) : 0),
        display_name ? (const void *) display_name : NULL
    };
    chunks[n++] = (struct hx_chunk) {
        HTLC_DATA_ICON, 2, &icon_be
    };
    chunks[n++] = (struct hx_chunk) {
        HTLC_DATA_CAPABILITIES, 2, &caps_be
    };
    g_assert (n == hc);

    hlpack_chunks (htlc, HTLC_HDR_LOGIN, 0, chunks, hc);
    gboolean ok = integration_send (fd, htlc->out.buf, htlc->out.len);
    g_free (htlc->out.buf);
    htlc->out.buf = NULL;
    htlc->out.pos = 0;
    htlc->out.len = 0;
    return ok;
}

#endif /* CONFIG_CIPHER */

int
integration_open_login_hope_or_skip (
    const hx_test_server *srv, struct htlc_conn *htlc,
    integration_hope_session *hope, const char *username,
    const char *password, const char *display_name, guint16 icon,
    const char *cipheralg, const char *compressalg)
{
#ifndef CONFIG_CIPHER
    (void) srv;
    (void) htlc;
    (void) hope;
    (void) username;
    (void) password;
    (void) display_name;
    (void) icon;
    (void) cipheralg;
    (void) compressalg;
    g_test_skip ("CONFIG_CIPHER not defined — HOPE-Secure-Login disabled "
                 "at build time.");
    return -1;
#else
    g_return_val_if_fail (srv != NULL, -1);
    g_return_val_if_fail (htlc != NULL, -1);
    g_return_val_if_fail (hope != NULL, -1);
    memset (htlc, 0, sizeof (*htlc));
    memset (hope, 0, sizeof (*hope));

    /* Step 0: TCP + magic handshake. */
    int fd = hx_test_server_connect (srv);
    if (fd < 0) {
        gchar *msg = g_strdup_printf (
            "integration server %s not reachable at %s:%d — start the "
            "container first (see tests/%s/README.md).",
            srv->name, srv->host, (int) srv->port, srv->name);
        g_test_skip (msg);
        g_free (msg);
        return -1;
    }
    if (!integration_handshake (fd)) {
        integration_close (fd);
        g_test_fail_printf ("magic handshake failed against %s", srv->name);
        return -1;
    }

    /* Stamp htlc with the connected endpoint so hope_validate_sessionkey_ip
     * has something to compare against (Janus tolerates the warning;
     * production logs it). */
    g_strlcpy (htlc->ip_addr, srv->host, sizeof (htlc->ip_addr));
    htlc->serverport = (guint16) srv->port;
    g_strlcpy (htlc->login, username ? username : "guest",
               sizeof (htlc->login));
    /* Seed our preferred macalg so hope_parse_step1_reply can detect
     * secure_login when the server echoes it. */
    g_strlcpy (htlc->macalg, "HMAC-SHA256", sizeof (htlc->macalg));
    if (cipheralg) {
        g_strlcpy (htlc->cipheralg, cipheralg, sizeof (htlc->cipheralg));
    }
    if (compressalg) {
        g_strlcpy (htlc->compressalg, compressalg,
                   sizeof (htlc->compressalg));
    }
    htlc->trans = 1;

    /* Step 1: send LOGIN with empty creds + algorithm advertisement. */
    {
        char app_string[64];
        g_snprintf (app_string, sizeof app_string,
                    "gtkhx-tier3-harness/%s", VERSION);
        const hx_login_request req = {
            .mode = HX_LOGIN_MODE_HOPE_STEP1,
            .hope_app_id = "GTKx",
            .hope_app_string = app_string,
            .cipheralg = cipheralg,
            .compressalg = compressalg,
        };
        struct hx_chunk step1_chunks[HX_LOGIN_MAX_CHUNKS];
        guint8 step1_scratch[HX_LOGIN_SCRATCH_SIZE];
        int hc = hx_login_build_chunks (&req, step1_chunks,
                                        HX_LOGIN_MAX_CHUNKS,
                                        step1_scratch,
                                        sizeof (step1_scratch));
        if (hc <= 0) {
            integration_close (fd);
            g_test_fail_printf ("HOPE Step 1 chunk build failed");
            return -1;
        }
        g_free (htlc->out.buf);
        htlc->out.buf = NULL;
        htlc->out.pos = 0;
        htlc->out.len = 0;
        hlpack_chunks (htlc, HTLC_HDR_LOGIN, 0, step1_chunks, hc);
        if (!integration_send (fd, htlc->out.buf, htlc->out.len)) {
            g_free (htlc->out.buf);
            htlc->out.buf = NULL;
            integration_close (fd);
            g_test_fail_printf ("HOPE Step 1 send failed");
            return -1;
        }
        g_free (htlc->out.buf);
        htlc->out.buf = NULL;
        htlc->out.pos = 0;
        htlc->out.len = 0;
    }

    /* Step 1 reply: a TASK with sessionkey + algorithm chunks. */
    if (!integration_recv_message (fd, htlc, /*timeout_ms=*/5000)) {
        integration_release_htlc (htlc);
        integration_close (fd);
        g_test_fail_printf ("HOPE Step 1 reply timeout");
        return -1;
    }
    if (hdr_type (htlc) != HTLS_HDR_TASK) {
        integration_release_htlc (htlc);
        integration_close (fd);
        g_test_fail_printf ("HOPE Step 1 reply wasn't a TASK (got 0x%x)",
                            hdr_type (htlc));
        return -1;
    }
    if (hdr_flag (htlc) & 1) {
        char err[256];
        gsize err_len = 0;
        if (task_error_extract (htlc, err, sizeof (err), &err_len)) {
            g_test_fail_printf ("HOPE Step 1 task-error: \"%s\"", err);
        } else {
            g_test_fail_printf ("HOPE Step 1 task-error (no chunk)");
        }
        integration_release_htlc (htlc);
        integration_close (fd);
        return -1;
    }

    struct hope_step1_reply sel;
    enum hope_step1_err herr = hope_parse_step1_reply (htlc, htlc->macalg,
                                                       &sel);
    if (herr != HOPE_OK) {
        integration_release_htlc (htlc);
        integration_close (fd);
        g_test_fail_printf ("hope_parse_step1_reply: err=%d", (int) herr);
        return -1;
    }
    /* Server's macalg pick overwrites our preference. */
    g_strlcpy (htlc->macalg, sel.macalg, sizeof (htlc->macalg));

    /* Step 2: authenticated LOGIN. */
    if (!send_hope_step2 (fd, htlc, username ? username : "guest", password,
                          display_name, icon,
                          HTLC_CAP_LARGE_FILES | HTLC_CAP_TEXT_ENCODING
                              | HTLC_CAP_CHAT_HISTORY,
                          sel.secure_login, sel.s_cipheralg,
                          sel.s_compressalg)) {
        integration_release_htlc (htlc);
        integration_close (fd);
        g_test_fail_printf ("HOPE Step 2 send failed");
        return -1;
    }

    /* If we negotiated ChaCha20-Poly1305, derive AEAD session keys
     * now — every byte after this point on the wire is framed AEAD. */
    if (hope_cipher_is_aead (sel.s_cipheralg)
        && hope_cipher_is_aead (sel.c_cipheralg)) {
        cipher_aead_derive_session_keys (
            &hope->encode_state, &hope->decode_state, htlc->sessionkey,
            htlc->sklen, htlc->cipher_decode_key, htlc->cipher_decode_keylen,
            htlc->cipher_encode_key, htlc->cipher_encode_keylen);
        hope->aead_active = 1;
    }

    /* Drain post-login messages until SELFINFO arrives (matches the
     * legacy-LOGIN drain behaviour). With AEAD active the
     * AEAD-aware recv unwraps each frame transparently. */
    int max_messages = 12;
    for (int i = 0; i < max_messages; i++) {
        if (!integration_recv_message_hope (fd, htlc, hope,
                                            /*timeout_ms=*/5000)) {
            integration_release_htlc (htlc);
            integration_hope_session_release (hope);
            integration_close (fd);
            g_test_fail_printf ("HOPE post-Step-2 recv failed");
            return -1;
        }
        guint32 type = hdr_type (htlc);
        guint32 flag = hdr_flag (htlc);
        if (type == HTLS_HDR_TASK && (flag & 1)) {
            char err[256];
            gsize err_len = 0;
            if (task_error_extract (htlc, err, sizeof (err), &err_len)) {
                g_test_fail_printf ("HOPE Step 2 rejected: \"%s\"", err);
            } else {
                g_test_fail_printf ("HOPE Step 2 rejected (no error chunk)");
            }
            integration_release_htlc (htlc);
            integration_hope_session_release (hope);
            integration_close (fd);
            return -1;
        }
        /* Opportunistic NAME / CAPABILITIES stash, same as the legacy
         * drain (and same caveat: htlc->in gets overwritten between
         * recv calls, so we capture what we want as we walk). */
        dh_start (htlc)
        {
            if (_type == HTLS_DATA_NAME && _len > 0 && htlc->name[0] == 0) {
                gsize nlen = _len > sizeof (htlc->name) - 1
                                 ? sizeof (htlc->name) - 1
                                 : _len;
                memcpy (htlc->name, dh->data, nlen);
                htlc->name[nlen] = '\0';
            } else if (_type == HTLS_DATA_CAPABILITIES && _len > 0) {
                htlc->caps = hl_capabilities_decode (dh->data, _len);
            }
        }
        dh_end ();

        if (type == HTLS_HDR_USER_SELFINFO) {
            hx_selfinfo_parse (htlc);
            if (htlc->name[0] == 0 && display_name && *display_name) {
                g_strlcpy ((char *) htlc->name, display_name,
                           sizeof (htlc->name));
            }
            return fd;
        }
    }

    g_test_fail_printf ("HOPE post-Step-2: timed out waiting for SELFINFO");
    integration_release_htlc (htlc);
    integration_hope_session_release (hope);
    integration_close (fd);
    return -1;
#endif /* CONFIG_CIPHER */
}

gboolean
integration_send_message_hope (int fd, struct htlc_conn *htlc,
                               integration_hope_session *hope, guint32 type,
                               guint32 flag, int hc, ...)
{
    /* Pack into htlc->out via hlpack first, same as integration_send_message. */
    g_free (htlc->out.buf);
    htlc->out.buf = NULL;
    htlc->out.pos = 0;
    htlc->out.len = 0;

    va_list ap;
    va_start (ap, hc);
    hlpack (htlc, type, flag, hc, ap);
    va_end (ap);

#ifdef CONFIG_CIPHER
    if (hope && hope->aead_active) {
        /* Frame the just-packed message: 4-byte BE length prefix +
         * ciphertext + 16-byte Poly1305 tag. */
        gsize pt_len = htlc->out.len;
        gsize out_cap = CIPHER_AEAD_LENGTH_PREFIX + pt_len
                        + CIPHER_AEAD_TAG_SIZE;
        guint8 *framed = g_malloc (out_cap);
        size_t n = cipher_aead_seal (&hope->encode_state, htlc->out.buf,
                                     pt_len, framed, out_cap);
        gboolean ok = FALSE;
        if (n) {
            ok = integration_send (fd, framed, n);
        }
        g_free (framed);
        g_free (htlc->out.buf);
        htlc->out.buf = NULL;
        htlc->out.pos = 0;
        htlc->out.len = 0;
        return ok;
    }
#else
    (void) hope;
#endif

    gboolean ok = integration_send (fd, htlc->out.buf, htlc->out.len);
    g_free (htlc->out.buf);
    htlc->out.buf = NULL;
    htlc->out.pos = 0;
    htlc->out.len = 0;
    return ok;
}

#ifdef CONFIG_CIPHER

/* Append bytes to hope->rx_accum, growing as needed. */
static void
rx_accum_append (integration_hope_session *hope, const guint8 *bytes,
                 gsize n)
{
    if (hope->rx_accum_len + n > hope->rx_accum_cap) {
        gsize new_cap = hope->rx_accum_cap ? hope->rx_accum_cap * 2 : 4096;
        while (new_cap < hope->rx_accum_len + n) {
            new_cap *= 2;
        }
        hope->rx_accum = g_realloc (hope->rx_accum, new_cap);
        hope->rx_accum_cap = new_cap;
    }
    memcpy (hope->rx_accum + hope->rx_accum_len, bytes, n);
    hope->rx_accum_len += n;
}

/* Try to consume one full AEAD frame from hope->rx_accum. Returns
 * the plaintext length on success (with htlc->in populated) and
 * shifts any leftover bytes to the start of the accumulator. Returns
 * -1 on AEAD-open failure (test should fail loudly). Returns 0 if
 * not enough bytes have arrived yet. */
static gssize
try_consume_aead_frame (struct htlc_conn *htlc,
                       integration_hope_session *hope)
{
    if (!hope->aead_active) {
        return 0;
    }
    size_t frame_size = cipher_aead_peek_frame_size (hope->rx_accum,
                                                     hope->rx_accum_len);
    if (!frame_size || hope->rx_accum_len < frame_size) {
        return 0;
    }

    /* Plaintext = frame - prefix - tag. */
    gsize pt_cap = frame_size - CIPHER_AEAD_LENGTH_PREFIX
                   - CIPHER_AEAD_TAG_SIZE;
    g_free (htlc->in.buf);
    htlc->in.buf = g_malloc (pt_cap > 0 ? pt_cap : 1);
    size_t pt_len = cipher_aead_open (&hope->decode_state, hope->rx_accum,
                                      frame_size, htlc->in.buf, pt_cap);
    if (!pt_len) {
        g_free (htlc->in.buf);
        htlc->in.buf = NULL;
        return -1;
    }
    htlc->in.pos = pt_len;
    htlc->in.len = pt_len;

    /* Shift leftover bytes down. */
    gsize leftover = hope->rx_accum_len - frame_size;
    if (leftover) {
        memmove (hope->rx_accum, hope->rx_accum + frame_size, leftover);
    }
    hope->rx_accum_len = leftover;
    return (gssize) pt_len;
}

#endif /* CONFIG_CIPHER */

gboolean
integration_recv_message_hope (int fd, struct htlc_conn *htlc,
                              integration_hope_session *hope, int timeout_ms)
{
#ifdef CONFIG_CIPHER
    if (hope && hope->aead_active) {
        /* First check whether we already have a buffered frame. */
        gssize n = try_consume_aead_frame (htlc, hope);
        if (n > 0) {
            return TRUE;
        }
        if (n < 0) {
            return FALSE;
        }

        /* Read more bytes from the socket until a full frame
         * accumulates. Cap iterations so a misbehaving server can't
         * stall us indefinitely. */
        for (int iter = 0; iter < 32; iter++) {
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
            guint8 chunk[4096];
            ssize_t got = read (fd, chunk, sizeof (chunk));
            if (got <= 0) {
                return FALSE;
            }
            rx_accum_append (hope, chunk, (gsize) got);
            gssize m = try_consume_aead_frame (htlc, hope);
            if (m > 0) {
                return TRUE;
            }
            if (m < 0) {
                return FALSE;
            }
        }
        return FALSE;
    }
#else
    (void) hope;
#endif
    return integration_recv_message (fd, htlc, timeout_ms);
}
