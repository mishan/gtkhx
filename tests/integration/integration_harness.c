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
#include "hxnet_htxf.h"
#include "hxconn.h" /* hx_conn_trans_post_inc for the LOGIN frame's trans */
#include "hl_code.h"
#include "hotline_proto.h"
#include "chat_history.h"
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
/* WEAK stub for the production hlwrite_chunks defined in network.c.
 * chat_history.c references it for hx_get_chat_history's production
 * send path, but most Tier 3 binaries don't link network.c (it would
 * drag in the whole GIOChannel / cipher / compress / signal stack).
 *
 * Marked __attribute__((weak)) (like hx_htlc_close below) so a Tier 3
 * binary that DOES link production network.c — e.g. a test exercising
 * the real htxf_connect — gets network.c's strong definition and this
 * stub yields, instead of a duplicate-symbol link error. Harness-only
 * binaries (no network.c) fall back to this stub.
 *
 * The harness's own send path uses hlpack_chunks + integration_send
 * directly (see integration_send_get_chat_history); production-only
 * code paths that go through hlwrite_chunks shouldn't be reachable
 * here. If a test ever hits this stub, we want a loud failure rather
 * than a silent empty send.
 *
 * Re-declare the extern locally rather than include network.h —
 * that header pulls in pthread + GTK-side state we don't want
 * leaking into the harness link surface. */
extern void hlwrite_chunks (struct htlc_conn *htlc, guint32 type, guint32 flag,
                            const struct hx_chunk *chunks, int hc);
__attribute__((weak)) void
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

/* Phase R1: cipher.c's NULL-state fail-closed paths call
 * hx_htlc_close to tear the connection down rather than encrypt
 * with an uninitialised state. Production wires that to network.c;
 * the integration harness needs its own stub for the Tier 3
 * binaries that link cipher.c without network.c. Test harness
 * behaviour: g_critical (visible failure) and zero htlc->fd so any
 * subsequent integration_send/recv loop terminates. Tests that DO
 * link network.c (real_connect, real_htxf_connect) get the
 * real symbol and skip this stub via the link-order resolution. */
extern void hx_htlc_close (struct htlc_conn *htlc, int expected);
__attribute__((weak)) void
hx_htlc_close (struct htlc_conn *htlc, int expected)
{
    (void) expected;
    g_critical ("hx_htlc_close called from a Tier 3 binary — "
                "cipher.c hit a NULL-state fail-closed path, which "
                "shouldn't happen with valid test fixtures");
    if (htlc) {
        htlc->fd = 0;
    }
}

/* ================================================================== *
 *  Orchestrated transport (Phase G increment 2)                       *
 * ================================================================== *
 *
 * The login entry points (integration_open_login_or_skip,
 * _to_caps_or_skip, _hope_or_skip, _tls_or_skip) drive connect + magic
 * + LOGIN + reply through the SAME production orchestrator the GUI uses
 * (hxnet's run_*_lifecycle, via the polling FFIs
 * hxnet_connection_open_{plaintext,hope,plaintext_tls}_polling). They
 * return a *synthetic* fd in the ORCH_FD_BASE range; integration_send,
 * integration_recv_message, and integration_close detect that range and
 * route through the actor (send_frame / try_recv_frame / destroy). Real
 * socket fds (xfer data channels, tracker connections) are below the
 * base and use the raw-socket path untouched.
 *
 * This was once gated behind GTKHX_HARNESS_ORCHESTRATED (CI ran the
 * suite twice, legacy raw-socket vs orchestrated). The flag is gone:
 * production is orchestrator-only (delete-old-connect removed the legacy
 * connect path), so the orchestrated transport is the only one worth
 * exercising and the entry points use it unconditionally.
 *
 * A few low-level helpers still hand-roll connect + magic + LOGIN over a
 * raw blocking socket — integration_open_or_skip / integration_login_guest
 * (test_handshake, test_login, test_user_account) and the C HOPE
 * step-1/step-2 senders (test_hope_hmac). Those tests call them directly
 * rather than through the orchestrated entry points; retiring that last
 * raw-socket + C-crypto surface is the follow-up that unblocks deleting
 * cipher.c / hope.c (see memory gtkhx_harness_flag_retire).
 */

typedef struct hxnet_connection hxnet_connection;

typedef struct {
    guint32 type_;
    guint32 trans;
    guint32 flag;
    guint16 hc;
    guint16 _pad;
    guint32 body_len;
    guint8 *body_ptr;
} hxnet_frame_t;

#define HXNET_RECV_EMPTY    0
#define HXNET_RECV_FRAME    1
#define HXNET_RECV_SHUTDOWN 2

#define HXNET_SEND_OK 0

extern hxnet_connection *hxnet_connection_open_plaintext_polling (
    const guint8 *host, gsize host_len, guint16 port,
    const guint8 *login, gsize login_len,
    const guint8 *password, gsize password_len,
    const guint8 *name, gsize name_len,
    guint16 icon, guint16 version, guint16 caps, guint32 trans,
    const guint8 *proxy_uri, gsize proxy_uri_len);
/* HOPE-Secure-Login sibling of the polling open. cipher_alg is the wire
 * cipher label ("BLOWFISH" / "CHACHA20-POLY1305"), or NULL/empty for a
 * no-cipher HMAC secure login. The actor runs the full step-1/step-2
 * handshake + cipher transition in Rust and replays the step-2 reply as
 * the first polled frame. */
extern hxnet_connection *hxnet_connection_open_hope_polling (
    const guint8 *host, gsize host_len, guint16 port,
    const guint8 *login, gsize login_len,
    const guint8 *password, gsize password_len,
    const guint8 *name, gsize name_len,
    guint16 icon, guint16 version, guint16 caps, guint32 trans,
    const guint8 *cipher_alg, gsize cipher_alg_len);
/* TLS-from-byte-zero sibling of the polling open (production rustls,
 * Mobius/Janus separate-port model). `verify_cert` decides trust on a
 * WebPKI failure — self-signed test certs need it; the harness passes an
 * accept-any callback (it runs on the tokio task, so it must be
 * thread-safe). */
typedef int (*hxnet_verify_cb_t) (const guint8 *fp, gsize fp_len,
                                  void *user_data);
extern hxnet_connection *hxnet_connection_open_plaintext_tls_polling (
    const guint8 *host, gsize host_len, guint16 port,
    const guint8 *login, gsize login_len,
    const guint8 *password, gsize password_len,
    const guint8 *name, gsize name_len,
    guint16 icon, guint16 version, guint16 caps, guint32 trans,
    hxnet_verify_cb_t verify_cert, void *user_data);
extern int hxnet_connection_try_recv_frame (hxnet_connection *handle,
                                            hxnet_frame_t *out_frame,
                                            int *out_reason);
