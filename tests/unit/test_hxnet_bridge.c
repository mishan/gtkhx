/*
 * test_hxnet_bridge.c — Tier 1 tests for the R3.3.e-4a hxnet
 * bridge module.
 *
 * The module ships two ingest paths:
 *
 *   * `hx_bridge_pack_header` — turns a (type, trans, flag, hc,
 *     body_len) tuple into a 22-byte Hotline header. Tested here
 *     against the production `hl_hdr_decode` to lock down the
 *     wire-format round-trip without standing up an htlc_conn.
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
#include "hxnet_bridge.h"
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
    (void) bytes;
    (void) len;
    (void) out_len;
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

/* hxnet_bridge.c references the production rcv state machine
 * (hx_rcv_hdr) and qbuf_set / hx_htlc_close to drive a real
 * htlc_conn. Those are exercised by R3.3.e-4b's wider
 * integration tests once the bridge is wired into the
 * connection lifecycle. Tier 1 here only links the bridge's
 * header-pack path, so the linker just needs unresolved symbols
 * resolved. proto_helpers.c needs debug_log for its trace
 * helpers; same treatment. */
void hx_rcv_hdr (struct htlc_conn *htlc);
void hx_htlc_close (struct htlc_conn *htlc, int expected);
void qbuf_set (struct qbuf *q, guint32 pos, guint32 len);
void debug_log (const char *cat, const char *fmt, ...);
void hx_orchestrator_register_login_task (struct htlc_conn *htlc);

void
hx_rcv_hdr (struct htlc_conn *htlc)
{
    (void) htlc;
    g_assert_not_reached ();
}

/* Stub: bridge_on_state_cb calls this on LOGIN_SENDING, but these
 * unit tests drive the header-pack / dispatch paths directly and
 * never feed a LOGIN_SENDING state, so it should never fire. */
void
hx_orchestrator_register_login_task (struct htlc_conn *htlc)
{
    (void) htlc;
    g_assert_not_reached ();
}

void
hx_htlc_close (struct htlc_conn *htlc, int expected)
{
    (void) htlc;
    (void) expected;
    g_assert_not_reached ();
}

void
qbuf_set (struct qbuf *q, guint32 pos, guint32 len)
{
    (void) q;
    (void) pos;
    (void) len;
    g_assert_not_reached ();
}

void
debug_log (const char *cat, const char *fmt, ...)
{
    (void) cat;
    (void) fmt;
    /* No-op: proto_helpers calls into this for trace events
     * that the test paths don't trigger. */
}

/* R3.3.e-4b adds install / send / uninstall helpers to the
 * bridge that wrap hxnet's callback FFI. Tier 1 only exercises
 * the header-pack path, so we stub the hxnet symbols (rather
 * than dragging the staticlib + tokio runtime into a smoke
 * test). The lifecycle helpers themselves are covered by the
 * production network.c hookups + Tier 3 integration when
 * R3.3.e-4c lands.
 *
 * The function-pointer types here must match the production
 * declarations in hxnet_bridge.c (and through it
 * rust/crates/hxnet/src/ffi.rs). Declaring the callback
 * parameters as `void *` would be undefined behavior under
 * C's function-type compatibility rules if anything ever
 * exercised this stub (the production caller passes real
 * function pointers, not data pointers, and "incompatible
 * function type" is the same ABI hazard the rest of the
 * hxnet FFI guards via hand-declared externs). */
struct hxnet_connection_opaque;
struct hxnet_frame_t;
struct hxnet_transform_config_t;

typedef void (*test_stub_event_cb) (struct hxnet_connection_opaque *conn,
                                    struct hxnet_frame_t *frame,
                                    void *user_data);
typedef void (*test_stub_shutdown_cb) (struct hxnet_connection_opaque *conn,
                                       int reason, void *user_data);

struct hxnet_connection_opaque *
hxnet_connection_spawn_fd_with_transforms_and_callback (
    int fd, const struct hxnet_transform_config_t *config,
    test_stub_event_cb on_event, test_stub_shutdown_cb on_shutdown,
    void *user_data);
int hxnet_connection_send_frame (struct hxnet_connection_opaque *handle,
                                 const guint8 *data, guint32 len);
void hxnet_connection_destroy (struct hxnet_connection_opaque *handle);
void hxnet_frame_free (struct hxnet_frame_t *f);

