/*
 * test_hxnet_ffi.c — smoke test for the hxnet C-callable FFI.
 *
 * hxnet creates connections through its connect entry points, which
 * resolve + connect the socket INSIDE Rust — no OS socket fd ever
 * crosses the FFI. This test drives the fd-free `hxnet_connection_open_tcp`
 * entry against an in-process loopback listener: it binds a TCP listener
 * on 127.0.0.1, hands hxnet the host:port (hxnet connects), then accepts
 * the incoming connection to get the server side it drives by hand.
 *
 * Covers:
 *   - open_tcp connects and starts the actor; the event callback fires
 *     per frame with the correct fields (routed through the hxbridge
 *     ferry to the GLib main loop);
 *   - the shutdown callback fires with HXNET_SHUTDOWN_EOF on peer close;
 *   - send_frame round-trips bytes to the peer;
 *   - NULL-arg handling on try_recv_frame / send_frame / destroy /
 *     frame_free (g_critical + failure code, never a crash);
 *   - the HxnetFrame struct ABI (offsets / size / alignment).
 *
 * The C side mirrors the FFI's constants by hand — the established
 * discipline for hxbridge / hotline-proto FFI (drift surfaces as a
 * link-time undefined symbol or a test failure).
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

extern int hxnet_connection_try_recv_frame (hxnet_connection *handle,
                                            hxnet_frame *out_frame,
                                            int *out_reason);
extern int hxnet_connection_send_frame (hxnet_connection *handle,
                                        const uint8_t *data, unsigned int len);
extern void hxnet_connection_destroy (hxnet_connection *handle);
extern void hxnet_frame_free (hxnet_frame *f);

/* Callback FFI. on_event fires on the GLib main thread per Event::Frame;
 * on_shutdown fires once when the actor exits; on_state (optional, may be
 * NULL) fires per connection-state transition. user_data is opaque.
 *
 * `frame` is passed as `hxnet_frame *` (not const): the C side is
 * expected to call `hxnet_frame_free(frame)` to release the body, and
 * that function writes through the struct to zero body_ptr / body_len. */
typedef void (*hxnet_event_cb) (hxnet_connection *conn, hxnet_frame *frame,
                                void *user_data);
typedef void (*hxnet_shutdown_cb) (hxnet_connection *conn, int reason,
                                   void *user_data);
typedef void (*hxnet_state_cb) (hxnet_connection *conn, unsigned int state,
                                void *user_data);

/* fd-free connect entry: hxnet resolves + connects host:port on the
 * shared tokio runtime and routes events through the callbacks. */
extern hxnet_connection *hxnet_connection_open_tcp (
    const uint8_t *host, size_t host_len, uint16_t port,
    hxnet_event_cb on_event, hxnet_shutdown_cb on_shutdown,
    hxnet_state_cb on_state, void *user_data);

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

/* Bind a loopback listener on 127.0.0.1:0, have hxnet connect to it via
 * the fd-free open_tcp entry, and return the handle in *out_conn plus the
 * accepted server side in *out_server (the end the test drives by hand).
 * hxnet owns the client socket; the caller destroys the handle. Aborts
 * via g_error on any setup failure.
 *
 * open_tcp connects asynchronously on the tokio runtime, so accept()
 * below blocks until the connection lands — no ordering race. */
static void
open_tcp_to_listener (hxnet_event_cb on_event, hxnet_shutdown_cb on_shutdown,
                      void *user_data, hxnet_connection **out_conn,
                      int *out_server)
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

    socklen_t alen = sizeof (addr);
    if (getsockname (listener, (struct sockaddr *) &addr, &alen) < 0) {
        g_error ("getsockname: %s", g_strerror (errno));
    }
    uint16_t port = ntohs (addr.sin_port);

    const char *host = "127.0.0.1";
    hxnet_connection *conn = hxnet_connection_open_tcp (
        (const uint8_t *) host, strlen (host), port, on_event, on_shutdown,
        /*on_state=*/NULL, user_data);
    if (!conn) {
        g_error ("hxnet_connection_open_tcp returned NULL");
    }

    int server = accept (listener, NULL, NULL);
    if (server < 0) {
        g_error ("accept: %s", g_strerror (errno));
    }
    close (listener);

    *out_conn = conn;
    *out_server = server;
}

/* Callback state + main-loop pump shared by the connect tests. */

