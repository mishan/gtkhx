/*
 * test_htxf_cancel.c — the HTXF cooperative-cancel C shim (Phase R3 X1).
 *
 * Exercises the cancellation path xfers.c relies on, through the real
 * htxf_io.c shim + the hxnet abort token, over a TCP loopback pair (so
 * the socket is a genuine connected TCP stream, matching production and
 * the tokio/std TcpStream invariants the hxnet side assumes).
 *
 * The full client transfer worker (xfers.c::xfer_ready_write + the
 * blocking-pool worker + xfer_delete) needs the whole GTK / GtkhxSession
 * app context to drive, which the test harness doesn't stand up. But the
 * load-bearing new logic is the shim layer this test covers directly:
 *
 *   - htxf_io_read short-circuits to ECANCELED when htxf->canceled is set
 *     (the centralized boundary check every worker read goes through);
 *   - htxf_io_abort shuts the subchannel socket down so a read PARKED in
 *     the hxnet transport wakes promptly (the mechanism that makes a
 *     blocking-pool worker cancellable at all);
 *   - an abort-driven wakeup reclassifies as ECANCELED, not a spurious
 *     EIO "channel error";
 *   - the token lifecycle (init → arm → abort → release → free) is
 *     leak-clean (the analyze workflow runs this under ASan).
 *
 * What xfers.c adds on top — the worker-ref handoff and the
 * completion-marshal — is main-loop / refcount glue the Tier 3 transfer
 * matrix already exercises end-to-end on the happy path.
 */

#include "config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <glib.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h> /* struct timeval for SO_RCVTIMEO */
#include <unistd.h>

#include "compat.h"
#include "hotline.h"
#include "protocol.h"
#include "htxf_io.h"

/* Connected TCP loopback pair on 127.0.0.1: sv[0] = server side we
 * control by hand, sv[1] = client side handed to hxnet_htxf_open (which
 * adopts + closes it). Aborts via g_error on any setup failure. Mirrors
 * the helper in test_hxnet_ffi.c. */