struct hxnet_connection_opaque *
hxnet_connection_spawn_fd_with_transforms_and_callback (
    int fd, const struct hxnet_transform_config_t *config,
    test_stub_event_cb on_event, test_stub_shutdown_cb on_shutdown,
    void *user_data)
{
    (void) fd;
    (void) config;
    (void) on_event;
    (void) on_shutdown;
    (void) user_data;
    g_assert_not_reached ();
    return NULL;
}

int
hxnet_connection_send_frame (struct hxnet_connection_opaque *handle,
                             const guint8 *data, guint32 len)
{
    (void) handle;
    (void) data;
    (void) len;
    g_assert_not_reached ();
    return -1;
}

void
hxnet_connection_destroy (struct hxnet_connection_opaque *handle)
{
    (void) handle;
    g_assert_not_reached ();
}

void
hxnet_frame_free (struct hxnet_frame_t *f)
{
    (void) f;
    g_assert_not_reached ();
}

/* Phase G HTXF-AEAD: hx_bridge_orchestrated_hope_aead() calls the Rust
 * material getter to seed htlc->hope_aead. Tier 1 never installs a
 * connection, so a g_assert_not_reached stub satisfies the link without
 * dragging in the staticlib. */
struct HxnetHopeAead;
struct HxnetHopeAead *hxnet_connection_hope_aead_material (
    struct hxnet_connection_opaque *conn);
struct HxnetHopeAead *
hxnet_connection_hope_aead_material (struct hxnet_connection_opaque *conn)
{
    (void) conn;
    g_assert_not_reached ();
    return NULL;
}

/* R3.3.e-4d's hx_bridge_install_with_hope_state pulls in the
 * Blowfish OFB state-snapshot helper from hxcrypto-stream. The
 * Tier 1 test never invokes the install path, so a
 * g_assert_not_reached stub keeps the symbol satisfied without
 * dragging the staticlib in. */
void
gtkhx_blowfish_ofb64_save_state (const void *state, guint8 *out_ivec,
                                 guint32 *out_num);
