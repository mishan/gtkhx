/*
 * test_hxnet_bridge.c — Tier 1 tests for the R3.3.e-4a hxnet
 * bridge module.
 *
 * The module ships two ingest paths:
 *
 *   * `gtkhx_proto_pack_header` (hotline-proto) — turns a (type,
 *     trans, flag, hc, body_len) tuple into a 22-byte Hotline
 *     header. The bridge calls it to reconstruct the header of a
 *     frame the hxnet actor already parsed; tested here against the
 *     production `hl_hdr_decode` to lock down the wire-format
 *     round-trip without standing up an htlc_conn.
 *
 *   * `hx_bridge_dispatch_frame` / `hx_bridge_dispatch_shutdown`
 *     — exercise the rcv state machine. Those need a populated
 *     htlc_conn and a body handler hook; they're integration-
 *     shaped and are covered by the Tier 3 suite now that the
 *     bridge is wired into the production connect path
 *     (e.g. real_connect, test_hope_blowfish_hxnet).
 *     Tier 1 here verifies the translation contract those callers
 *     rely on.
 *
 * Production discipline: the test links only proto_helpers.c
 * alongside the bridge, so we don't drag the GUI tree into the
 * Tier 1 binary. The hl_hdr_decode round-trip is the load-
 * bearing assertion — if the encoder and decoder ever drift,
 * production's rcv path would silently dispatch on the wrong
 * type.
 */

#include <glib.h>
#include <stdint.h>
#include <string.h>

#include "config.h"
#include "hxconn.h"
#include "hxnet_bridge.h"
#include "hotline_proto.h" /* gtkhx_proto_pack_header (wire header encode) */
#include "proto_helpers.h"
#include "protocol.h"

/* proto_helpers.c pulls in gtkhx_text_to_utf8 transitively via
 * its chat / msg event helpers, but those code paths are
 * unreachable from the header-decode entry we exercise here.
 * Supply a stub so the link succeeds without dragging text_util.c
 * (and through it the Pango / glycin tree) into a Tier 1 test. */
char *gtkhx_text_to_utf8 (const char *bytes, gsize len, gsize *out_len);
char *
gtkhx_text_to_utf8 (const char *bytes, gsize len, gsize *out_len)
{
    (void)bytes;
    (void)len;
    (void)out_len;
    g_assert_not_reached ();
    return NULL;
}

/* Same deal for the phase E6 emoji-shortcode toggle accessor, which
 * proto_helpers.c now consults in its chat / msg decode path — also
 * unreachable from the header-decode entry exercised here. */
gboolean gtkhx_text_emoji_shortcodes_enabled (void);
gboolean
gtkhx_text_emoji_shortcodes_enabled (void)
{
    return FALSE;
}

/* hxnet_bridge.c references the production receive dispatch
 * (hx_dispatch_frame) and qbuf_set / hx_htlc_close to drive a real
 * htlc_conn. Those are exercised by the wider integration tests once
 * the bridge is wired into the connection lifecycle. Tier 1 here only
 * links the bridge's header-pack path, so the linker just needs
 * unresolved symbols resolved. proto_helpers.c needs debug_log for its
 * trace helpers; same treatment. */
void hx_dispatch_frame (struct htlc_conn *htlc, const guint8 *frame,
                        gsize frame_len, guint32 type, guint32 trans,
                        guint32 flag, guint32 body_len);
void hx_htlc_close (struct htlc_conn *htlc, int expected);
void qbuf_set (struct qbuf *q, guint32 pos, guint32 len);
void debug_log (const char *cat, const char *fmt, ...);
void hx_orchestrator_register_login_task (struct htlc_conn *htlc);

/* Recording, not fatal: the stale-actor guard tests below are precisely
 * about whether a frame reaches dispatch, so the stub has to report that
 * rather than abort. */
static int dispatch_calls;
static struct htlc_conn *last_dispatch_htlc;
static guint32 last_dispatch_type;

void
hx_dispatch_frame (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len,
                   guint32 type, guint32 trans, guint32 flag, guint32 body_len)
{
    (void)frame;
    (void)frame_len;
    (void)trans;
    (void)flag;
    (void)body_len;
    dispatch_calls++;
    last_dispatch_htlc = htlc;
    last_dispatch_type = type;
}

/* Stub: bridge_on_state_cb calls this on LOGIN_SENDING, but these
 * unit tests drive the header-pack / dispatch paths directly and
 * never feed a LOGIN_SENDING state, so it should never fire. */