static void
tcp_loopback_pair (int sv[2])
{
    int listener = socket (AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        g_error ("socket(listener): %s", g_strerror (errno));
    }
    struct sockaddr_in addr;
    memset (&addr, 0, sizeof (addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind (listener, (struct sockaddr *) &addr, sizeof (addr)) < 0) {
        g_error ("bind: %s", g_strerror (errno));
    }
    if (listen (listener, 1) < 0) {
        g_error ("listen: %s", g_strerror (errno));
    }
    socklen_t alen = sizeof (addr);
    if (getsockname (listener, (struct sockaddr *) &addr, &alen) < 0) {
        g_error ("getsockname: %s", g_strerror (errno));
    }
    int client = socket (AF_INET, SOCK_STREAM, 0);
    if (client < 0) {
        g_error ("socket(client): %s", g_strerror (errno));
    }
    if (connect (client, (struct sockaddr *) &addr, sizeof (addr)) < 0) {
        g_error ("connect: %s", g_strerror (errno));
    }
    int server = accept (listener, NULL, NULL);
    if (server < 0) {
        g_error ("accept: %s", g_strerror (errno));
    }
    close (listener);
    sv[0] = server;
    sv[1] = client;
}

/* Open a plaintext (no-TLS, no-AEAD, empty-preamble) HTXF channel over
 * `client_fd` and stash it on `xfer`, matching how xfers.c drives the
 * shim. The fd is adopted by hxnet — the caller must NOT close it (the
 * channel is torn down via htxf_io_release). */
static void
open_passthrough (struct htxf_conn *xfer, int client_fd)
{
    memset (xfer, 0, sizeof (*xfer));
    htxf_io_init (xfer);
    xfer->hx = hxnet_htxf_open (client_fd, /*tls=*/0, /*host=*/NULL, 0,
                                /*preamble=*/NULL, 0, /*hope_aead=*/NULL,
                                /*xfer_ref=*/0, /*verify_cert=*/NULL,
                                /*user_data=*/NULL);
    g_assert_nonnull (xfer->hx);
}

/* ------------------------------------------------------------------ *
 * Test: the canceled flag short-circuits a read to ECANCELED before it
 * ever touches the transport. This is the centralized boundary check
 * every worker read in xfers.c goes through. */
static void
test_canceled_flag_short_circuits_read (void)
{
    int sv[2];
    tcp_loopback_pair (sv);

    struct htxf_conn xfer;
    open_passthrough (&xfer, sv[1]);

    g_atomic_int_set (&xfer.canceled, TRUE);

    guint8 buf[16];
    errno = 0;
    ssize_t r = htxf_io_read (&xfer, buf, sizeof (buf));
    g_assert_cmpint (r, ==, -1);
    g_assert_cmpint (errno, ==, ECANCELED);

    /* A write is gated the same way. */
    errno = 0;
    ssize_t w = htxf_io_write (&xfer, buf, sizeof (buf));
    g_assert_cmpint (w, ==, -1);
    g_assert_cmpint (errno, ==, ECANCELED);

    htxf_io_release (&xfer);
    htxf_io_abort_free (&xfer);
    close (sv[0]);
}

/* Shared state for the parked-reader thread. */
struct reader_ctx {
    struct htxf_conn *xfer;
    ssize_t result;
    int saved_errno;
    gint64 elapsed_us;
    /* Set (atomically) the instant before the worker calls
	 * htxf_io_read, so the main thread can wait for the reader to be
	 * about to park rather than guessing with a fixed sleep. */
    gint entered;
};

static gpointer
parked_reader (gpointer data)
{
    struct reader_ctx *ctx = data;
    guint8 buf[16];
    gint64 start = g_get_monotonic_time ();
    errno = 0;
    /* Announce we're about to enter the (blocking) transport read. The
	 * main thread waits for this before cancelling, so the cancel
	 * genuinely races a parked recv rather than landing before the read
	 * even started (which would pass via the canceled-flag short-circuit
	 * and prove nothing about waking a blocked read). */
    g_atomic_int_set (&ctx->entered, 1);
    ctx->result = htxf_io_read (ctx->xfer, buf, sizeof (buf));
    ctx->saved_errno = errno;
    ctx->elapsed_us = g_get_monotonic_time () - start;
    return NULL;
}

/* ------------------------------------------------------------------ *
 * Test: a read PARKED in the transport (server sends nothing) is woken
 * promptly when the main thread cancels — set canceled + htxf_io_abort,
 * exactly as xfers.c::xfer_delete does — and the wakeup reclassifies as
 * ECANCELED rather than a generic EIO. A read timeout backstops the test
 * so a regression fails fast instead of hanging the suite. */
static void
test_abort_wakes_parked_read (void)
{
    int sv[2];
    tcp_loopback_pair (sv);

    struct htxf_conn xfer;
    open_passthrough (&xfer, sv[1]);
    htxf_io_abort_init (&xfer); /* main-thread token alloc */
    htxf_io_abort_arm (&xfer);  /* arm with the channel's socket */

    /* Fail-fast backstop: if abort ever stops waking the read, the read
	 * timeout returns on its own instead of wedging the whole test run.
	 * The elapsed assertion below is keyed to this so a timeout-driven
	 * return (≈ READ_TIMEOUT_MS) fails while an abort-driven wake
	 * (tens of ms) passes — robust to CI scheduling variance. */
    const guint READ_TIMEOUT_MS = 3000;
    g_assert_cmpint (htxf_io_set_read_timeout (&xfer, READ_TIMEOUT_MS), ==, 0);

    struct reader_ctx ctx = { .xfer = &xfer, .result = 0, .saved_errno = 0,
                              .elapsed_us = 0, .entered = 0 };
    GThread *t = g_thread_new ("parked-reader", parked_reader, &ctx);

    /* Wait until the reader has reached the read call, then a short
	 * extra beat so it's actually parked in recv() — not a fixed guess.
	 * Bounded so a reader that never starts can't hang the test. */
    for (int i = 0; i < 5000 && !g_atomic_int_get (&ctx.entered); i++) {
        g_usleep (1000);
    }
    g_assert_cmpint (g_atomic_int_get (&ctx.entered), ==, 1);
    g_usleep (50 * 1000); /* let the recv() actually block */

    /* Cancel exactly as xfer_delete does: latch the flag, then abort. */
    g_atomic_int_set (&xfer.canceled, TRUE);
    htxf_io_abort (&xfer);

    g_thread_join (t);

    g_assert_cmpint (ctx.result, ==, -1);
    /* Reclassified as cancel, not a transport fault. */
    g_assert_cmpint (ctx.saved_errno, ==, ECANCELED);
    /* Woke via the abort, not the timeout backstop: well under the
	 * configured read timeout (half of it leaves ample CI margin). */
    g_assert_cmpint (ctx.elapsed_us, <,
                     (gint64) READ_TIMEOUT_MS * 1000 / 2);

    htxf_io_release (&xfer);
    htxf_io_abort_free (&xfer);
    close (sv[0]);
}

/* ------------------------------------------------------------------ *
 * Test: abort fired BEFORE the channel is armed still tears the socket
 * down — the connect-races-cancel window. The token latches the aborted
 * flag with no socket yet; when arm runs late it sees the latch and
 * shuts the socket down (rather than storing a live fd).
 *
 * Crucially this does NOT set htxf->canceled, so it can't pass via the
 * htxf_io_read canceled-flag short-circuit (which never touches the
 * transport). We observe the real effect from the server side: the
 * client socket the late arm shut down delivers EOF. If the
 * abort-before-arm latch broke (arm stored the socket instead of
 * shutting it), the server read would block and the recv timeout would
 * fail the test. */
static void
test_abort_before_arm (void)
{
    int sv[2];
    tcp_loopback_pair (sv);

    struct htxf_conn xfer;
    open_passthrough (&xfer, sv[1]);
    htxf_io_abort_init (&xfer);

    /* Abort BEFORE arm (no socket on the token yet), then arm late. */
    htxf_io_abort (&xfer);
    htxf_io_abort_arm (&xfer);

    /* The late arm must have shut the client socket (shutdown both
	 * directions), so the server side sees EOF. A recv timeout keeps a
	 * broken latch from hanging the test. */
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    g_assert_cmpint (
        setsockopt (sv[0], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof (tv)), ==, 0);
    guint8 b = 0;
    ssize_t n = read (sv[0], &b, 1);
    g_assert_cmpint (n, ==, 0); /* clean EOF — the socket was shut down */

    htxf_io_release (&xfer);
    htxf_io_abort_free (&xfer);
    close (sv[0]);
}

/* ------------------------------------------------------------------ *
 * Test: the abort shims are NULL / not-yet-open safe — the banner.c
 * transient-htxf path and an early cancel both rely on this. No token,
 * no channel, no crash. */
static void
test_abort_shims_null_safe (void)
{
    /* All four on a NULL htxf. */
    htxf_io_abort_init (NULL);
    htxf_io_abort_arm (NULL);
    htxf_io_abort (NULL);
    htxf_io_abort_free (NULL);

    /* A zeroed htxf with no token / no channel (banner's transient
	 * shape): abort + free are no-ops, arm is a no-op (no hx). */
    struct htxf_conn xfer;
    memset (&xfer, 0, sizeof (xfer));
    htxf_io_abort_arm (&xfer);  /* no hx, no token → no-op */
    htxf_io_abort (&xfer);      /* no token → no-op */
    htxf_io_abort_free (&xfer); /* no token → no-op */

    /* init then free with no channel ever opened: leak-clean (ASan). */
    htxf_io_abort_init (&xfer);
    htxf_io_abort_free (&xfer);
}

/* ------------------------------------------------------------------ *
 * Test: many independent channels each created / armed / aborted /
 * freed — exercises the Arc-backed token lifecycle under fan-out (the
 * concurrency shape app shutdown hits via xfers_delete_all), leak-clean
 * under ASan. */
static void
test_many_channels_abort_clean (void)
{
    enum { N = 16 };
    int srv[N];
    struct htxf_conn xfers[N];

    for (int i = 0; i < N; i++) {
        int sv[2];
        tcp_loopback_pair (sv);
        srv[i] = sv[0];
        open_passthrough (&xfers[i], sv[1]);
        htxf_io_abort_init (&xfers[i]);
        htxf_io_abort_arm (&xfers[i]);
    }
    /* Cancel all, as xfers_delete_all does. */
    for (int i = 0; i < N; i++) {
        g_atomic_int_set (&xfers[i].canceled, TRUE);
        htxf_io_abort (&xfers[i]);
    }
    for (int i = 0; i < N; i++) {
        htxf_io_release (&xfers[i]);
        htxf_io_abort_free (&xfers[i]);
        close (srv[i]);
    }
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/htxf/cancel/canceled-flag-short-circuits",
                     test_canceled_flag_short_circuits_read);
    g_test_add_func ("/htxf/cancel/abort-wakes-parked-read",
                     test_abort_wakes_parked_read);
    g_test_add_func ("/htxf/cancel/abort-before-arm", test_abort_before_arm);
    g_test_add_func ("/htxf/cancel/abort-shims-null-safe",
                     test_abort_shims_null_safe);
    g_test_add_func ("/htxf/cancel/many-channels-abort-clean",
                     test_many_channels_abort_clean);

    return g_test_run ();
}
