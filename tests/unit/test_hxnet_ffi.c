/*
 * test_hxnet_ffi.c — smoke test for the hxnet C-callable FFI.
 *
 * Phase R3.3.b: hxnet ships a polling FFI surface (spawn_fd,
 * try_recv_frame, send_frame, destroy, frame_free). This test
 * uses a TCP loopback pair (bind/listen/accept + connect) so the
 * fd we hand to hxnet_connection_spawn_fd is a real connected
 * TCP socket — matching what production will pass and the
 * AF_INET assumptions the FFI's peer_addr probe + the underlying
 * tokio::net::TcpStream depend on. The earlier draft used an
 * AF_UNIX socketpair(2), which is a stream socket but NOT a TCP
 * one, so it violated TcpStream's invariants.
 *
 * Covers:
 *   - spawn_fd adopts the fd and starts the actor
 *   - try_recv_frame returns HXNET_RECV_FRAME with correct fields
 *   - send_frame round-trips bytes to the peer
 *   - try_recv_frame on EOF returns HXNET_RECV_SHUTDOWN with
 *     HXNET_SHUTDOWN_EOF
 *   - destroy + frame_free clean up without leaks (no asan
 *     screams)
 *
 * The C side mirrors the FFI's constants by hand — the
 * established discipline for hxbridge / hotline-proto FFI
 * (drift surfaces as a link-time undefined symbol or a test
 * failure).
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Opaque handle type. We only ever hold a pointer. */
typedef struct hxnet_connection hxnet_connection;

/* Layout-locked frame struct exposed by ffi.rs as #[repr(C)].
 * Mirror of HxnetFrame; the Rust side pins offsets/size/align via
 * const-asserts in rust/crates/hxnet/src/ffi.rs and we pin the
 * matching offsets here so drift on either side breaks both
 * compiles before any byte hits the wire. body_ptr's exact
 * offset and the struct's total size are pointer-alignment-
 * dependent — expressed in terms of sizeof(void *). */
typedef struct {
    uint32_t type_;
    uint32_t trans;
    uint32_t flag;
    uint16_t hc;
    uint16_t _pad;
    uint32_t body_len;
    uint8_t *body_ptr;
} hxnet_frame;

_Static_assert (offsetof (hxnet_frame, type_) == 0,
                "hxnet_frame.type_ offset");
_Static_assert (offsetof (hxnet_frame, trans) == 4,
                "hxnet_frame.trans offset");
_Static_assert (offsetof (hxnet_frame, flag) == 8,
                "hxnet_frame.flag offset");
_Static_assert (offsetof (hxnet_frame, hc) == 12, "hxnet_frame.hc offset");
_Static_assert (offsetof (hxnet_frame, _pad) == 14, "hxnet_frame._pad offset");
_Static_assert (offsetof (hxnet_frame, body_len) == 16,
                "hxnet_frame.body_len offset");
_Static_assert (offsetof (hxnet_frame, body_ptr)
                    == (sizeof (void *) == 8 ? 24 : 20),
                "hxnet_frame.body_ptr offset");
_Static_assert (_Alignof (hxnet_frame) == _Alignof (void *),
                "hxnet_frame alignment matches pointer alignment");
/* Total size: body_ptr offset + sizeof(void *). Pinning the size
 * catches trailing-padding drift (a regression that adds a field
 * after body_ptr or changes the struct layout in a way that
 * affects only the tail). Mirrors the size_of assertion in the
 * Rust const-block in rust/crates/hxnet/src/ffi.rs. */
_Static_assert (sizeof (hxnet_frame)
                    == ((sizeof (void *) == 8 ? 24 : 20) + sizeof (void *)),
                "hxnet_frame total size");

/* Return codes mirrored from rust/crates/hxnet/src/ffi.rs. */
#define HXNET_RECV_EMPTY    0
#define HXNET_RECV_FRAME    1
#define HXNET_RECV_SHUTDOWN 2

#define HXNET_SHUTDOWN_EOF            0
#define HXNET_SHUTDOWN_STREAM_ERROR   1
#define HXNET_SHUTDOWN_FRAME_TOO_LARGE 2
#define HXNET_SHUTDOWN_HANDLE_DROPPED  3

#define HXNET_SEND_OK      0
#define HXNET_SEND_FULL   -1
#define HXNET_SEND_CLOSED -2
#define HXNET_SEND_INVALID -3

extern hxnet_connection *hxnet_connection_spawn_fd (int fd);
extern int hxnet_connection_try_recv_frame (hxnet_connection *handle,
                                            hxnet_frame *out_frame,
                                            int *out_reason);
extern int hxnet_connection_send_frame (hxnet_connection *handle,
                                        const uint8_t *data, unsigned int len);