void
hx_orchestrator_register_login_task (struct htlc_conn *htlc)
{
    (void)htlc;
    g_assert_not_reached ();
}

void
hx_htlc_close (struct htlc_conn *htlc, int expected)
{
    (void)htlc;
    (void)expected;
    g_assert_not_reached ();
}

void
qbuf_set (struct qbuf *q, guint32 pos, guint32 len)
{
    (void)q;
    (void)pos;
    (void)len;
    g_assert_not_reached ();
}

void
debug_log (const char *cat, const char *fmt, ...)
{
    (void)cat;
    (void)fmt;
    /* No-op: proto_helpers calls into this for trace events
     * that the test paths don't trigger. */
}

/* R3.3.e-4b's bridge install / send / uninstall helpers wrap hxnet's
 * callback FFI. Tier 1 only exercises the header-pack path, so we stub
 * the hxnet symbols the bridge references (rather than dragging the
 * staticlib + tokio runtime into a smoke test). The lifecycle helpers
 * themselves are covered by the production network.c hookups + Tier 3.
 *
 * Most stubs are g_assert_not_reached and exist only to satisfy the link.
 * The handful the per-connection tests below actually drive — open_plaintext,
 * send_frame, destroy, frame_free, dispatch_frame — record instead, because
 * what those tests assert is *which* connection the bridge reached for. */
struct hxnet_connection_opaque;
/* The frame struct the event callback receives. Field-for-field the same as
 * hxnet_bridge.c's local mirror of hxnet's `hxnet_frame_t`; declared again
 * here because that one is file-local to the bridge. */
struct hxnet_frame_t {
    guint32 type_;
    guint32 trans;
    guint32 flag;
    guint16 hc;
    guint16 _pad;
    guint32 body_len;
    guint8 *body_ptr;
};

/* Callback function-pointer types shared by the connect-entry stubs
 * below (open_plaintext / open_hope / open_plaintext_tls). Match the
 * production declarations in hxnet_bridge.c (and through it
 * rust/crates/hxnet/src/ffi.rs) — declaring the callback parameters as
 * `void *` would be undefined behavior under C's function-type
 * compatibility rules. */
typedef void (*test_stub_event_cb) (struct hxnet_connection_opaque *conn,
                                    struct hxnet_frame_t *frame,
                                    void *user_data);
typedef void (*test_stub_shutdown_cb) (struct hxnet_connection_opaque *conn,
                                       int reason, void *user_data);

int hxnet_connection_send_frame (struct hxnet_connection_opaque *handle,
                                 const guint8 *data, guint32 len);
void hxnet_connection_destroy (struct hxnet_connection_opaque *handle);
void hxnet_frame_free (struct hxnet_frame_t *f);

/* Recording rather than fatal: the per-connection routing tests below are
 * about *which* handle the bridge reaches for, so the stub has to report it
 * instead of refusing to be called. Everything else in this file still never
 * touches the transport. */
static struct hxnet_connection_opaque *last_send_handle;
static guint32 last_send_len;
static struct hxnet_connection_opaque *last_destroyed_handle;
static int destroy_calls;

int
hxnet_connection_send_frame (struct hxnet_connection_opaque *handle,
                             const guint8 *data, guint32 len)
{
    (void)data;
    last_send_handle = handle;
    last_send_len = len;
    return 0;
}

void
hxnet_connection_destroy (struct hxnet_connection_opaque *handle)
{
    last_destroyed_handle = handle;
    destroy_calls++;
}

/* The bridge frees every frame it is handed, stale or not — that is the
 * ownership contract, and the guard tests assert it holds on the drop path
 * too. Recording rather than fatal for the same reason as the others. */
static int frame_free_calls;

void
hxnet_frame_free (struct hxnet_frame_t *f)
{
    frame_free_calls++;
    /* Actually free it. The contract this stub models is that the callee owns
     * the frame, and the guard tests assert the bridge honours that even on
     * the drop path — a stub that only counted would make those assertions
     * pass while leaking every frame they allocate. */
    g_free (f);
}

/* Phase G HTXF-AEAD: hx_bridge_orchestrated_hope_aead() calls the Rust
 * material getter to seed htlc->hope_aead. Tier 1 never installs a
 * connection, so a g_assert_not_reached stub satisfies the link without
 * dragging in the staticlib. */
struct HxnetHopeAead;
struct HxnetHopeAead *
hxnet_connection_hope_aead_material (struct hxnet_connection_opaque *conn);
struct HxnetHopeAead *
hxnet_connection_hope_aead_material (struct hxnet_connection_opaque *conn)
{
    (void)conn;
    g_assert_not_reached ();
    return NULL;
}