extern int hxnet_connection_send_frame (hxnet_connection *handle,
                                        const guint8 *data, guint len);
extern void hxnet_connection_destroy (hxnet_connection *handle);
extern void hxnet_frame_free (hxnet_frame_t *frame);
/* HOPE AEAD material handle: the orchestrated login seeds htlc->hope_aead
 * from it; passed to hxnet_htxf_connect so the subchannel derives its
 * per-transfer keys in-process. The HxnetHopeAead type + hxnet_hope_aead_free
 * come from htxf_io.h; this getter is hxnet-internal (not in that header). */
extern HxnetHopeAead *hxnet_connection_hope_aead_material (
    hxnet_connection *handle);

/* Synthetic-fd space for orchestrated control connections. Picked far
 * above any real socket fd so orch_lookup can branch on the value
 * alone. ORCH_MAX bounds concurrent orchestrated connections — two is
 * the most any test needs today (test_two_client_chat), 8 is slack. */
#define ORCH_FD_BASE 0x40000000
#define ORCH_MAX     8

static hxnet_connection *orch_table[ORCH_MAX];

static int
orch_register (hxnet_connection *h)
{
    for (int i = 0; i < ORCH_MAX; i++) {
        if (!orch_table[i]) {
            orch_table[i] = h;
            return ORCH_FD_BASE + i;
        }
    }
    return -1;
}

static hxnet_connection *
orch_lookup (int fd)
{
    if (fd < ORCH_FD_BASE) {
        return NULL;
    }
    int i = fd - ORCH_FD_BASE;
    if (i < 0 || i >= ORCH_MAX) {
        return NULL;
    }
    return orch_table[i];
}

static void
orch_unregister (int fd)
{
    hxnet_connection *h = orch_lookup (fd);
    if (h) {
        hxnet_connection_destroy (h);
        orch_table[fd - ORCH_FD_BASE] = NULL;
    }
}

/* Pack a 22-byte Hotline header byte-for-byte the way
 * src/hxnet_bridge.c::hx_bridge_pack_header does — reconstructing the
 * wire frame the actor already parsed so the downstream chunk walkers
 * (dh_start / hdr_type) see exactly what a legacy raw read would have
 * left in hx_test_in(htlc)->buf. Reimplemented locally rather than linking
 * hxnet_bridge.c, which would drag in the GTK-side session/bridge
 * state the harness deliberately excludes. The wire `len` encodes
 * body_len + sizeof(hc) (Hotline's hc-counts-as-data quirk that
 * hl_hdr_decode reverses); len2 is set equal to len, matching
 * hlpack/hlwrite and hx_bridge_pack_header, so the reconstructed
 * header is byte-identical to real wire bytes. */
static void
orch_pack_header (guint8 *dst, guint32 type, guint32 trans, guint32 flag,
                  guint16 hc, guint32 body_len)
{
    const guint32 wire_len = body_len + (guint32) sizeof (guint16);
    dst[0]  = (guint8) ((type     >> 24) & 0xff);
    dst[1]  = (guint8) ((type     >> 16) & 0xff);
    dst[2]  = (guint8) ((type     >>  8) & 0xff);
    dst[3]  = (guint8) ( type            & 0xff);
    dst[4]  = (guint8) ((trans    >> 24) & 0xff);
    dst[5]  = (guint8) ((trans    >> 16) & 0xff);
    dst[6]  = (guint8) ((trans    >>  8) & 0xff);
    dst[7]  = (guint8) ( trans           & 0xff);
    dst[8]  = (guint8) ((flag     >> 24) & 0xff);
    dst[9]  = (guint8) ((flag     >> 16) & 0xff);
    dst[10] = (guint8) ((flag     >>  8) & 0xff);
    dst[11] = (guint8) ( flag            & 0xff);
    dst[12] = (guint8) ((wire_len >> 24) & 0xff);
    dst[13] = (guint8) ((wire_len >> 16) & 0xff);
    dst[14] = (guint8) ((wire_len >>  8) & 0xff);
    dst[15] = (guint8) ( wire_len        & 0xff);
    dst[16] = (guint8) ((wire_len >> 24) & 0xff);
    dst[17] = (guint8) ((wire_len >> 16) & 0xff);
    dst[18] = (guint8) ((wire_len >>  8) & 0xff);
    dst[19] = (guint8) ( wire_len        & 0xff);
    dst[20] = (guint8) ((hc >> 8) & 0xff);
    dst[21] = (guint8) ( hc       & 0xff);
}

/* Drive connect + magic + LOGIN + reply-replay through the production
 * orchestrator and register the resulting actor as a synthetic fd.
 * Returns the synthetic fd, or -1 (with g_test_fail_printf already
 * called) on open failure. The caller runs the same post-LOGIN drain
 * (integration_drain_until_selfinfo_or_error) it would for a legacy
 * connection — the replayed LOGIN reply is the first frame the actor
 * delivers, exactly as the GUI's rcv path sees it. */
static int
orch_open_login (struct htlc_conn *htlc, const char *host, int port,
                 const char *login, const char *display_name, guint16 icon,
                 guint16 caps)
{
    if (!login) {
        login = "guest";
    }
    const char *name = (display_name && *display_name) ? display_name : "";
    hxnet_connection *h = hxnet_connection_open_plaintext_polling (
        (const guint8 *) host, strlen (host), (guint16) port,
        (const guint8 *) login, strlen (login),
        (const guint8 *) "", 0,             /* guest: empty password */
        (const guint8 *) name, strlen (name),
        icon, /*version=*/185, caps, /*trans=*/1,
        /*proxy_uri=*/NULL, /*proxy_uri_len=*/0);
    if (!h) {
        g_test_fail_printf (
            "hxnet_connection_open_plaintext_polling(%s:%d) returned NULL — "
            "orchestrator could not start the connection.",
            host, port);
        return -1;
    }
    int fd = orch_register (h);
    if (fd < 0) {
        hxnet_connection_destroy (h);
        g_test_fail_printf ("orchestrated transport table full (>%d "
                            "concurrent connections).",
                            ORCH_MAX);
        return -1;
    }

    /* hlpack assigns each outgoing frame's trans from htlc->trans, then
     * increments (proto_helpers.c). In the legacy path the LOGIN send
     * bumps htlc->trans off zero; here the orchestrator owns LOGIN (it
     * used trans=1 internally), so the harness's htlc->trans is still
     * the memset-zero value. Seed it past the LOGIN trans so the first
     * post-login send the test makes gets a unique nonzero trans —
     * test helpers capture htlc->trans as the "expected reply trans"
     * and g_assert it's nonzero. Mirrors production's
     * network.c htlc->trans = reply_trans + 1 convention. */
    if (htlc) {
        htlc->trans = 2;
    }
    return fd;
}