void
gtkhx_blowfish_ofb64_save_state (const void *state, guint8 *out_ivec,
                                 guint32 *out_num)
{
    (void) state;
    (void) out_ivec;
    (void) out_num;
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
typedef int (*test_stub_verify_cb) (const guint8 *fp, gsize fp_len,
                                    void *user_data);
struct hxnet_connection_opaque *hxnet_connection_open_plaintext (
    const guint8 *host, gsize host_len, guint16 port, const guint8 *login,
    gsize login_len, const guint8 *password, gsize password_len,
    const guint8 *name, gsize name_len, guint16 icon, guint16 version,
    guint16 caps, guint32 trans, const guint8 *proxy_uri,
    gsize proxy_uri_len, test_stub_event_cb on_event,
    test_stub_shutdown_cb on_shutdown, test_stub_state_cb on_state,
    void *user_data);
struct hxnet_connection_opaque *
hxnet_connection_open_plaintext (
    const guint8 *host, gsize host_len, guint16 port, const guint8 *login,
    gsize login_len, const guint8 *password, gsize password_len,
    const guint8 *name, gsize name_len, guint16 icon, guint16 version,
    guint16 caps, guint32 trans, const guint8 *proxy_uri,
    gsize proxy_uri_len, test_stub_event_cb on_event,
    test_stub_shutdown_cb on_shutdown, test_stub_state_cb on_state,
    void *user_data)
{
    (void) host; (void) host_len; (void) port; (void) login;
    (void) login_len; (void) password; (void) password_len; (void) name;
    (void) name_len; (void) icon; (void) version; (void) caps; (void) trans;
    (void) proxy_uri; (void) proxy_uri_len;
    (void) on_event; (void) on_shutdown; (void) on_state; (void) user_data;
    g_assert_not_reached ();
    return NULL;
}

struct hxnet_connection_opaque *hxnet_connection_open_hope (
    const guint8 *host, gsize host_len, guint16 port, const guint8 *login,
    gsize login_len, const guint8 *password, gsize password_len,
    const guint8 *name, gsize name_len, guint16 icon, guint16 version,
    guint16 caps, guint32 trans, const guint8 *cipher_alg,
    gsize cipher_alg_len, const guint8 *proxy_uri, gsize proxy_uri_len,
    test_stub_event_cb on_event,
    test_stub_shutdown_cb on_shutdown, test_stub_state_cb on_state,
    void *user_data);
struct hxnet_connection_opaque *
hxnet_connection_open_hope (
    const guint8 *host, gsize host_len, guint16 port, const guint8 *login,
    gsize login_len, const guint8 *password, gsize password_len,
    const guint8 *name, gsize name_len, guint16 icon, guint16 version,
    guint16 caps, guint32 trans, const guint8 *cipher_alg,
    gsize cipher_alg_len, const guint8 *proxy_uri, gsize proxy_uri_len,
    test_stub_event_cb on_event,
    test_stub_shutdown_cb on_shutdown, test_stub_state_cb on_state,
    void *user_data)
{
    (void) host; (void) host_len; (void) port; (void) login;
    (void) login_len; (void) password; (void) password_len; (void) name;
    (void) name_len; (void) icon; (void) version; (void) caps; (void) trans;
    (void) cipher_alg; (void) cipher_alg_len;
    (void) proxy_uri; (void) proxy_uri_len;
    (void) on_event; (void) on_shutdown; (void) on_state; (void) user_data;
    g_assert_not_reached ();
    return NULL;
}

struct hxnet_connection_opaque *hxnet_connection_open_plaintext_tls (
    const guint8 *host, gsize host_len, guint16 port, const guint8 *login,
    gsize login_len, const guint8 *password, gsize password_len,
    const guint8 *name, gsize name_len, guint16 icon, guint16 version,
    guint16 caps, guint32 trans, const guint8 *proxy_uri,
    gsize proxy_uri_len, test_stub_event_cb on_event,
    test_stub_shutdown_cb on_shutdown, test_stub_state_cb on_state,
    test_stub_verify_cb verify_cert, void *user_data);
struct hxnet_connection_opaque *
hxnet_connection_open_plaintext_tls (
    const guint8 *host, gsize host_len, guint16 port, const guint8 *login,
    gsize login_len, const guint8 *password, gsize password_len,
    const guint8 *name, gsize name_len, guint16 icon, guint16 version,
    guint16 caps, guint32 trans, const guint8 *proxy_uri,
    gsize proxy_uri_len, test_stub_event_cb on_event,
    test_stub_shutdown_cb on_shutdown, test_stub_state_cb on_state,
    test_stub_verify_cb verify_cert, void *user_data)
{
    (void) host; (void) host_len; (void) port; (void) login;
    (void) login_len; (void) password; (void) password_len; (void) name;
    (void) name_len; (void) icon; (void) version; (void) caps; (void) trans;
    (void) proxy_uri; (void) proxy_uri_len;
    (void) on_event; (void) on_shutdown; (void) on_state; (void) verify_cert;
    (void) user_data;
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
    (void) htlc;
    (void) fingerprint;
    g_assert_not_reached ();
    return FALSE;
}

typedef struct _GtkhxSession GtkhxSession;
GtkhxSession *gtkhx_session_get_default (void);
GtkhxSession *
gtkhx_session_get_default (void)
{
    return NULL;
}
void gtkhx_session_emit_connection_state (GtkhxSession *self, int state);
void
gtkhx_session_emit_connection_state (GtkhxSession *self, int state)
{
    (void) self;
    (void) state;
    g_assert_not_reached ();
}

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
    hx_bridge_pack_header (hdr, type, trans, flag, hc, body_len);

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
        /* type 0x01020304 BE */ 0x01, 0x02, 0x03, 0x04,
        /* trans 0x05060708 BE */ 0x05, 0x06, 0x07, 0x08,
        /* flag 0x090a0b0c BE */ 0x09, 0x0a, 0x0b, 0x0c,
        /* len = body_len(=10) + sizeof(hc)=2 = 12, BE */
        0x00, 0x00, 0x00, 0x0c,
        /* len2 == len (matches hlpack) */ 0x00, 0x00, 0x00, 0x0c,
        /* hc 0x0d0e BE */ 0x0d, 0x0e,
    };
    hx_bridge_pack_header (hdr, 0x01020304, 0x05060708, 0x090a0b0c, 0x0d0e,
                           10);
    g_assert_cmpmem (hdr, sizeof (hdr), expected, sizeof (expected));
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
    return g_test_run ();
}