/* R3.3.e-4d's hx_bridge_install_with_hope_state pulls in the
 * Blowfish OFB state-snapshot helper from hxcrypto::stream. The
 * Tier 1 test never invokes the install path, so a
 * g_assert_not_reached stub keeps the symbol satisfied without
 * dragging the staticlib in. */
void gtkhx_blowfish_ofb64_save_state (const void *state, guint8 *out_ivec,
                                      guint32 *out_num);
void
gtkhx_blowfish_ofb64_save_state (const void *state, guint8 *out_ivec,
                                 guint32 *out_num)
{
    (void)state;
    (void)out_ivec;
    (void)out_num;
    g_assert_not_reached ();
}

/* Phase G adds hx_bridge_install_orchestrated_plaintext, which
 * references hxnet_connection_open_plaintext (the lifecycle FFI)
 * and the GtkhxSession state-emit pair (for the coarse
 * connection-state signal mapping). Tier 1 never drives the
 * orchestrator path — it's covered by the production network.c
 * hookup + the Tier 3 real-mhxd test — so stub all three. The
 * open_plaintext signature mirrors the production hand-declared
 * extern in src/hxnet_bridge.c; the GtkhxSession stubs use an
 * opaque local type (the test doesn't include gtkhx_session.h),
 * which is fine for linking. */
typedef void (*test_stub_state_cb) (struct hxnet_connection_opaque *conn,
                                    guint32 state, void *user_data);

/* What the open_plaintext stub records and returns. Driving a real install
 * is what makes the refusal and the stale-actor guards testable at Tier 1;
 * everything the bridge does with a handle is pointer identity, so a fake
 * address is indistinguishable from a real one to the code under test. */
static struct hxnet_connection_opaque *open_result;
static int open_calls;
static test_stub_event_cb last_open_event_cb;
static test_stub_shutdown_cb last_open_shutdown_cb;
static void *last_open_user_data;
typedef int (*test_stub_verify_cb) (const guint8 *fp, gsize fp_len,
                                    void *user_data);
struct hxnet_connection_opaque *hxnet_connection_open_plaintext (
    const guint8 *host, gsize host_len, guint16 port, const guint8 *login,
    gsize login_len, const guint8 *password, gsize password_len,
    const guint8 *name, gsize name_len, guint16 icon, guint16 version,
    guint16 caps, guint32 trans, const guint8 *proxy_uri, gsize proxy_uri_len,
    test_stub_event_cb on_event, test_stub_shutdown_cb on_shutdown,
    test_stub_state_cb on_state, void *user_data);
struct hxnet_connection_opaque *
hxnet_connection_open_plaintext (
    const guint8 *host, gsize host_len, guint16 port, const guint8 *login,
    gsize login_len, const guint8 *password, gsize password_len,
    const guint8 *name, gsize name_len, guint16 icon, guint16 version,
    guint16 caps, guint32 trans, const guint8 *proxy_uri, gsize proxy_uri_len,
    test_stub_event_cb on_event, test_stub_shutdown_cb on_shutdown,
    test_stub_state_cb on_state, void *user_data)
{
    (void)host;
    (void)host_len;
    (void)port;
    (void)login;
    (void)login_len;
    (void)password;
    (void)password_len;
    (void)name;
    (void)name_len;
    (void)icon;
    (void)version;
    (void)caps;
    (void)trans;
    (void)proxy_uri;
    (void)proxy_uri_len;
    (void)on_state;
    /* Capture the callbacks and hand back whatever the test asked for, so
     * the install path can be driven without a socket. A test that wants two
     * connections distinguishable sets `open_result` between calls; setting
     * it to NULL forces the open-failed branch. */
    open_calls++;
    last_open_event_cb = on_event;
    last_open_shutdown_cb = on_shutdown;
    last_open_user_data = user_data;
    return open_result;
}

struct hxnet_connection_opaque *hxnet_connection_open_hope (
    const guint8 *host, gsize host_len, guint16 port, const guint8 *login,
    gsize login_len, const guint8 *password, gsize password_len,
    const guint8 *name, gsize name_len, guint16 icon, guint16 version,
    guint16 caps, guint32 trans, const guint8 *cipher_alg, gsize cipher_alg_len,
    const guint8 *proxy_uri, gsize proxy_uri_len, test_stub_event_cb on_event,
    test_stub_shutdown_cb on_shutdown, test_stub_state_cb on_state,
    void *user_data);
