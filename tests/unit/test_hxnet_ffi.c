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

/* R3.3.e callback FFI. The on_event callback fires on the GLib
 * main thread per Event::Frame; on_shutdown fires once when the
 * actor exits. user_data is opaque.
 *
 * `frame` is passed as `hxnet_frame *` (not const): the C side
 * is expected to call `hxnet_frame_free(frame)` to release the
 * body, and that function writes through the struct to zero
 * body_ptr / body_len. Marking it const would force a const-
 * cast at every correct call site. */
typedef void (*hxnet_event_cb) (hxnet_connection *conn, hxnet_frame *frame,
                                void *user_data);
typedef void (*hxnet_shutdown_cb) (hxnet_connection *conn, int reason,
                                   void *user_data);
extern hxnet_connection *hxnet_connection_spawn_fd_with_callback (
    int fd, hxnet_event_cb on_event, hxnet_shutdown_cb on_shutdown,
    void *user_data);

/* R3.3.e-2 transform-config FFI. Lets the C side hand hxnet a
 * cipher + compression stack to compose around the adopted TCP
 * socket before the Connection actor sees it. The Rust side
 * pins this struct's offsets + size via const-asserts in
 * rust/crates/hxnet/src/ffi.rs; the _Static_asserts below
 * mirror those so drift on either side breaks both compiles. */
#define HXNET_CIPHER_NONE              0
#define HXNET_CIPHER_BLOWFISH          1
#define HXNET_CIPHER_CHACHA20_POLY1305 2

#define HXNET_COMPRESSION_NONE 0
#define HXNET_COMPRESSION_GZIP 1
#define HXNET_COMPRESSION_LZ4  2
#define HXNET_COMPRESSION_ZSTD 3

typedef struct {
    unsigned int cipher_kind;
    unsigned int compression_kind;
    unsigned int blowfish_key_len;
    uint8_t      blowfish_key[56];
    uint8_t      blowfish_read_ivec[8];
    uint8_t      blowfish_write_ivec[8];
    uint8_t      aead_read_key[32];
    uint8_t      aead_write_key[32];
    uint64_t     aead_read_counter;
    uint64_t     aead_write_counter;
    uint8_t      aead_read_dir;
    uint8_t      aead_write_dir;
    uint8_t      _pad[6];
} hxnet_transform_config;

_Static_assert (offsetof (hxnet_transform_config, cipher_kind) == 0,
                "hxnet_transform_config.cipher_kind offset drift");
_Static_assert (offsetof (hxnet_transform_config, compression_kind) == 4,
                "hxnet_transform_config.compression_kind offset drift");
_Static_assert (offsetof (hxnet_transform_config, blowfish_key_len) == 8,
                "hxnet_transform_config.blowfish_key_len offset drift");
_Static_assert (offsetof (hxnet_transform_config, blowfish_key) == 12,
                "hxnet_transform_config.blowfish_key offset drift");
_Static_assert (offsetof (hxnet_transform_config, blowfish_read_ivec) == 68,
                "hxnet_transform_config.blowfish_read_ivec offset drift");
_Static_assert (offsetof (hxnet_transform_config, blowfish_write_ivec) == 76,
                "hxnet_transform_config.blowfish_write_ivec offset drift");
_Static_assert (offsetof (hxnet_transform_config, aead_read_key) == 84,
                "hxnet_transform_config.aead_read_key offset drift");
_Static_assert (offsetof (hxnet_transform_config, aead_write_key) == 116,
                "hxnet_transform_config.aead_write_key offset drift");
_Static_assert (offsetof (hxnet_transform_config, aead_read_counter) == 152,
                "hxnet_transform_config.aead_read_counter offset drift");
_Static_assert (offsetof (hxnet_transform_config, aead_write_counter) == 160,
                "hxnet_transform_config.aead_write_counter offset drift");
_Static_assert (offsetof (hxnet_transform_config, aead_read_dir) == 168,
                "hxnet_transform_config.aead_read_dir offset drift");
_Static_assert (offsetof (hxnet_transform_config, aead_write_dir) == 169,
                "hxnet_transform_config.aead_write_dir offset drift");
_Static_assert (sizeof (hxnet_transform_config) == 176,
                "hxnet_transform_config size drift");

extern hxnet_connection *hxnet_connection_spawn_fd_with_transforms_and_callback (
    int fd, const hxnet_transform_config *config,
    hxnet_event_cb on_event, hxnet_shutdown_cb on_shutdown, void *user_data);

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