extern void hxnet_connection_destroy (hxnet_connection *handle);
extern void hxnet_frame_free (hxnet_frame *f);

/* Write `len` bytes of `buf` to `fd`, retrying on short writes
 * and on EINTR. A real I/O error (broken pipe, etc.) or a 0-byte
 * return aborts the test with a clear message — those signal a
 * fundamental teardown the test can't recover from. */
static void
write_all_or_die (int fd, const uint8_t *buf, size_t len)
{
    while (len) {
        ssize_t w = write (fd, buf, len);
        if (w < 0) {
            if (errno == EINTR) {
                continue; /* signal — retry the syscall */
            }
            g_error ("write(fd=%d, len=%zu) failed: %s", fd, len,
                     g_strerror (errno));
        }
        if (w == 0) {
            g_error ("write(fd=%d) returned 0 — peer closed mid-write", fd);
        }
        buf += (size_t) w;
        len -= (size_t) w;
    }
}

/* Read `len` bytes from `fd`, retrying on short reads and on
 * EINTR. Unexpected EOF (a zero return) or a real I/O error
 * aborts the test with a clear message. */
static void
read_all_or_die (int fd, uint8_t *buf, size_t len)
{
    while (len) {
        ssize_t r = read (fd, buf, len);
        if (r < 0) {
            if (errno == EINTR) {
                continue; /* signal — retry the syscall */
            }
            g_error ("read(fd=%d, len=%zu) failed: %s", fd, len,
                     g_strerror (errno));
        }
        if (r == 0) {
            g_error ("read(fd=%d) hit unexpected EOF with %zu bytes still expected",
                     fd, len);
        }
        buf += (size_t) r;
        len -= (size_t) r;
    }
}

/* Construct a 22-byte Hotline header for a frame with `body_len`
 * body bytes following. Wire `len` counts body + sizeof(hc=2). */
static void
build_header (uint8_t hdr[22], uint32_t type_, uint32_t trans, uint32_t flag,
              uint32_t body_len, uint16_t hc)
{
    uint32_t wire_len = body_len + 2;
    uint32_t type_be = g_htonl (type_);
    uint32_t trans_be = g_htonl (trans);
    uint32_t flag_be = g_htonl (flag);
    uint32_t len_be = g_htonl (wire_len);
    uint16_t hc_be = g_htons (hc);
    memcpy (&hdr[0], &type_be, 4);
    memcpy (&hdr[4], &trans_be, 4);
    memcpy (&hdr[8], &flag_be, 4);
    memcpy (&hdr[12], &len_be, 4);
    memcpy (&hdr[16], &len_be, 4); /* len2 mirrors len */
    memcpy (&hdr[20], &hc_be, 2);
}

/* Build a connected TCP loopback pair: bind a listening socket
 * on 127.0.0.1:0 (kernel-chosen port), connect a second socket
 * to it, accept on the listener. Returns the two ends as
 * sv[0] (server, accepted side) and sv[1] (client, the side we
 * hand to hxnet). The listener is closed before return — only
 * the connected pair survives. Aborts the test via g_error on
 * any setup failure. */
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
    addr.sin_port = 0; /* kernel picks */
    if (bind (listener, (struct sockaddr *) &addr, sizeof (addr)) < 0) {
        g_error ("bind: %s", g_strerror (errno));
    }
    if (listen (listener, 1) < 0) {
        g_error ("listen: %s", g_strerror (errno));
    }

    /* Read back the kernel-assigned port. */
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

/* Poll try_recv_frame until it returns FRAME or SHUTDOWN, capped
 * at ~5 seconds total wall clock (5000 attempts × 1 ms). Returns
 * the code; if FRAME, fills `out_frame`; if SHUTDOWN, fills
 * `out_reason`. */
static int
poll_for_event (hxnet_connection *handle, hxnet_frame *out_frame,
                int *out_reason)
{
    for (int i = 0; i < 5000; i++) {
        int r = hxnet_connection_try_recv_frame (handle, out_frame, out_reason);
        if (r != HXNET_RECV_EMPTY) {
            return r;
        }
        g_usleep (1000);
    }
    return HXNET_RECV_EMPTY;
}

/* ------------------------------------------------------------------- *
 * Test: round-trip a single frame through hxnet.
 *
 * Server side writes header + body; client side (hxnet) reads
 * and emits an Event::Frame. */