struct hxnet_connection_opaque *
hxnet_connection_open_hope (const guint8 *host, gsize host_len, guint16 port,
                            const guint8 *login, gsize login_len,
                            const guint8 *password, gsize password_len,
                            const guint8 *name, gsize name_len, guint16 icon,
                            guint16 version, guint16 caps, guint32 trans,
                            const guint8 *cipher_alg, gsize cipher_alg_len,
                            const guint8 *proxy_uri, gsize proxy_uri_len,
                            test_stub_event_cb on_event,
                            test_stub_shutdown_cb on_shutdown,
                            test_stub_state_cb on_state, void *user_data)
{
    (void)host;
    (void)host_len;
    (void)port;
    (void)login;
    (void)login_len;
    (void)password;
    (void)password_len;
    (void)name;
    (void)name_len;
    (void)icon;
    (void)version;
    (void)caps;
    (void)trans;
    (void)cipher_alg;
    (void)cipher_alg_len;
    (void)proxy_uri;
    (void)proxy_uri_len;
    (void)on_event;
    (void)on_shutdown;
    (void)on_state;
    (void)user_data;
    g_assert_not_reached ();
    return NULL;
}

struct hxnet_connection_opaque *hxnet_connection_open_plaintext_tls (
    const guint8 *host, gsize host_len, guint16 port, const guint8 *login,
    gsize login_len, const guint8 *password, gsize password_len,
    const guint8 *name, gsize name_len, guint16 icon, guint16 version,
    guint16 caps, guint32 trans, const guint8 *proxy_uri, gsize proxy_uri_len,
    test_stub_event_cb on_event, test_stub_shutdown_cb on_shutdown,
    test_stub_state_cb on_state, test_stub_verify_cb verify_cert,
    void *user_data);
struct hxnet_connection_opaque *
hxnet_connection_open_plaintext_tls (
    const guint8 *host, gsize host_len, guint16 port, const guint8 *login,
    gsize login_len, const guint8 *password, gsize password_len,
    const guint8 *name, gsize name_len, guint16 icon, guint16 version,
    guint16 caps, guint32 trans, const guint8 *proxy_uri, gsize proxy_uri_len,
    test_stub_event_cb on_event, test_stub_shutdown_cb on_shutdown,
    test_stub_state_cb on_state, test_stub_verify_cb verify_cert,
    void *user_data)
{
    (void)host;
    (void)host_len;
    (void)port;
    (void)login;
    (void)login_len;
    (void)password;
    (void)password_len;
    (void)name;
    (void)name_len;
    (void)icon;
    (void)version;
    (void)caps;
    (void)trans;
    (void)proxy_uri;
    (void)proxy_uri_len;
    (void)on_event;
    (void)on_shutdown;
    (void)on_state;
    (void)verify_cert;
    (void)user_data;
    g_assert_not_reached ();
    return NULL;
}

/* hx_bridge_dispatch_shutdown reads network.c's `connected` flag to
 * pick the shutdown log level. Tier 1 never drives that path, but the
 * symbol must resolve — define it here (test never reads it). */
int connected;

/* bridge_on_verify_cert_cb (TLS TOFU trampoline) calls the production
 * verify in network.c. Tier 1 never drives the TLS path, but the
 * symbol must resolve. */
struct htlc_conn;
gboolean hx_tls_orchestrator_verify_cert (struct htlc_conn *htlc,
                                          const char *fingerprint);
gboolean
hx_tls_orchestrator_verify_cert (struct htlc_conn *htlc,
                                 const char *fingerprint)
{
    (void)htlc;
    (void)fingerprint;
    g_assert_not_reached ();
    return FALSE;
}

/* gtkhx_session_get_default + gtkhx_session_emit_connection_state (called only
 * from hxnet_bridge.c's bridge_on_state_cb, which this Tier 1 test never drives)
 * resolve against the real gtkhx-core symbols this target already links for
 * hx_conn_*. They are NOT stubbed here: a local stub is a second strong
 * definition of a symbol libgtkhx_core also exports, which the release-profile
 * codegen layout packs into an object this target pulls (for hx_conn_fd) —
 * yielding a multiple-definition link error. One definition, from the library. */

/* Round-trip a single (type, trans, flag, hc, body_len) tuple
 * through pack_header → hl_hdr_decode and assert the fields
 * survive intact. body_len is the application-level body byte
 * count (no hc adjustment); the wire `len` field encodes
 * body_len + sizeof(hc), and hl_hdr_decode reverses that to
 * report body_len_out back to the caller. */