/* ------------------------------------------------------------------- *
 * Callback-FFI smoke test (Phase R3.3.e). spawn_fd_with_callback
 * routes events through the hxbridge ferry to the GLib main loop
 * and invokes the C callback per frame. We use GMainContext
 * iteration to drive the dispatch in the test (the production
 * path uses the GtkApplication's main loop). */

struct callback_state {
    int frames_seen;
    int shutdown_seen;
    int shutdown_reason;
    uint32_t last_type;
    uint32_t last_trans;
    uint32_t last_body_len;
    uint8_t last_body[32];
    /* Capture the handle pointer the callback received so we
     * can verify it matches what spawn_fd_with_callback
     * returned. */
    void *got_handle;
};

static void
test_on_event (hxnet_connection *conn, hxnet_frame *frame, void *user_data)
{
    struct callback_state *s = user_data;
    s->got_handle = conn;
    s->last_type = frame->type_;
    s->last_trans = frame->trans;
    s->last_body_len = frame->body_len;
    if (frame->body_len <= sizeof (s->last_body)) {
        memcpy (s->last_body, frame->body_ptr, frame->body_len);
    }
    s->frames_seen++;
    /* C side owns the body until it calls hxnet_frame_free.
     * The callback's frame pointer is mutable per the FFI
     * contract — no const-cast required. */
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

/* Iterate the default GMainContext until either `pred(state)` is
 * true or `deadline` elapses. Returns true if pred satisfied. */
static gboolean
pump_main_until (gboolean (*pred) (struct callback_state *),
                 struct callback_state *state, gint64 deadline_us)
{
    GMainContext *ctx = g_main_context_default ();
    gint64 start = g_get_monotonic_time ();
    while (g_get_monotonic_time () - start < deadline_us) {
        if (pred (state)) {
            return TRUE;
        }
        g_main_context_iteration (ctx, FALSE);
        g_usleep (1000);
    }
    return pred (state);
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

static void
test_callback_frame_then_shutdown (void)
{
    int sv[2];
    tcp_loopback_pair (sv);

    struct callback_state state;
    memset (&state, 0, sizeof (state));

    hxnet_connection *conn = hxnet_connection_spawn_fd_with_callback (
        sv[1], test_on_event, test_on_shutdown, &state);
    g_assert_nonnull (conn);

    /* Write a frame from the server side. */
    uint8_t hdr[22];
    build_header (hdr, 0x69, 7, 0, 4, 0);
    write_all_or_die (sv[0], hdr, sizeof (hdr));
    write_all_or_die (sv[0], (const uint8_t *) "abcd", 4);

    /* Pump main context until the callback fires. */
    g_assert_true (pump_main_until (saw_one_frame, &state, 5 * 1000000));
    g_assert_cmpint (state.frames_seen, ==, 1);
    g_assert_cmpuint (state.last_type, ==, 0x69);
    g_assert_cmpuint (state.last_trans, ==, 7);
    g_assert_cmpuint (state.last_body_len, ==, 4);
    g_assert_cmpmem (state.last_body, state.last_body_len, "abcd", 4);
    g_assert_true (state.got_handle == conn);

    /* Close the server side; shutdown callback should fire with
     * HXNET_SHUTDOWN_EOF. */
    g_assert_cmpint (close (sv[0]), ==, 0);
    g_assert_true (pump_main_until (saw_shutdown, &state, 5 * 1000000));
    g_assert_cmpint (state.shutdown_seen, ==, 1);
    g_assert_cmpint (state.shutdown_reason, ==, HXNET_SHUTDOWN_EOF);

    hxnet_connection_destroy (conn);
}

static void
test_callback_null_arg_rejects (void)
{
    /* NULL on_event must be rejected with g_critical + NULL. */
    g_test_expect_message ("hxnet", G_LOG_LEVEL_CRITICAL, "*NULL callback*");
    hxnet_connection *r = hxnet_connection_spawn_fd_with_callback (
        0, NULL, test_on_shutdown, NULL);
    g_test_assert_expected_messages ();
    g_assert_null (r);

    /* NULL on_shutdown likewise. */
    g_test_expect_message ("hxnet", G_LOG_LEVEL_CRITICAL, "*NULL callback*");
    r = hxnet_connection_spawn_fd_with_callback (0, test_on_event, NULL, NULL);
    g_test_assert_expected_messages ();
    g_assert_null (r);
}

static void
test_transforms_null_config_rejected (void)
{
    /* NULL config must be rejected with g_critical + NULL — no
     * fd is adopted, so nothing leaks. */
    g_test_expect_message ("hxnet", G_LOG_LEVEL_CRITICAL, "*NULL config*");
    hxnet_connection *r =
        hxnet_connection_spawn_fd_with_transforms_and_callback (
            0, NULL, test_on_event, test_on_shutdown, NULL);
    g_test_assert_expected_messages ();
    g_assert_null (r);
}

static void
test_transforms_unknown_kind_rejected (void)
{
    hxnet_transform_config cfg;
    memset (&cfg, 0, sizeof (cfg));
    cfg.cipher_kind = 42; /* not a defined HXNET_CIPHER_* */
    cfg.compression_kind = HXNET_COMPRESSION_NONE;

    g_test_expect_message ("hxnet", G_LOG_LEVEL_CRITICAL, "*unknown cipher_kind*");
    hxnet_connection *r =
        hxnet_connection_spawn_fd_with_transforms_and_callback (
            0, &cfg, test_on_event, test_on_shutdown, NULL);
    g_test_assert_expected_messages ();
    g_assert_null (r);
}

static void
test_transforms_passthrough_round_trip (void)
{
    /* Smoke test: passthrough config (cipher=NONE,
     * compression=NONE) should behave identically to the
     * non-transform spawn entry. We send a Hotline-framed
     * message in, expect the callback to fire with the matching
     * frame, then close + verify shutdown.
     *
     * The point is exercising the C ABI surface end to end —
     * the C side builds an hxnet_transform_config, the Rust
     * side decodes it, composes a no-op stack, and the
     * Connection actor still drives the same Event::Frame path
     * the non-transform variant uses. The cipher / compression
     * variants are covered by the Rust-side transform.rs
     * integration tests; the C smoke here just verifies that
     * the new spawn entry is wired up. */
    int sv[2];
    tcp_loopback_pair (sv);

    struct callback_state state;
    memset (&state, 0, sizeof (state));

    hxnet_transform_config cfg;
    memset (&cfg, 0, sizeof (cfg));
    cfg.cipher_kind = HXNET_CIPHER_NONE;
    cfg.compression_kind = HXNET_COMPRESSION_NONE;

    hxnet_connection *conn =
        hxnet_connection_spawn_fd_with_transforms_and_callback (
            sv[1], &cfg, test_on_event, test_on_shutdown, &state);
    g_assert_nonnull (conn);

    /* Send a frame using the same builder the callback test
     * uses — type 0x69, trans 7, flag 0, body "abcd". */
    uint8_t hdr[22];
    build_header (hdr, 0x69, 7, 0, 4, 0);
    write_all_or_die (sv[0], hdr, sizeof (hdr));
    write_all_or_die (sv[0], (const uint8_t *) "abcd", 4);

    g_assert_true (pump_main_until (saw_one_frame, &state, 5 * 1000000));
    g_assert_cmpint (state.frames_seen, ==, 1);
    g_assert_cmpuint (state.last_type, ==, 0x69);
    g_assert_cmpuint (state.last_trans, ==, 7);
    g_assert_cmpuint (state.last_body_len, ==, 4);
    g_assert_cmpmem (state.last_body, state.last_body_len, "abcd", 4);

    g_assert_cmpint (close (sv[0]), ==, 0);
    g_assert_true (pump_main_until (saw_shutdown, &state, 5 * 1000000));
    g_assert_cmpint (state.shutdown_reason, ==, HXNET_SHUTDOWN_EOF);

    hxnet_connection_destroy (conn);
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
    g_test_add_func ("/hxnet/ffi/callback-frame-then-shutdown",
                     test_callback_frame_then_shutdown);
    g_test_add_func ("/hxnet/ffi/callback-null-arg-rejects",
                     test_callback_null_arg_rejects);
    g_test_add_func ("/hxnet/ffi/transforms-null-config-rejected",
                     test_transforms_null_config_rejected);
    g_test_add_func ("/hxnet/ffi/transforms-unknown-kind-rejected",
                     test_transforms_unknown_kind_rejected);
    g_test_add_func ("/hxnet/ffi/transforms-passthrough-round-trip",
                     test_transforms_passthrough_round_trip);
    return g_test_run ();
}