/* HOPE-Secure-Login sibling of orch_open_login: drive the full step-1 /
 * step-2 handshake + cipher transition through the production
 * orchestrator (hxnet's run_hope_lifecycle) and register the resulting
 * actor as a synthetic fd. `cipheralg` is the wire cipher label
 * ("BLOWFISH" / "CHACHA20-POLY1305"), or NULL for a no-cipher HMAC
 * secure login. Returns the synthetic fd (with the post-login drain
 * still to run, exactly like orch_open_login), or -1 on failure. The
 * caller's integration_hope_session stays zeroed: the actor owns all
 * crypto, so the harness's AEAD / stream framing in
 * integration_{send,recv}_message_hope is bypassed for the synthetic
 * fd (those wrappers pass through to integration_{send,recv}_message,
 * which route ORCH fds to the actor). */
static int
orch_open_login_hope (struct htlc_conn *htlc, const char *host, int port,
                      const char *login, const char *password,
                      const char *display_name, guint16 icon, guint16 caps,
                      const char *cipheralg)
{
    if (!login) {
        login = "guest";
    }
    const char *name = (display_name && *display_name) ? display_name : "";
    const char *pass = password ? password : "";
    const char *calg = cipheralg ? cipheralg : "";
    hxnet_connection *h = hxnet_connection_open_hope_polling (
        (const guint8 *) host, strlen (host), (guint16) port,
        (const guint8 *) login, strlen (login),
        (const guint8 *) pass, strlen (pass),
        (const guint8 *) name, strlen (name),
        icon, /*version=*/185, caps, /*trans=*/1,
        (const guint8 *) calg, strlen (calg));
    if (!h) {
        g_test_fail_printf (
            "hxnet_connection_open_hope_polling(%s:%d, cipher=%s) returned "
            "NULL — orchestrator could not complete the HOPE handshake.",
            host, port, calg[0] ? calg : "(none)");
        return -1;
    }
    int fd = orch_register (h);
    if (fd < 0) {
        hxnet_connection_destroy (h);
        g_test_fail_printf ("orchestrated transport table full (>%d "
                            "concurrent connections).",
                            ORCH_MAX);
        return -1;
    }

    /* The orchestrator sent step 1 as trans=1 and step 2 as trans=2;
     * the replayed step-2 reply carries trans=2. Seed htlc->trans past
     * it so the first post-login send the test makes gets a unique
     * nonzero trans (mirrors orch_open_login's +1 seed for plaintext). */
    if (htlc) {
        htlc->trans = 3;
    }
    return fd;
}

/* Accept-any TLS verify callback for the Tier 3 harness. Janus's cert is
 * self-signed (WebPKI fails), so the production rustls path consults this
 * to make the trust decision; the harness unconditionally accepts. Runs
 * on the tokio lifecycle task — stateless, so thread-safe. */
static int
tls_test_accept_cert_cb (const guint8 *fp, gsize fp_len, void *user_data)
{
    (void) fp;
    (void) fp_len;
    (void) user_data;
    return 1;
}

/* TLS-from-byte-zero sibling of orch_open_login: drive a plaintext
 * Hotline login over production rustls (separate-port model) and register
 * the actor as a synthetic fd. Returns the synthetic fd, or -1. */
static int
orch_open_login_tls (struct htlc_conn *htlc, const char *host, int port,
                     const char *login, const char *display_name,
                     guint16 icon, guint16 caps)
{
    if (!login) {
        login = "guest";
    }
    const char *name = (display_name && *display_name) ? display_name : "";
    hxnet_connection *h = hxnet_connection_open_plaintext_tls_polling (
        (const guint8 *) host, strlen (host), (guint16) port,
        (const guint8 *) login, strlen (login),
        (const guint8 *) "", 0,             /* guest: empty password */
        (const guint8 *) name, strlen (name),
        icon, /*version=*/185, caps, /*trans=*/1,
        tls_test_accept_cert_cb, NULL);
    if (!h) {
        g_test_fail_printf (
            "hxnet_connection_open_plaintext_tls_polling(%s:%d) returned NULL "
            "— rustls handshake / connect failed (TLS port mapped?).",
            host, port);
        return -1;
    }
    int fd = orch_register (h);
    if (fd < 0) {
        hxnet_connection_destroy (h);
        g_test_fail_printf ("orchestrated transport table full (>%d "
                            "concurrent connections).",
                            ORCH_MAX);
        return -1;
    }
    /* Plaintext LOGIN over TLS: reply trans is HX_LOGIN_TRANS (1); seed
     * past it (mirrors orch_open_login). */
    if (htlc) {
        htlc->trans = 2;
    }
    return fd;
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
    /* Orchestrated control connection: the actor owns the socket, so
     * a "send" is a full pre-framed Hotline message (header + body, as
     * the hlpack helpers produced it) handed to the actor's write
     * channel. Real fds fall through to the raw write loop below. */
    hxnet_connection *oh = orch_lookup (fd);
    if (oh) {
        if (len > G_MAXUINT) {
            return FALSE;
        }
        return hxnet_connection_send_frame (oh, buf, (guint) len)
               == HXNET_SEND_OK;
    }
    /* A synthetic fd whose slot is empty (already closed / corruption)
     * must NOT fall through to the raw write() path below — that would
     * write() to an enormous fd value. Fail fast instead. */
    if (fd >= ORCH_FD_BASE) {
        return FALSE;
    }

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
    if (orch_lookup (fd)) {
        orch_unregister (fd);
        return;
    }
    if (fd >= 0) {
        close (fd);
    }
}

/* ---- High-level message helpers --------------------------------- */

gboolean
integration_send_message (int fd, struct htlc_conn *htlc, guint32 type,
                          guint32 flag, int hc, ...)
{
    va_list ap;
    va_start (ap, hc);
    gsize len = 0;
    guint8 *buf = hlpack (htlc, type, flag, hc, ap, &len);
    va_end (ap);

    if (!buf) {
        return FALSE;
    }
    gboolean ok = integration_send (fd, buf, len);
    g_free (buf);
    return ok;
}

gboolean
integration_send_chunks (int fd, struct htlc_conn *htlc, guint32 type,
                         guint32 flag, const struct hx_chunk *chunks, int hc)
{
    gsize len = 0;
    guint8 *buf = hlpack_chunks (htlc, type, flag, chunks, hc, &len);

    if (!buf) {
        return FALSE;
    }
    gboolean ok = integration_send (fd, buf, len);
    g_free (buf);
    return ok;
}