static void
assert_round_trip (guint32 type, guint32 trans, guint32 flag, guint16 hc,
                   guint32 body_len)
{
    guint8 hdr[SIZEOF_HL_HDR];
    gtkhx_proto_pack_header (hdr, type, trans, flag, hc, body_len);

    guint32 got_type, got_trans, got_flag;
    guint16 got_hc;
    guint32 got_wire_len, got_body_len;
    g_assert_true (hl_hdr_decode (hdr, &got_type, &got_trans, &got_flag,
                                  &got_hc, &got_wire_len, &got_body_len));

    g_assert_cmpuint (got_type, ==, type);
    g_assert_cmpuint (got_trans, ==, trans);
    g_assert_cmpuint (got_flag, ==, flag);
    g_assert_cmpuint (got_hc, ==, hc);
    g_assert_cmpuint (got_wire_len, ==, body_len + sizeof (guint16));
    g_assert_cmpuint (got_body_len, ==, body_len);
}

static void
test_pack_header_zero_body (void)
{
    /* The body-less case (e.g. an ack, or a request with no
     * data chunks). wire_len = sizeof(hc) = 2. */
    assert_round_trip (0x00010000, 0, 0, 0, 0);
}

static void
test_pack_header_typical_chat (void)
{
    /* Typical inbound chat reply: HTLS_HDR_CHAT (0x6a) with a
     * trans id, no flag bits, one data chunk, ~40 bytes of body. */
    assert_round_trip (0x0000006a, 0x12345678, 0, 1, 40);
}

static void
test_pack_header_task_error_flag (void)
{
    /* HTLS_HDR_TASK with the in_error bit set (flag & 1). */
    assert_round_trip (0x00010000, 42, 1, 1, 16);
}

static void
test_pack_header_large_body (void)
{
    /* Larger body. Verifies the u32 BE encoding holds past the
     * 16-bit boundary in the wire `len` field. */
    assert_round_trip (0x00000068, 0xffffffff, 0xffffffff, 7, 8192);
}

static void
test_pack_header_full_u32_fields (void)
{
    /* Maximum-coverage byte pattern across every field. Catches
     * byte-swap mistakes that wouldn't show up on small-integer
     * inputs. */
    assert_round_trip (0xdeadbeef, 0xcafef00d, 0x12345678, 0xa55a, 1024);
}

static void
test_pack_header_byte_layout (void)
{
    /* Explicit byte-level check independent of hl_hdr_decode —
     * confirms the 22-byte layout matches struct hl_hdr's
     * documented field order regardless of whether
     * hl_hdr_decode itself drifts. */
    guint8 hdr[SIZEOF_HL_HDR];
    /* Sentinel so we'd notice an off-by-one write past the end. */
    guint8 expected[SIZEOF_HL_HDR] = {
        /* type 0x01020304 BE */ 0x01,
        0x02,
        0x03,
        0x04,
        /* trans 0x05060708 BE */ 0x05,
        0x06,
        0x07,
        0x08,
        /* flag 0x090a0b0c BE */ 0x09,
        0x0a,
        0x0b,
        0x0c,
        /* len = body_len(=10) + sizeof(hc)=2 = 12, BE */
        0x00,
        0x00,
        0x00,
        0x0c,
        /* len2 == len (matches hlpack) */ 0x00,
        0x00,
        0x00,
        0x0c,
        /* hc 0x0d0e BE */ 0x0d,
        0x0e,
    };
    gtkhx_proto_pack_header (hdr, 0x01020304, 0x05060708, 0x090a0b0c, 0x0d0e,
                             10);
    g_assert_cmpmem (hdr, sizeof (hdr), expected, sizeof (expected));
}

/* ---- the transport handle is per-connection ---------------------------- *
 *
 * These are what M2 bought. The handle used to be a file-static, so all four
 * of these questions had one process-wide answer: a second connection's
 * frames went to whichever installed last, and asking "is this one up" or
 * tearing one down hit whatever was in the slot.
 *
 * No real transport is stood up. The bridge stores whatever pointer it is
 * given and compares it by identity, so two distinct non-NULL addresses are
 * enough to tell the connections apart — and using fakes keeps this a Tier 1
 * test with no socket, no actor and no server. */

/* Two sentinel addresses standing in for hxnet handles. Never dereferenced. */
static int fake_transport_a;
static int fake_transport_b;

#define FAKE_A ((struct hxnet_connection_opaque *)&fake_transport_a)
#define FAKE_B ((struct hxnet_connection_opaque *)&fake_transport_b)