struct callback_state {
    int frames_seen;
    int shutdown_seen;
    int shutdown_reason;
    uint32_t last_type;
    uint32_t last_trans;
    uint32_t last_body_len;
    uint8_t last_body[32];
    /* Capture the handle pointer the callback received so we can verify
     * it matches what open_tcp returned. */
    void *got_handle;
};

static void
test_on_event (hxnet_connection *conn, hxnet_frame *frame, void *user_data)
{
    struct callback_state *s = user_data;
    s->got_handle = conn;
    s->last_type = frame->type_;
    s->last_trans = frame->trans;
    /* Clamp the recorded length to what we actually copied — otherwise a
     * future test sending a body > sizeof(last_body) would leave
     * last_body_len exceeding the bytes-in-buffer and the assert_cmpmem
     * path could read past the array. */
    uint32_t cap = (uint32_t) sizeof (s->last_body);
    s->last_body_len = frame->body_len > cap ? cap : frame->body_len;
    memcpy (s->last_body, frame->body_ptr, s->last_body_len);
    s->frames_seen++;
    /* C side owns the body until it calls hxnet_frame_free. */
    hxnet_frame_free (frame);
}

static void
test_on_shutdown (hxnet_connection *conn, int reason, void *user_data)
{
    struct callback_state *s = user_data;
    s->got_handle = conn;
    s->shutdown_reason = reason;
    s->shutdown_seen++;
}

/* Iterate the thread-default GMainContext until either `pred(state)` is
 * true or `deadline` elapses. Returns true if pred satisfied.
 *
 * Uses ref_thread_default(), not g_main_context_default(): the Rust
 * callback forwarder attaches its idle source to the thread-default
 * context (MainContext::ref_thread_default on the Rust side). */
static gboolean
pump_main_until (gboolean (*pred) (struct callback_state *),
                 struct callback_state *state, gint64 deadline_us)
{
    GMainContext *ctx = g_main_context_ref_thread_default ();
    gint64 start = g_get_monotonic_time ();
    while (g_get_monotonic_time () - start < deadline_us) {
        if (pred (state)) {
            g_main_context_unref (ctx);
            return TRUE;
        }
        g_main_context_iteration (ctx, FALSE);
        g_usleep (1000);
    }
    gboolean result = pred (state);
    g_main_context_unref (ctx);
    return result;
}

static gboolean
saw_one_frame (struct callback_state *s)
{
    return s->frames_seen >= 1;
}

static gboolean
saw_shutdown (struct callback_state *s)
{
    return s->shutdown_seen >= 1;
}

/* ------------------------------------------------------------------- *
 * Test: a frame written by the server side arrives via the event
 * callback with correct fields; peer close fires the shutdown callback
 * with HXNET_SHUTDOWN_EOF. */
static void
test_open_tcp_frame_then_shutdown (void)
{
    struct callback_state state;
    memset (&state, 0, sizeof (state));

    hxnet_connection *conn = NULL;
    int server = -1;
    open_tcp_to_listener (test_on_event, test_on_shutdown, &state, &conn,
                          &server);

    /* Write a frame from the server side. */
    uint8_t hdr[22];
    build_header (hdr, 0x69, 7, 0, 4, 0);
    write_all_or_die (server, hdr, sizeof (hdr));
    write_all_or_die (server, (const uint8_t *) "abcd", 4);

    /* Pump main context until the callback fires. */
    g_assert_true (pump_main_until (saw_one_frame, &state, 5 * 1000000));
    g_assert_cmpint (state.frames_seen, ==, 1);
    g_assert_cmpuint (state.last_type, ==, 0x69);
    g_assert_cmpuint (state.last_trans, ==, 7);
    g_assert_cmpuint (state.last_body_len, ==, 4);
    g_assert_cmpmem (state.last_body, state.last_body_len, "abcd", 4);
    g_assert_true (state.got_handle == conn);

    /* Close the server side; shutdown callback should fire with EOF. */
    g_assert_cmpint (close (server), ==, 0);
    g_assert_true (pump_main_until (saw_shutdown, &state, 5 * 1000000));
    g_assert_cmpint (state.shutdown_seen, ==, 1);
    g_assert_cmpint (state.shutdown_reason, ==, HXNET_SHUTDOWN_EOF);

    hxnet_connection_destroy (conn);
}

/* No-op callbacks for the send-frame test (it inspects the peer socket,
 * not the callbacks). open_tcp requires both slots non-NULL. */