static void
test_round_trip_single_frame (void)
{
    int sv[2];
    tcp_loopback_pair (sv);

    /* sv[0] is the "server" side we control by hand. sv[1] goes
     * to hxnet. After the handoff hxnet owns sv[1] — we must not
     * close it. */
    hxnet_connection *conn = hxnet_connection_spawn_fd (sv[1]);
    g_assert_nonnull (conn);

    uint8_t hdr[22];
    build_header (hdr, 0x69, 1, 0, 8, 0);
    write_all_or_die (sv[0], hdr, sizeof (hdr));
    write_all_or_die (sv[0], (const uint8_t *) "hello!\0\0", 8);

    hxnet_frame frame;
    memset (&frame, 0, sizeof (frame));
    int reason = -1;
    int r = poll_for_event (conn, &frame, &reason);
    g_assert_cmpint (r, ==, HXNET_RECV_FRAME);

    g_assert_cmpuint (frame.type_, ==, 0x69);
    g_assert_cmpuint (frame.trans, ==, 1);
    g_assert_cmpuint (frame.flag, ==, 0);
    g_assert_cmpuint (frame.body_len, ==, 8);
    g_assert_nonnull (frame.body_ptr);
    g_assert_cmpmem (frame.body_ptr, frame.body_len, "hello!\0\0", 8);

    hxnet_frame_free (&frame);

    /* Close the server side and confirm we see the Shutdown(Eof) */
    g_assert_cmpint (close (sv[0]), ==, 0);

    memset (&frame, 0, sizeof (frame));
    reason = -1;
    r = poll_for_event (conn, &frame, &reason);
    g_assert_cmpint (r, ==, HXNET_RECV_SHUTDOWN);
    g_assert_cmpint (reason, ==, HXNET_SHUTDOWN_EOF);

    hxnet_connection_destroy (conn);
}

/* ------------------------------------------------------------------- *
 * Test: send_frame writes bytes to the peer. */
static void
test_send_frame_round_trip (void)
{
    int sv[2];
    tcp_loopback_pair (sv);

    hxnet_connection *conn = hxnet_connection_spawn_fd (sv[1]);
    g_assert_nonnull (conn);

    /* Build a full frame as bytes and ship via send_frame. */
    uint8_t buf[22 + 5];
    build_header (buf, 0x6b, 99, 0, 5, 0);
    memcpy (&buf[22], "login", 5);

    int r = hxnet_connection_send_frame (conn, buf, sizeof (buf));
    g_assert_cmpint (r, ==, HXNET_SEND_OK);

    /* Server side reads the bytes back. */
    uint8_t got[27] = { 0 };
    read_all_or_die (sv[0], got, sizeof (got));
    g_assert_cmpmem (got, sizeof (got), buf, sizeof (buf));

    g_assert_cmpint (close (sv[0]), ==, 0);
    hxnet_connection_destroy (conn);
}

/* ------------------------------------------------------------------- *
 * Test: NULL handle / NULL data fall through cleanly.
 *
 * The FFI's policy on invalid args is "g_critical + return a
 * failure code, never crash." GTest's default fatal-mask
 * promotes criticals to abort; we use g_test_expect_message to
 * acknowledge each expected critical so the test doesn't bail.
 * (Same pattern hxbridge::tests::malformed_utf8_signal_name uses
 * for its critical-emitting path.) */
static void
test_invalid_args_return_invalid_not_crash (void)
{
    int reason = 0;
    hxnet_frame f;
    memset (&f, 0, sizeof (f));

    /* NULL handle to try_recv_frame returns EMPTY (we picked
     * EMPTY rather than SHUTDOWN because the caller passed a
     * NULL handle — there's no actor to attribute a shutdown
     * reason to). Just confirm no crash. */
    g_test_expect_message ("hxnet", G_LOG_LEVEL_CRITICAL, "*NULL arg*");
    int r = hxnet_connection_try_recv_frame (NULL, &f, &reason);
    g_test_assert_expected_messages ();
    g_assert_cmpint (r, ==, HXNET_RECV_EMPTY);

    /* NULL handle → INVALID. The FFI checks the handle first,
     * before the data/len pair, so this is the one critical that
     * actually fires when both arguments are NULL. */
    g_test_expect_message ("hxnet", G_LOG_LEVEL_CRITICAL, "*NULL handle*");
    r = hxnet_connection_send_frame (NULL, NULL, 4);
    g_test_assert_expected_messages ();
    g_assert_cmpint (r, ==, HXNET_SEND_INVALID);

    /* destroy + frame_free are NULL-safe no-ops. */
    hxnet_connection_destroy (NULL);
    hxnet_frame_free (NULL);
}

int
main (int argc, char *argv[])
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/hxnet/ffi/round-trip-frame",
                     test_round_trip_single_frame);
    g_test_add_func ("/hxnet/ffi/send-frame-round-trip",
                     test_send_frame_round_trip);
    g_test_add_func ("/hxnet/ffi/invalid-args", test_invalid_args_return_invalid_not_crash);
    return g_test_run ();
}