static void
test_installed_is_per_connection (void)
{
    struct htlc_conn *a = hx_conn_new ();
    struct htlc_conn *b = hx_conn_new ();

    g_assert_false (hx_bridge_is_installed (a));
    g_assert_false (hx_bridge_is_installed (b));

    hx_conn_set_bridge_handle (a, FAKE_A);
    g_assert_true (hx_bridge_is_installed (a));
    /* The whole point: b is unaffected by a's install. */
    g_assert_false (hx_bridge_is_installed (b));

    hx_conn_set_bridge_handle (b, FAKE_B);
    g_assert_true (hx_bridge_is_installed (a));
    g_assert_true (hx_bridge_is_installed (b));

    hx_conn_set_bridge_handle (a, NULL);
    hx_conn_set_bridge_handle (b, NULL);
    hx_conn_free (a);
    hx_conn_free (b);
}

/* A NULL connection has no transport. Not a reachable production state —
 * every install refuses a NULL htlc before registering it as user_data — but
 * the helper is written to tolerate it and the shutdown guard leans on the
 * same tolerance, so pin it rather than leave it to inspection. */
static void
test_installed_null_connection (void)
{
    g_assert_false (hx_bridge_is_installed (NULL));
}

/* ---- install ---------------------------------------------------------- */

static void
reset_stub_state (void)
{
    open_calls = 0;
    open_result = FAKE_A;
    last_open_event_cb = NULL;
    last_open_shutdown_cb = NULL;
    last_open_user_data = NULL;
    dispatch_calls = 0;
    last_dispatch_htlc = NULL;
    frame_free_calls = 0;
    destroy_calls = 0;
    last_destroyed_handle = NULL;
    last_send_handle = NULL;
}

static gboolean
install_plaintext (struct htlc_conn *htlc)
{
    /* -1 is the orchestrator's "connected, but hxnet owns the socket"
     * sentinel (see hx_conn_set_fd in network.c). It matters here because
     * hx_bridge_dispatch_frame treats fd == 0 as closed and drops the frame
     * before the stale-actor guard is reached — a fresh connection is zeroed,
     * so without this the dispatch tests would pass for the wrong reason. */
    hx_conn_set_fd (htlc, -1);
    return hx_bridge_install_orchestrated_plaintext (
        htlc, "example.invalid", 5500, "login", "pass", "name", 0, 197, 0, 1);
}

/* The behaviour change M2 exists for: installing a transport on a second
 * connection while a first one is up. This used to be refused outright with
 * "a connection is already installed", which is what made a second
 * simultaneous connect impossible. */
static void
test_install_allows_a_second_connection (void)
{
    struct htlc_conn *a = hx_conn_new ();
    struct htlc_conn *b = hx_conn_new ();

    reset_stub_state ();
    open_result = FAKE_A;
    g_assert_true (install_plaintext (a));
    g_assert_true (hx_bridge_is_installed (a));

    open_result = FAKE_B;
    g_assert_true (install_plaintext (b));
    g_assert_cmpint (open_calls, ==, 2);

    /* Two live transports, each on its own connection, each reachable
     * independently. */
    g_assert_true (hx_bridge_is_installed (a));
    g_assert_true (hx_bridge_is_installed (b));
    hx_bridge_send_frame (a, (const guint8 *)"x", 1);
    g_assert_true (last_send_handle == FAKE_A);
    hx_bridge_send_frame (b, (const guint8 *)"x", 1);
    g_assert_true (last_send_handle == FAKE_B);

    hx_bridge_uninstall (a);
    hx_bridge_uninstall (b);
    hx_conn_free (a);
    hx_conn_free (b);
}

/* Still refused: a second install over the *same* connection, which would
 * orphan the first actor and its socket with nothing left pointing at them. */
static void
test_install_refuses_over_the_same_connection (void)
{
    struct htlc_conn *a = hx_conn_new ();

    reset_stub_state ();
    g_assert_true (install_plaintext (a));

    g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL,
                           "*already has a transport*");
    g_assert_false (install_plaintext (a));
    g_test_assert_expected_messages ();
    /* Refused before reaching hxnet, so the first actor is untouched. */
    g_assert_cmpint (open_calls, ==, 1);
    g_assert_true (hx_bridge_is_installed (a));

    hx_bridge_uninstall (a);
    hx_conn_free (a);
}