gboolean
integration_recv_message (int fd, struct htlc_conn *htlc, int timeout_ms)
{
    /* Reset in buffer. */
    g_free (hx_test_in(htlc)->buf);
    hx_test_in(htlc)->buf = NULL;
    hx_test_in(htlc)->pos = 0;
    hx_test_in(htlc)->len = 0;

    /* Orchestrated control connection: the actor delivers whole,
     * already-parsed frames. Poll the event queue, then rebuild the
     * 22-byte header + body into htlc->in so every downstream chunk
     * walker (dh_start / hdr_type / extractors) works byte-identically
     * to the legacy raw-read path. */
    hxnet_connection *oh = orch_lookup (fd);
    if (oh) {
        gint64 deadline = g_get_monotonic_time ()
                          + (gint64) timeout_ms * 1000;
        for (;;) {
            hxnet_frame_t f;
            int reason = 0;
            int rc = hxnet_connection_try_recv_frame (oh, &f, &reason);
            if (rc == HXNET_RECV_FRAME) {
                /* Build the frame into a fresh local buffer, then hand
                 * it to htlc->in only once it's fully populated. (Don't
                 * assign hx_test_in(htlc)->buf the raw g_malloc up front — the
                 * free-at-top + realloc pattern reads as a potential
                 * use-after-free to static analysis even though the
                 * malloc reassigns it.) */
                gsize total = SIZEOF_HL_HDR + f.body_len;
                guint8 *buf = g_malloc (total);
                orch_pack_header (buf, f.type_, f.trans, f.flag,
                                  f.hc, f.body_len);
                if (f.body_len > 0 && f.body_ptr) {
                    memcpy (buf + SIZEOF_HL_HDR, f.body_ptr, f.body_len);
                }
                hx_test_in(htlc)->buf = buf;
                hx_test_in(htlc)->pos = total;
                hx_test_in(htlc)->len = total;
                hxnet_frame_free (&f);
                return TRUE;
            }
            if (rc == HXNET_RECV_SHUTDOWN) {
                return FALSE;
            }
            if (g_get_monotonic_time () >= deadline) {
                return FALSE;
            }
            g_usleep (5000); /* 5 ms */
        }
    }
    /* A synthetic fd whose slot is empty (already closed / corruption)
     * must NOT fall through to the legacy select()/read path below —
     * that would FD_SET an enormous fd value. Fail fast instead. */
    if (fd >= ORCH_FD_BASE) {
        return FALSE;
    }

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
    hx_test_in(htlc)->buf = g_malloc (total);
    memcpy (hx_test_in(htlc)->buf, hdr_bytes, SIZEOF_HL_HDR);
    if (body_len > 0) {
        if (!integration_recv (fd, hx_test_in(htlc)->buf + SIZEOF_HL_HDR, body_len)) {
            g_free (hx_test_in(htlc)->buf);
            hx_test_in(htlc)->buf = NULL;
            return FALSE;
        }
    }
    hx_test_in(htlc)->pos = total;
    hx_test_in(htlc)->len = total;
    return TRUE;
}

void
integration_release_htlc (struct htlc_conn *htlc)
{
    hx_test_in_free (htlc);
    /* htlc->hope_aead is an owned HxnetHopeAead* (a copy of the control
     * channel's HOPE material, independent of the connection's lifetime —
     * see hxnet_connection_hope_aead_material), seeded by the orchestrated
     * login. Free it here — the single owner — so it doesn't leak across
     * the test run. NULL-safe. A test that needs to free the handle
     * *early* (before release) must NULL the field after doing so, or this
     * would double-free. */
    hxnet_hope_aead_free ((HxnetHopeAead *) htlc->hope_aead);
    htlc->hope_aead = NULL;
}

/* hxnet/src/ffi.rs — build a plaintext LOGIN frame via the production builder
 * (crate::login::build_login_frame). Replaces the old login_packet.c: production
 * login is Rust now (the hxnet orchestrator), so the harness drives the same
 * builder rather than a C duplicate. */
extern size_t hxnet_build_login_frame (const guint8 *login, size_t login_len,
                                       const guint8 *password,
                                       size_t password_len, const guint8 *name,
                                       size_t name_len, guint16 icon,
                                       guint16 version, guint16 caps,
                                       guint32 trans, guint8 *out,
                                       size_t out_cap);

/* Build + synchronously send one plaintext LOGIN packet. Used by both
 * integration_login_guest and integration_login_guest_caps. An empty password
 * omits the PASSWORD chunk (guest login); empty name / zero icon / version / caps
 * each omit their chunk. The trans is post-incremented off the htlc exactly as
 * hlpack_chunks would have done. */
static gboolean
send_login_packet (int fd, struct htlc_conn *htlc, const char *login_name,
                   const char *password, const char *display_name, guint16 icon,
                   guint16 client_version, guint16 caps)
{
    const char *ln = login_name ? login_name : "";
    const char *pw = password ? password : "";
    const char *dn = display_name ? display_name : "";
    guint8 frame[512];
    size_t flen = hxnet_build_login_frame (
        (const guint8 *) ln, strlen (ln), (const guint8 *) pw, strlen (pw),
        (const guint8 *) dn, strlen (dn), icon, client_version, caps,
        hx_conn_trans_post_inc (htlc), frame, sizeof (frame));
    if (flen == 0) {
        return FALSE;
    }
    return integration_send (fd, frame, flen);
}

gboolean
integration_login_guest (int fd, struct htlc_conn *htlc,
                         const char *display_name, guint16 icon)
{
    /* The harness sends HTLC_DATA_NAME inline so test assertions can check
     * "the name we asserted round-trips back unchanged" without driving the full
     * AGREEMENTAGREE flow. Production deliberately does NOT send NAME at LOGIN
     * time; the builder gates the chunk on a non-empty name.
     *
     * clientversion 185 = Hotline 1.8.5: mhxd uses it in rcv_login to set
     * access_extra.can_ping (gated on clientversion >= 150), without which mhxd
     * rejects HTLC_HDR_PING with a task-error. */
    return send_login_packet (fd, htlc, "guest", NULL, display_name, icon,
                              /*client_version=*/185, /*caps=*/0);
}

gboolean
integration_login_guest_caps (int fd, struct htlc_conn *htlc,
                              const char *display_name, guint16 icon,
                              guint16 caps)
{
    /* DATA_CAPABILITIES is "variable-width big-endian" per spec; two bytes cover
     * bits 0..15 which is everything we have today (CHAT_HISTORY is bit 4).
     * Matches the wire layout the production LOGIN path emits. */
    return send_login_packet (fd, htlc, "guest", NULL, display_name, icon,
                              /*client_version=*/185, caps);
}

/* Wall-clock safety bound for the drain helpers below. The per-message
 * recv timeout (3 s) already ends the wait when the stream goes quiet;
 * this only bites under a *continuous* flood of unrelated frames. It's
 * generous so a legitimately slow reply on a busy shared server still
 * lands. */
#define INTEGRATION_DRAIN_DEADLINE_US (30 * G_USEC_PER_SEC)