static void
noop_event (hxnet_connection *conn, hxnet_frame *frame, void *user_data)
{
    (void) conn;
    (void) user_data;
    hxnet_frame_free (frame);
}

static void
noop_shutdown (hxnet_connection *conn, int reason, void *user_data)
{
    (void) conn;
    (void) reason;
    (void) user_data;
}

/* ------------------------------------------------------------------- *
 * Test: send_frame writes bytes to the peer. */
static void
test_send_frame_round_trip (void)
{
    hxnet_connection *conn = NULL;
    int server = -1;
    open_tcp_to_listener (noop_event, noop_shutdown, NULL, &conn, &server);

    /* Build a full frame as bytes and ship via send_frame. */
    uint8_t buf[22 + 5];
    build_header (buf, 0x6b, 99, 0, 5, 0);
    memcpy (&buf[22], "login", 5);

    int r = hxnet_connection_send_frame (conn, buf, sizeof (buf));
    g_assert_cmpint (r, ==, HXNET_SEND_OK);

    /* Server side reads the bytes back. */
    uint8_t got[27] = { 0 };
    read_all_or_die (server, got, sizeof (got));
    g_assert_cmpmem (got, sizeof (got), buf, sizeof (buf));

    g_assert_cmpint (close (server), ==, 0);
    hxnet_connection_destroy (conn);
}

/* ------------------------------------------------------------------- *
 * Test: NULL handle / NULL data fall through cleanly.
 *
 * The FFI's policy on invalid args is "g_critical + return a failure
 * code, never crash." GTest's default fatal-mask promotes criticals to
 * abort; we use g_test_expect_message to acknowledge each expected
 * critical so the test doesn't bail. */
static void
test_invalid_args_return_invalid_not_crash (void)
{
    int reason = 0;
    hxnet_frame f;
    memset (&f, 0, sizeof (f));

    /* NULL handle to try_recv_frame returns EMPTY (there's no actor to
     * attribute a shutdown reason to). Just confirm no crash. */
    g_test_expect_message ("hxnet", G_LOG_LEVEL_CRITICAL, "*NULL arg*");
    int r = hxnet_connection_try_recv_frame (NULL, &f, &reason);
    g_test_assert_expected_messages ();
    g_assert_cmpint (r, ==, HXNET_RECV_EMPTY);

    /* NULL handle → INVALID. The FFI checks the handle first, before the
     * data/len pair, so this is the one critical that fires. */
    g_test_expect_message ("hxnet", G_LOG_LEVEL_CRITICAL, "*NULL handle*");
    r = hxnet_connection_send_frame (NULL, NULL, 4);
    g_test_assert_expected_messages ();
    g_assert_cmpint (r, ==, HXNET_SEND_INVALID);

    /* destroy + frame_free are NULL-safe no-ops. */
    hxnet_connection_destroy (NULL);
    hxnet_frame_free (NULL);
}

/* ------------------------------------------------------------------- *
 * Test: open_tcp rejects NULL host / NULL callbacks with g_critical +
 * NULL, no connection attempted. */
static void
test_open_tcp_null_args_rejected (void)
{
    g_test_expect_message ("hxnet", G_LOG_LEVEL_CRITICAL, "*NULL or empty host*");
    hxnet_connection *r = hxnet_connection_open_tcp (
        NULL, 0, 5500, noop_event, noop_shutdown, NULL, NULL);
    g_test_assert_expected_messages ();
    g_assert_null (r);

    const char *host = "127.0.0.1";
    g_test_expect_message ("hxnet", G_LOG_LEVEL_CRITICAL,
                           "*NULL on_event*");
    r = hxnet_connection_open_tcp ((const uint8_t *) host, strlen (host), 5500,
                                   NULL, noop_shutdown, NULL, NULL);
    g_test_assert_expected_messages ();
    g_assert_null (r);
}

int
main (int argc, char *argv[])
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/hxnet/ffi/open-tcp-frame-then-shutdown",
                     test_open_tcp_frame_then_shutdown);
    g_test_add_func ("/hxnet/ffi/send-frame-round-trip",
                     test_send_frame_round_trip);
    g_test_add_func ("/hxnet/ffi/invalid-args",
                     test_invalid_args_return_invalid_not_crash);
    g_test_add_func ("/hxnet/ffi/open-tcp-null-args-rejected",
                     test_open_tcp_null_args_rejected);
    return g_test_run ();
}