/* ---- the stale-actor guards ------------------------------------------- *
 *
 * Both guards compare the handle an event carries against the one stored on
 * the connection. They were written for reconnect — an event queued by the
 * old actor arriving after a new handle is installed — and the per-connection
 * move is what lets them tell that case apart from a *second live
 * connection*, which under the old module-wide slot was indistinguishable. */

static struct hxnet_frame_t *
make_frame (void)
{
    struct hxnet_frame_t *f = g_new0 (struct hxnet_frame_t, 1);
    f->type_ = 105;
    return f;
}

static void
test_event_from_a_stale_actor_is_dropped (void)
{
    struct htlc_conn *a = hx_conn_new ();

    reset_stub_state ();
    open_result = FAKE_A;
    g_assert_true (install_plaintext (a));
    test_stub_event_cb on_event = last_open_event_cb;
    g_assert_nonnull (on_event);
    g_assert_true (last_open_user_data == a);

    /* The live handle dispatches. */
    on_event (FAKE_A, make_frame (), a);
    g_assert_cmpint (dispatch_calls, ==, 1);
    g_assert_true (last_dispatch_htlc == a);
    g_assert_cmpuint (last_dispatch_type, ==, 105);

    /* A handle this connection no longer has does not — this is the frame
     * that would otherwise be injected into a reconnected session's state
     * machine. */
    on_event (FAKE_B, make_frame (), a);
    g_assert_cmpint (dispatch_calls, ==, 1);

    /* Dropped or not, every frame is freed: the ownership contract does not
     * bend for stale events. */
    g_assert_cmpint (frame_free_calls, ==, 2);

    hx_bridge_uninstall (a);
    hx_conn_free (a);
}

/* The guard is per-connection, so B's live events are not mistaken for A's
 * stale ones. Under the old single slot this was the frame that got dropped:
 * whichever connection installed last owned the slot, and every other
 * connection's traffic failed the identity test. */
static void
test_events_route_to_their_own_connection (void)
{
    struct htlc_conn *a = hx_conn_new ();
    struct htlc_conn *b = hx_conn_new ();

    reset_stub_state ();
    open_result = FAKE_A;
    g_assert_true (install_plaintext (a));
    test_stub_event_cb on_event = last_open_event_cb;
    open_result = FAKE_B;
    g_assert_true (install_plaintext (b));

    on_event (FAKE_A, make_frame (), a);
    g_assert_cmpint (dispatch_calls, ==, 1);
    g_assert_true (last_dispatch_htlc == a);

    on_event (FAKE_B, make_frame (), b);
    g_assert_cmpint (dispatch_calls, ==, 2);
    g_assert_true (last_dispatch_htlc == b);

    /* Crossed pairs are still rejected — the identity test is (handle,
     * connection), not either one alone. */
    on_event (FAKE_A, make_frame (), b);
    on_event (FAKE_B, make_frame (), a);
    g_assert_cmpint (dispatch_calls, ==, 2);
    g_assert_cmpint (frame_free_calls, ==, 4);

    hx_bridge_uninstall (a);
    hx_bridge_uninstall (b);
    hx_conn_free (a);
    hx_conn_free (b);
}

/* A shutdown from an actor this connection has already replaced must not tear
 * down the live one. This is the guard in the more dangerous direction: the
 * old code's failure mode was destroying the *new* handle. */
static void
test_shutdown_from_a_stale_actor_is_ignored (void)
{
    struct htlc_conn *a = hx_conn_new ();

    reset_stub_state ();
    open_result = FAKE_A;
    g_assert_true (install_plaintext (a));
    test_stub_shutdown_cb on_shutdown = last_open_shutdown_cb;
    g_assert_nonnull (on_shutdown);

    on_shutdown (FAKE_B, 0 /* EOF */, a);
    g_assert_cmpint (destroy_calls, ==, 0);
    g_assert_true (hx_bridge_is_installed (a));

    hx_bridge_uninstall (a);
    hx_conn_free (a);
}

static void
test_send_routes_to_its_own_connection (void)
{
    struct htlc_conn *a = hx_conn_new ();
    struct htlc_conn *b = hx_conn_new ();
    const guint8 payload[] = { 0xde, 0xad, 0xbe, 0xef };

    hx_conn_set_bridge_handle (a, FAKE_A);
    hx_conn_set_bridge_handle (b, FAKE_B);

    last_send_handle = NULL;
    g_assert_cmpint (hx_bridge_send_frame (a, payload, sizeof (payload)), ==,
                     0);
    g_assert_true (last_send_handle == FAKE_A);
    g_assert_cmpuint (last_send_len, ==, sizeof (payload));

    /* The bug this replaced: with one slot, this went to whichever
     * connection installed last regardless of which one was asked. */
    last_send_handle = NULL;
    g_assert_cmpint (hx_bridge_send_frame (b, payload, sizeof (payload)), ==,
                     0);
    g_assert_true (last_send_handle == FAKE_B);

    hx_conn_set_bridge_handle (a, NULL);
    hx_conn_set_bridge_handle (b, NULL);
    hx_conn_free (a);
    hx_conn_free (b);
}