/* Cross-talk note: the Tier 3 binaries run in parallel against shared
 * servers, so every drain sees other sessions' traffic AND unsolicited
 * server broadcasts (chat, USER_CHANGE, ICON_CHANGE, …). `max_messages`
 * therefore counts only frames of the *category we're waiting on* (e.g.
 * TASK replies, CHAT messages) that simply aren't ours — unrelated
 * frame types are skipped for free so a burst of, say, ICON_CHANGE
 * broadcasts from a concurrent test can't starve the search. The
 * deadline + the recv timeout bound the loop either way. */
gboolean
integration_drain_until_task_trans (int fd, struct htlc_conn *htlc,
                                    guint32 wanted_trans, int max_messages)
{
    gint64 deadline = g_get_monotonic_time () + INTEGRATION_DRAIN_DEADLINE_US;
    int seen = 0;
    while (seen < max_messages && g_get_monotonic_time () < deadline) {
        if (!integration_recv_message (fd, htlc, /*timeout_ms=*/3000)) {
            return FALSE;
        }
        if (hdr_type (htlc) != HTLS_HDR_TASK) {
            continue; /* unrelated broadcast — doesn't count */
        }
        if (hdr_trans (htlc) != wanted_trans) {
            seen++; /* a TASK reply, just not the one we sent */
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
    gint64 deadline = g_get_monotonic_time () + INTEGRATION_DRAIN_DEADLINE_US;
    int seen = 0;
    while (seen < max_messages && g_get_monotonic_time () < deadline) {
        if (!integration_recv_message (fd, htlc, /*timeout_ms=*/3000)) {
            return FALSE;
        }
        if (hdr_type (htlc) != HTLS_HDR_CHAT) {
            continue; /* unrelated broadcast — doesn't count */
        }
        if (!hx_chat_extract (hx_test_in(htlc)->buf, hx_test_in(htlc)->pos, out)) {
            continue;
        }
        if (out->uid == wanted_uid) {
            return TRUE;
        }
        seen++; /* a chat, just not from the uid we want */
    }
    return FALSE;
}

gboolean
integration_drain_until_chat_marker (int fd, struct htlc_conn *htlc,
                                     const char *marker, struct hx_chat_msg *out,
                                     int max_messages)
{
    /* Like integration_drain_until_chat, but matches on a unique
	 * substring in the chat body rather than the sender uid. Required
	 * for chats relayed by Janus: its HTLS_HDR_CHAT broadcasts carry
	 * uid 0 (it doesn't stamp the sender), so a uid filter can't scope
	 * to our own message. A high-entropy marker is the robust
	 * cross-talk discriminator (same approach test_chat_history uses). */
    gint64 deadline = g_get_monotonic_time () + INTEGRATION_DRAIN_DEADLINE_US;
    int seen = 0;
    while (seen < max_messages && g_get_monotonic_time () < deadline) {
        if (!integration_recv_message (fd, htlc, /*timeout_ms=*/3000)) {
            return FALSE;
        }
        if (hdr_type (htlc) != HTLS_HDR_CHAT) {
            continue; /* unrelated broadcast — doesn't count */
        }
        if (!hx_chat_extract (hx_test_in(htlc)->buf, hx_test_in(htlc)->pos, out)) {
            continue;
        }
        if (out->text && marker && strstr (out->text, marker)) {
            return TRUE;
        }
        seen++; /* a chat, just not the one we sent */
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
integration_drain_until_type (int fd, struct htlc_conn *htlc,
                              guint16 wanted_type, int max_messages)
{
    /* No per-instance filter here — any frame of `wanted_type` is the
	 * hit — so there's nothing "of the right category but wrong" to
	 * count. Bound purely by the recv timeout (stream quiet) and the
	 * deadline, so unrelated cross-talk can't make us give up early. */
    (void) max_messages;
    gint64 deadline = g_get_monotonic_time () + INTEGRATION_DRAIN_DEADLINE_US;
    while (g_get_monotonic_time () < deadline) {
        if (!integration_recv_message (fd, htlc, /*timeout_ms=*/3000)) {
            return FALSE;
        }
        if (hdr_type (htlc) == wanted_type) {
            return TRUE;
        }
    }
    return FALSE;
}

gboolean
integration_drain_until_chat_invite (int fd, struct htlc_conn *htlc,
                                     int max_messages)
{
    return integration_drain_until_type (fd, htlc, HTLS_HDR_CHAT_INVITE,
                                         max_messages);
}

gboolean
integration_join_chat (int fd, struct htlc_conn *htlc, guint32 chat_id,
                       int max_messages)
{
    guint32 cid_be = htonl (chat_id);
    guint32 join_trans = htlc->trans;
    if (!integration_send_message (fd, htlc, HTLC_HDR_CHAT_JOIN, /*flag=*/0,
                                   /*hc=*/1, (int)HTLC_DATA_CHAT_ID,
                                   (int)sizeof (cid_be), &cid_be)) {
        return FALSE;
    }
    if (!integration_drain_until_task_trans (fd, htlc, join_trans,
                                             max_messages)) {
        return FALSE;
    }
    /* Reject task-error replies — a JOIN that errored out is never
	 * what the caller wanted (we'd be exercising the wrong path). */
    return (hdr_flag (htlc) & 1) == 0;
}

gboolean
integration_drain_until_chat_user_event (int fd, struct htlc_conn *htlc,
                                         guint16 wanted_type,
                                         guint32 wanted_cid,
                                         guint16 wanted_uid, int max_messages)
{
    gint64 deadline = g_get_monotonic_time () + INTEGRATION_DRAIN_DEADLINE_US;
    int seen = 0;
    while (seen < max_messages && g_get_monotonic_time () < deadline) {
        if (!integration_recv_message (fd, htlc, /*timeout_ms=*/3000)) {
            return FALSE;
        }
        if (hdr_type (htlc) != wanted_type) {
            continue; /* unrelated broadcast — doesn't count */
        }
        seen++; /* a frame of the awaited type; counts whether or not
                 * it turns out to be the cid/uid we want */

        guint32 got_cid = 0;
        guint16 got_uid = 0;
        gboolean got_uid_chunk = FALSE;
        dh_start (hx_test_in(htlc)->buf, hx_test_in(htlc)->pos)
        {
            switch (_type) {
            case HTLS_DATA_CHAT_ID:
                dh_getint (got_cid);
                break;
            case HTLS_DATA_UID:
                if (_len == sizeof (guint16)) {
                    guint16 v;
                    memcpy (&v, dh->data, sizeof v);
                    got_uid = ntohs (v);
                    got_uid_chunk = TRUE;
                }
                break;
            }
        }
        dh_end ();
        if (!got_uid_chunk) {
            continue;
        }
        if (got_cid != wanted_cid || got_uid != wanted_uid) {
            continue;
        }
        return TRUE;
    }
    return FALSE;
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
    dh_start (hx_test_in(htlc)->buf, hx_test_in(htlc)->pos)
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
	 * — the former packs + sends inline, the latter is production's
	 * queue-via-FDW path. */
    guint32 trans = htlc->trans;

    struct hx_chunk chunks[4];
    struct hx_get_chat_history_scratch scratch;
    int hc = hx_get_chat_history_build_chunks (channel_id, before, after,
                                               limit, chunks, 4, &scratch);
    if (hc <= 0) {
        return 0;
    }
    gsize len = 0;
    guint8 *buf = hlpack_chunks (htlc, HTLC_HDR_GET_CHAT_HISTORY, 0, chunks, hc, &len);

    if (!buf) {
        return 0;
    }
    gboolean ok = integration_send (fd, buf, len);
    g_free (buf);
    return ok ? trans : 0;
}

guint32
integration_send_get_chat_history_hope (int fd, struct htlc_conn *htlc,
                                        integration_hope_session *hope,
                                        guint32 channel_id, guint64 before,
                                        guint64 after, guint16 limit)
{
    /* HOPE-aware send. Same chunk-building path as the plain variant
     * (hx_get_chat_history_build_chunks → hlpack_chunks), then a plain
     * send: the orchestrator owns the control-channel crypto, so the
     * harness just writes the framed bytes through the synthetic fd. */
    guint32 trans = htlc->trans;

    /* Pack via hlpack_chunks then plain-send. Snapshot trans before
     * the pack (hlpack_chunks bumps it after writing the header) so the
     * trans-id accounting stays identical to production. */
    struct hx_chunk chunks[4];
    struct hx_get_chat_history_scratch scratch;
    int hc = hx_get_chat_history_build_chunks (channel_id, before, after,
                                               limit, chunks, 4, &scratch);
    if (hc <= 0) {
        return 0;
    }

    /* hlpack_chunks bumps htlc->trans after writing the header, so
     * snapshot before. */
    gsize len = 0;
    guint8 *buf = hlpack_chunks (htlc, HTLC_HDR_GET_CHAT_HISTORY, 0, chunks, hc, &len);

    if (!buf) {
        return 0;
    }
    gboolean ok = integration_send (fd, buf, len);
    g_free (buf);
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

/* fd-free HTXF subchannel open against the default test server's
 * plaintext xfer target (same host / port selection as
 * integration_connect_xfer, honoring GTKHX_TEST_HOST /
 * GTKHX_TEST_XFER_PORT), via hxnet_htxf_connect. `preamble`
 * (length preamble_len) is written raw right after connect; a non-NULL
 * `hope` + `xfer_ref` arm per-transfer AEAD (NULL hope = plaintext). This
 * is the fd-free replacement for the old
 * integration_connect_xfer() + integration_send_xfer_hdr() +
 * hxnet_htxf_open(fd, ...) sequence. Returns NULL if the matrix filter
 * excluded every server, or on a connect / TLS failure. */
HtxfConn *
integration_htxf_open_xfer (const guint8 *preamble, gsize preamble_len,
                            const HxnetHopeAead *hope, guint32 xfer_ref)
{
    const hx_test_server *srv = hx_test_server_default ();
    if (!srv) {
        return NULL;
    }
    return hxnet_htxf_connect ((const guint8 *) srv->host, strlen (srv->host),
                               srv->xfer_port, NULL, 0, /*tls=*/0,
                               preamble, preamble_len, hope, xfer_ref,
                               /*verify_cert=*/NULL, /*user_data=*/NULL);
}

/* Convenience wrapper over integration_htxf_open_xfer for the common
 * single-file case: packs the 16-byte legacy HTXF FILE header
 * (ref, total_size, type=FILE, flag=0) as the preamble, then connects. */
HtxfConn *
integration_htxf_open_xfer_file (guint32 ref, guint32 total_size,
                                 const HxnetHopeAead *hope, guint32 xfer_ref)
{
    guint8 hdr[SIZEOF_HTXF_HDR];
    hl_htxf_hdr_pack (hdr, ref, total_size, HTXF_TYPE_FILE, 0);
    return integration_htxf_open_xfer (hdr, sizeof (hdr), hope, xfer_ref);
}

int
integration_open_or_skip (void)
{
    const hx_test_server *srv = hx_test_server_default ();
    if (!srv) {
        g_test_fail_printf ("GTKHX_TEST_SERVERS env filter excluded every "
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
        g_test_fail_printf (msg);
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
    /* Bounded by the recv timeout (login stream goes quiet) + the
	 * wall-clock deadline, not a frame count: the login interleaving
	 * (TASK reply, agreement, banner, …) plus unrelated broadcasts on a
	 * busy shared server (e.g. ICON_CHANGE from a concurrent test) must
	 * not make us give up before SELFINFO arrives. */
    (void) max_messages;
    gint64 deadline = g_get_monotonic_time () + INTEGRATION_DRAIN_DEADLINE_US;
    while (g_get_monotonic_time () < deadline) {
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
            dh_start (hx_test_in(htlc)->buf, hx_test_in(htlc)->pos)
            {
                if (_type == HTLS_DATA_NAME && _len > 0
                    && htlc->name[0] == 0) {
                    gsize nlen = _len > sizeof (htlc->name) - 1
                                     ? sizeof (htlc->name) - 1
                                     : _len;
                    memcpy (htlc->name, dh->data, nlen);
                    htlc->name[nlen] = '\0';
                } else if (_type == HTLS_DATA_UID
                           && _len == sizeof (guint16)
                           && type == HTLS_HDR_TASK && htlc->uid == 0) {
                    /* 1.9-style servers (Janus) carry our own uid in
					 * the TASK login reply (HTLS_DATA_UID), not in the
					 * SELFINFO that follows — so hx_selfinfo_parse can't
					 * recover it. Stash it here; the open helpers prefer
					 * the SELFINFO uid (mhxd) and fall back to this. */
                    guint16 v;
                    memcpy (&v, dh->data, sizeof v);
                    htlc->uid = ntohs (v);
                } else if (_type == HTLS_DATA_CAPABILITIES && _len > 0) {
                    htlc->caps = hl_capabilities_decode (dh->data, _len);
                } else if (_type == HTLS_DATA_CHAT_MEDIA_MAX_BYTES
                           && _len >= 4) {
                    guint32 v;
                    HN32 (&v, dh->data);
                    htlc->media_max_bytes = v;
                } else if (_type == HTLS_DATA_CHAT_MEDIA_MAX_DIMENSION
                           && _len >= 4) {
                    guint32 v;
                    HN32 (&v, dh->data);
                    htlc->media_max_dimension = v;
                } else if (_type == HTLS_DATA_CHAT_MEDIA_MAX_PIXELS
                           && _len >= 4) {
                    guint32 v;
                    HN32 (&v, dh->data);
                    htlc->media_max_pixels = v;
                } else if (_type == HTLS_DATA_CHAT_MEDIA_CHUNK_SIZE
                           && _len >= 4) {
                    guint32 v;
                    HN32 (&v, dh->data);
                    htlc->media_chunk_size = v;
                } else if (_type == HTLS_DATA_CHAT_MEDIA_MAX_FRAMES
                           && _len >= 4) {
                    guint32 v;
                    HN32 (&v, dh->data);
                    htlc->media_max_frames = v;
                } else if (_type == HTLS_DATA_CHAT_MEDIA_MAX_DURATION_MS
                           && _len >= 4) {
                    guint32 v;
                    HN32 (&v, dh->data);
                    htlc->media_max_duration_ms = v;
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

    /* Production connect + magic + LOGIN through the orchestrator. This
     * non-caps login advertises no capabilities, so caps=0. */
    const hx_test_server *srv = hx_test_server_default ();
    if (!srv) {
        g_test_fail_printf ("no default test server configured.");
        return -1;
    }
    int fd = orch_open_login (htlc, srv->host, srv->port, "guest",
                              display_name, icon, /*caps=*/0);
    if (fd < 0) {
        return -1;
    }

    guint32 type = integration_drain_until_selfinfo_or_error (fd, htlc, 8);

    if (type == HTLS_HDR_TASK) {
        char err[256];
        gsize err_len = 0;
        if (task_error_extract (hx_test_in(htlc)->buf, hx_test_in(htlc)->pos, err, sizeof (err), &err_len)) {
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
    /* On Janus the SELFINFO carries no uid (it arrived in the TASK
	 * login reply and was stashed during the drain above); preserve
	 * that stashed value when hx_selfinfo_parse can't supply one. */
    guint16 stashed_uid = htlc->uid;
    hx_selfinfo_parse (htlc, hx_test_in(htlc)->buf, hx_test_in(htlc)->pos);
    if (htlc->uid == 0) {
        htlc->uid = stashed_uid;
    }

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
        dh_start (hx_test_in(htlc)->buf, hx_test_in(htlc)->pos)
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

    /* Production connect + magic + LOGIN through the orchestrator,
     * advertising the requested capabilities so cap-aware servers
     * (Janus) echo the agreed bits back in the LOGIN reply — the drain
     * below stashes that echo into htlc->caps. */
    int fd = orch_open_login (htlc, srv->host, srv->port, "guest",
                              display_name, icon, caps);
    if (fd < 0) {
        return -1;
    }

    guint32 type = integration_drain_until_selfinfo_or_error (fd, htlc, 12);

    if (type == HTLS_HDR_TASK) {
        char err[256];
        gsize err_len = 0;
        if (task_error_extract (hx_test_in(htlc)->buf, hx_test_in(htlc)->pos, err, sizeof (err), &err_len)) {
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

    /* On Janus the SELFINFO carries no uid (it arrived in the TASK
	 * login reply and was stashed into htlc->uid during the drain);
	 * preserve it across hx_selfinfo_parse, same as the non-caps open
	 * helper. Without this the uid is lost here and any uid-filtered
	 * drain (e.g. inline_media's chat_with_media) never matches. */
    guint16 stashed_uid = htlc->uid;
    hx_selfinfo_parse (htlc, hx_test_in(htlc)->buf, hx_test_in(htlc)->pos);
    if (htlc->uid == 0) {
        htlc->uid = stashed_uid;
    }

    /* Same NAME-recovery cascade as integration_open_login_or_skip:
	 * drain captured a HTLS_DATA_NAME if present; else SELFINFO's
	 * USER_LIST chunk; else fall back to the display_name we sent.
	 * Janus skips both server-side paths so the fallback fires. */
    if (htlc->name[0] == 0) {
        dh_start (hx_test_in(htlc)->buf, hx_test_in(htlc)->pos)
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

/* Open + guest-login over TLS, draining to SELFINFO. Returns a synthetic
 * fd (the orchestrated transport's actor handle) or -1 with
 * g_test_fail_printf already called. Caller closes via integration_close
 * and does I/O with the fd-based integration_send/recv helpers, exactly
 * like the plaintext/HOPE opens. Always production rustls — the GnuTLS
 * GIOStream harness was retired in the harness-TLS migration. */
int
integration_open_login_tls_or_skip (const hx_test_server *srv,
                                    struct htlc_conn *htlc,
                                    const char *display_name, guint16 icon)
{
    memset (htlc, 0, sizeof (*htlc));
    if (!srv || srv->tls_port == 0) {
        g_test_fail_printf (
            "integration_open_login_tls_or_skip: no TLS-capable server "
            "(need HX_TEST_CAP_TLS + tls_port; start the Janus container "
            "with TLS ports mapped).");
        return -1;
    }

    /* Plaintext Hotline over TLS-from-byte-zero on the dedicated port,
     * driven by the production orchestrator. caps=0 — the TLS tests
     * don't exercise capability negotiation. */
    int fd = orch_open_login_tls (htlc, srv->host, srv->tls_port, "guest",
                                  display_name, icon, /*caps=*/0);
    if (fd < 0) {
        return -1;
    }
    /* Mark TLS so an HTXF subchannel the test opens mirrors it. */
    htlc->tls = 1;

    guint32 type = integration_drain_until_selfinfo_or_error (fd, htlc, 8);
    if (type == HTLS_HDR_TASK) {
        char err[256];
        gsize err_len = 0;
        if (task_error_extract (hx_test_in(htlc)->buf, hx_test_in(htlc)->pos, err, sizeof (err), &err_len)) {
            g_test_fail_printf ("server rejected guest login over TLS: \"%s\"",
                                err);
        } else {
            g_test_fail_printf (
                "server rejected guest login over TLS (no error chunk).");
        }
        integration_release_htlc (htlc);
        integration_close (fd);
        return -1;
    }
    if (type != HTLS_HDR_USER_SELFINFO) {
        g_test_fail_printf (
            "timed out waiting for SELFINFO after guest login over TLS.");
        integration_release_htlc (htlc);
        integration_close (fd);
        return -1;
    }
    hx_selfinfo_parse (htlc, hx_test_in(htlc)->buf, hx_test_in(htlc)->pos);
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
    g_free (hope->rx_accum);
    hope->rx_accum = NULL;
    hope->rx_accum_len = 0;
    hope->rx_accum_cap = 0;
    hope->aead_active = 0;
}

int
integration_open_login_hope_or_skip (
    const hx_test_server *srv, struct htlc_conn *htlc,
    integration_hope_session *hope, const char *username,
    const char *password, const char *display_name, guint16 icon,
    const char *cipheralg, const char *compressalg)
{
    g_return_val_if_fail (srv != NULL, -1);
    g_return_val_if_fail (htlc != NULL, -1);
    g_return_val_if_fail (hope != NULL, -1);
    memset (htlc, 0, sizeof (*htlc));
    memset (hope, 0, sizeof (*hope));

    /* Drive the whole HOPE handshake through the production orchestrator
     * (run_hope_lifecycle in Rust): magic + step1 + step2 + cipher
     * transition, with the step-2 reply replayed as the first polled
     * frame for the shared drain below. The hope session stays zeroed —
     * the actor owns crypto, so the integration_*_message_hope wrappers
     * pass the synthetic fd through to the actor (they engage their own
     * AEAD / stream framing only when hope->aead_active / stream_active,
     * which this orchestrated path never sets). */
    int fd = orch_open_login_hope (htlc, srv->host, srv->port, username,
                                   password, display_name, icon,
                                   HTLC_CAP_LARGE_FILES | HTLC_CAP_TEXT_ENCODING
                                       | HTLC_CAP_CHAT_HISTORY,
                                   cipheralg);
    if (fd < 0) {
        return -1;
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
            if (task_error_extract (hx_test_in(htlc)->buf, hx_test_in(htlc)->pos, err, sizeof (err), &err_len)) {
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
        dh_start (hx_test_in(htlc)->buf, hx_test_in(htlc)->pos)
        {
            if (_type == HTLS_DATA_NAME && _len > 0 && htlc->name[0] == 0) {
                gsize nlen = _len > sizeof (htlc->name) - 1
                                 ? sizeof (htlc->name) - 1
                                 : _len;
                memcpy (htlc->name, dh->data, nlen);
                htlc->name[nlen] = '\0';
            } else if (_type == HTLS_DATA_CAPABILITIES && _len > 0) {
                htlc->caps = hl_capabilities_decode (dh->data, _len);
            } else if (_type == HTLS_DATA_CHAT_MEDIA_MAX_BYTES && _len >= 4) {
                guint32 v;
                HN32 (&v, dh->data);
                htlc->media_max_bytes = v;
            } else if (_type == HTLS_DATA_CHAT_MEDIA_MAX_DIMENSION
                       && _len >= 4) {
                guint32 v;
                HN32 (&v, dh->data);
                htlc->media_max_dimension = v;
            } else if (_type == HTLS_DATA_CHAT_MEDIA_MAX_PIXELS && _len >= 4) {
                guint32 v;
                HN32 (&v, dh->data);
                htlc->media_max_pixels = v;
            } else if (_type == HTLS_DATA_CHAT_MEDIA_CHUNK_SIZE && _len >= 4) {
                guint32 v;
                HN32 (&v, dh->data);
                htlc->media_chunk_size = v;
            } else if (_type == HTLS_DATA_CHAT_MEDIA_MAX_FRAMES && _len >= 4) {
                guint32 v;
                HN32 (&v, dh->data);
                htlc->media_max_frames = v;
            } else if (_type == HTLS_DATA_CHAT_MEDIA_MAX_DURATION_MS
                       && _len >= 4) {
                guint32 v;
                HN32 (&v, dh->data);
                htlc->media_max_duration_ms = v;
            }
        }
        dh_end ();

        if (type == HTLS_HDR_USER_SELFINFO) {
            hx_selfinfo_parse (htlc, hx_test_in(htlc)->buf, hx_test_in(htlc)->pos);
            if (htlc->name[0] == 0 && display_name && *display_name) {
                g_strlcpy ((char *) htlc->name, display_name,
                           sizeof (htlc->name));
            }
            /* The HOPE handshake is now complete, so the actor's
             * retained AEAD material is populated. Seed htlc->hope_aead
             * so an HTXF subchannel (banner / file) can derive its
             * per-transfer keys in-process, mirroring production's
             * rcv_task_login. NULL for non-AEAD. */
            hxnet_connection *oh = orch_lookup (fd);
            if (oh) {
                htlc->hope_aead = hxnet_connection_hope_aead_material (oh);
            }
            return fd;
        }
    }

    g_test_fail_printf ("HOPE post-Step-2: timed out waiting for SELFINFO");
    integration_release_htlc (htlc);
    integration_hope_session_release (hope);
    integration_close (fd);
    return -1;
}

gboolean
integration_send_message_hope (int fd, struct htlc_conn *htlc,
                               integration_hope_session *hope, guint32 type,
                               guint32 flag, int hc, ...)
{
    /* Pack via hlpack, same as integration_send_message. */
    va_list ap;
    va_start (ap, hc);
    gsize len = 0;
    guint8 *buf = hlpack (htlc, type, flag, hc, ap, &len);
    va_end (ap);

    if (!buf) {
        return FALSE;
    }
    gboolean ok = integration_send (fd, buf, len);
    g_free (buf);
    return ok;
}

gboolean
integration_send_agreementagree_hope (int                       fd,
                                      struct htlc_conn         *htlc,
                                      integration_hope_session *hope,
                                      const char               *display_name,
                                      guint16                   icon)
{
    /* Drive the same chunk builder production uses
     * (gtkhx_proto_build_agreement_agree_chunks, hotline-proto).
     * Wire shape: icon as u16 BE, display name as raw bytes, options
     * as u16 BE (zero from production; the chunk is mandatory or
     * Mobius panics — see hx_send_agreement_agree's comment). Janus
     * only fires HTLS_HDR_BANNER after seeing this message — without
     * it the post-login push sequence never starts, and any test
     * that drains for the banner times out into a skip.
     *
     * Framing path mirrors integration_send_get_chat_history_hope:
     * hlpack_chunks → plain send (the orchestrator owns the crypto).
     * The harness
     * passes display_name verbatim (typically ASCII for tests);
     * production calls gtkhx_text_for_wire at the caller for UTF-8
     * vs Mac Roman, which is identical to ASCII for the test
     * names. */
    gsize name_len = display_name ? strlen (display_name) : 0;
    struct hx_chunk chunks[HX_AGREEMENT_AGREE_MAX_CHUNKS];
    guint8 scratch[HX_AGREEMENT_AGREE_SCRATCH_SIZE];
    int hc = (int) gtkhx_proto_build_agreement_agree_chunks (
        icon, (const uint8_t *) display_name, name_len, /*options=*/0, chunks,
        HX_AGREEMENT_AGREE_MAX_CHUNKS, scratch, sizeof (scratch));
    if (hc <= 0) {
        return FALSE;
    }

    gsize len = 0;
    guint8 *buf = hlpack_chunks (htlc, HTLC_HDR_AGREEMENTAGREE, 0, chunks, hc, &len);

    if (!buf) {
        return FALSE;
    }
    gboolean ok = integration_send (fd, buf, len);
    g_free (buf);
    return ok;
}

gboolean
integration_recv_message_hope (int fd, struct htlc_conn *htlc,
                              integration_hope_session *hope, int timeout_ms)
{
    return integration_recv_message (fd, htlc, timeout_ms);
}