static void
test_send_without_a_transport (void)
{
    struct htlc_conn *a = hx_conn_new ();
    const guint8 payload[] = { 0x01 };

    last_send_handle = NULL;
    /* g_critical from the bridge is expected here — it is how the caller
     * learns a send raced teardown. */
    g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL,
                           "*no installed*");
    g_assert_cmpint (hx_bridge_send_frame (a, payload, sizeof (payload)), ==,
                     HX_BRIDGE_SEND_NOT_INSTALLED);
    g_test_assert_expected_messages ();
    g_assert_null (last_send_handle);

    hx_conn_free (a);
}

static void
test_uninstall_leaves_other_connections_alone (void)
{
    struct htlc_conn *a = hx_conn_new ();
    struct htlc_conn *b = hx_conn_new ();

    hx_conn_set_bridge_handle (a, FAKE_A);
    hx_conn_set_bridge_handle (b, FAKE_B);

    destroy_calls = 0;
    last_destroyed_handle = NULL;
    hx_bridge_uninstall (b);
    g_assert_cmpint (destroy_calls, ==, 1);
    g_assert_true (last_destroyed_handle == FAKE_B);
    g_assert_false (hx_bridge_is_installed (b));
    /* a still has its transport — disconnecting one connection must not
     * tear down another. */
    g_assert_true (hx_bridge_is_installed (a));

    /* Idempotent: a second uninstall of the same connection is a no-op
     * rather than a double free. */
    hx_bridge_uninstall (b);
    g_assert_cmpint (destroy_calls, ==, 1);

    hx_bridge_uninstall (a);
    g_assert_cmpint (destroy_calls, ==, 2);
    g_assert_true (last_destroyed_handle == FAKE_A);
    g_assert_false (hx_bridge_is_installed (a));

    hx_conn_free (a);
    hx_conn_free (b);
}

int
main (int argc, char *argv[])
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/hxnet_bridge/pack_header/zero_body",
                     test_pack_header_zero_body);
    g_test_add_func ("/hxnet_bridge/pack_header/typical_chat",
                     test_pack_header_typical_chat);
    g_test_add_func ("/hxnet_bridge/pack_header/task_error_flag",
                     test_pack_header_task_error_flag);
    g_test_add_func ("/hxnet_bridge/pack_header/large_body",
                     test_pack_header_large_body);
    g_test_add_func ("/hxnet_bridge/pack_header/full_u32_fields",
                     test_pack_header_full_u32_fields);
    g_test_add_func ("/hxnet_bridge/pack_header/byte_layout",
                     test_pack_header_byte_layout);
    g_test_add_func ("/hxnet_bridge/handle/installed_is_per_connection",
                     test_installed_is_per_connection);
    g_test_add_func ("/hxnet_bridge/handle/installed_null_connection",
                     test_installed_null_connection);
    g_test_add_func ("/hxnet_bridge/handle/send_routes_to_its_own_connection",
                     test_send_routes_to_its_own_connection);
    g_test_add_func ("/hxnet_bridge/handle/send_without_a_transport",
                     test_send_without_a_transport);
    g_test_add_func ("/hxnet_bridge/handle/uninstall_leaves_others_alone",
                     test_uninstall_leaves_other_connections_alone);
    g_test_add_func ("/hxnet_bridge/install/allows_a_second_connection",
                     test_install_allows_a_second_connection);
    g_test_add_func ("/hxnet_bridge/install/refuses_over_the_same_connection",
                     test_install_refuses_over_the_same_connection);
    g_test_add_func ("/hxnet_bridge/guard/event_from_a_stale_actor",
                     test_event_from_a_stale_actor_is_dropped);
    g_test_add_func ("/hxnet_bridge/guard/events_route_to_their_own_connection",
                     test_events_route_to_their_own_connection);
    g_test_add_func ("/hxnet_bridge/guard/shutdown_from_a_stale_actor",
                     test_shutdown_from_a_stale_actor_is_ignored);
    return g_test_run ();
}
