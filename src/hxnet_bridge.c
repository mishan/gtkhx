/*
 * hxnet_bridge.c — translation layer between hxnet's callback FFI
 * and gtkhx's existing rcv state machine. See hxnet_bridge.h for
 * the module-level rationale.
 *
 * The functions here run on the GLib main thread (the hxnet
 * forwarder spawns_local onto the thread-default MainContext,
 * which in production is the main loop). No threading
 * primitives needed — the htlc_conn struct is touched
 * single-threaded throughout the lifetime of one connection.
 */

#include "config.h"

#include <string.h>
#include <unistd.h>             /* close() — fd cleanup on pre-spawn failure */

#include <glib.h>

#include "compat.h"             /* MAX_HOTLINE_PACKET_LEN */
#include "hxnet_bridge.h"
#include "protocol.h"
#include "proto_helpers.h"
#include "gtkhx_session.h"      /* GtkhxConnectionState + emit (Phase G state cb) */
#include "network.h"            /* hx_orchestrator_register_login_task (LOGIN_SENDING) */

/* Forward declaration of the production header decoder
 * (proto_helpers.c). hx_rcv_hdr decodes the buffered header by
 * calling this; we don't need to call it directly here. */
extern void hx_rcv_hdr (struct htlc_conn *htlc);

/* Forward declaration of the production teardown. Defined in
 * network.c; we trampoline to it from the shutdown bridge.
 * Marked `expected` semantically by the reason translation
 * inside hx_bridge_dispatch_shutdown. */
extern void hx_htlc_close (struct htlc_conn *htlc, int expected);

/* network.c's "session is fully logged in" flag. Used here only to
 * pick the shutdown log level: a failure before the session came up
 * (connect refused / handshake / login reject) is routine and gets a
 * user-facing error dialog, so it logs at g_message; a mid-session
 * drop after login is the noteworthy case. */
extern int connected;

/* Mirror of the hxnet FFI's shutdown-reason constants
 * (rust/crates/hxnet/src/ffi.rs::HXNET_SHUTDOWN_*). The C side
 * has its own copies in tests/unit/test_hxnet_ffi.c; redeclaring
 * them here as bridge-internal constants avoids that header
 * having to ship hxnet_frame's full ABI just for the reason
 * codes. Production's on_shutdown callback will pass these in
 * verbatim. */
#define HXNET_SHUTDOWN_EOF             0
#define HXNET_SHUTDOWN_STREAM_ERROR    1
#define HXNET_SHUTDOWN_FRAME_TOO_LARGE 2
#define HXNET_SHUTDOWN_HANDLE_DROPPED  3

void
hx_bridge_pack_header (guint8 *dst, guint32 type, guint32 trans, guint32 flag,
                       guint16 hc, guint32 body_len)
{
    /* Layout matches struct hl_hdr in src/hotline.h:
     *   type(4) + trans(4) + flag(4) + len(4) + len2(4) + hc(2)
     * = 22 bytes. All multi-byte fields are wire big-endian.
     *
     * wire `len` encodes "body bytes + sizeof(hc)" — the
     * Hotline protocol's quirk where hc counts as data section
     * even though it lives at the tail of the 22-byte header.
     * hl_hdr_decode (proto_helpers.c) reverses this by
     * subtracting sizeof(hc) to derive its body_len_out, so we
     * have to add it here. */
    const guint32 wire_len = body_len + (guint32) sizeof (guint16);

    /* Encode big-endian without depending on ntohl/htonl — same
     * byte-ordering discipline hlpack uses internally. */
    dst[0]  = (guint8) ((type  >> 24) & 0xff);
    dst[1]  = (guint8) ((type  >> 16) & 0xff);
    dst[2]  = (guint8) ((type  >>  8) & 0xff);
    dst[3]  = (guint8) ( type         & 0xff);
    dst[4]  = (guint8) ((trans >> 24) & 0xff);
    dst[5]  = (guint8) ((trans >> 16) & 0xff);
    dst[6]  = (guint8) ((trans >>  8) & 0xff);
    dst[7]  = (guint8) ( trans        & 0xff);
    dst[8]  = (guint8) ((flag  >> 24) & 0xff);
    dst[9]  = (guint8) ((flag  >> 16) & 0xff);
    dst[10] = (guint8) ((flag  >>  8) & 0xff);
    dst[11] = (guint8) ( flag         & 0xff);
    dst[12] = (guint8) ((wire_len >> 24) & 0xff);
    dst[13] = (guint8) ((wire_len >> 16) & 0xff);
    dst[14] = (guint8) ((wire_len >>  8) & 0xff);
    dst[15] = (guint8) ( wire_len        & 0xff);
    /* len2: a second copy of the wire length. hlpack/hlwrite set
     * len2 == len (proto_helpers.c: h.len = h.len2 = ...); match that
     * so a bridge-packed header is byte-identical to an hlpack one and
     * can't drift if a decoder ever starts validating len2. */
    dst[16] = (guint8) ((wire_len >> 24) & 0xff);
    dst[17] = (guint8) ((wire_len >> 16) & 0xff);
    dst[18] = (guint8) ((wire_len >>  8) & 0xff);
    dst[19] = (guint8) ( wire_len        & 0xff);
    /* hc (host chunk count) is u16 BE at offset 20-21. */
    dst[20] = (guint8) ((hc >> 8) & 0xff);
    dst[21] = (guint8) ( hc       & 0xff);
}

void
hx_bridge_dispatch_frame (struct htlc_conn *htlc, guint32 type, guint32 trans,
                          guint32 flag, guint16 hc, const guint8 *body,
                          guint32 body_len)
{
    g_return_if_fail (htlc != NULL);
    g_return_if_fail (body_len == 0 || body != NULL);

    /* Drop in-flight events that the GLib idle queue dispatched
     * after `hx_htlc_close` already ran. There's an inherent
     * race: hxnet's tokio actor forwards `Event::Frame`s through
     * `MainForwarder::forward_to_main` onto the GLib idle queue,
     * but a synchronous failure on the C-side send path (e.g.
     * `hx_bridge_send_frame` returning HXNET_SEND_CLOSED after
     * the actor exited) calls `hx_htlc_close` immediately —
     * which clears `htlc->fd`, frees `htlc->in.buf`, and
     * uninstalls the bridge. Any already-queued idle source
     * then fires AFTER close, calls us here, and we'd crash in
     * `hx_bridge_pack_header` writing through the freed buffer.
     * Skip the dispatch when either signal of close is set;
     * the in-flight frame is information the C side no longer
     * cares about. */
    if (htlc->fd == 0 || !hx_bridge_is_installed ()) {
        return;
    }

    /* `hx_rcv_hdr` calls `hl_hdr_decode` which clamps the wire
     * `len` field (= body_len + sizeof(hc)) to
     * `MAX_HOTLINE_PACKET_LEN`. The body qbuf inside hx_rcv_hdr
     * therefore allocates at most `MAX_HOTLINE_PACKET_LEN - 2`
     * bytes of body space. If we accepted a larger body_len
     * here, the memcpy below would write past the end of
     * htlc->in.buf. hxnet's actor enforces its own
     * MAX_BODY_LEN (1 MiB = MAX_HOTLINE_PACKET_LEN) so any real
     * server traffic that reaches us is in range — but the
     * bridge shouldn't depend on that coincidence. Refuse
     * oversize frames and tear the connection down loudly
     * rather than silently truncating or overflowing. */
    if (body_len > MAX_HOTLINE_PACKET_LEN - 2) {
        g_critical (
            "hxnet_bridge: dispatch_frame body_len %u exceeds "
            "MAX_HOTLINE_PACKET_LEN - sizeof(hc) %u; closing connection",
            (unsigned) body_len,
            (unsigned) (MAX_HOTLINE_PACKET_LEN - 2));
        hx_htlc_close (htlc, /*expected=*/0);
        return;
    }

    /* Stage the header into htlc->in. qbuf_set both grows the
     * underlying buffer if needed and sets pos+len in one call.
     * After the memcpy, advance pos to the header end and clear
     * len so hx_rcv_hdr sees a fully-received header (the
     * "bytes-remaining" semantic htlc->in carries). */
    qbuf_set (&htlc->in, 0, SIZEOF_HL_HDR);
    hx_bridge_pack_header (htlc->in.buf, type, trans, flag, hc, body_len);
    htlc->in.pos = SIZEOF_HL_HDR;
    htlc->in.len = 0;

    /* Initial state precondition for hx_rcv_hdr: htlc->rcv must
     * be hx_rcv_hdr itself (the read loop in network.c
     * maintains that invariant; we don't have that loop running
     * when we're driving the bridge directly). */
    htlc->rcv = hx_rcv_hdr;
    hx_rcv_hdr (htlc);

    /* hx_rcv_hdr's two possible exit states:
     *
     *   (a) Body present (body_len > 0). htlc->rcv is now the
     *       body handler; htlc->in has been resized via
     *       qbuf_set(&htlc->in, htlc->in.pos, body_len) — where
     *       htlc->in.pos == SIZEOF_HL_HDR — and htlc->in.len
     *       holds the count of body bytes still wanted. We fill
     *       those bytes and call the body handler.
     *
     *   (b) No body (body_len == 0). hx_rcv_hdr called the body
     *       handler with empty body and reset state — htlc->rcv
     *       is hx_rcv_hdr and htlc->in is sized for the next
     *       header. Nothing else for us to do.
     *
     * hx_htlc_close on the inner code path (e.g. a fatal task
     * error) would set htlc->fd to 0 — we check that as the
     * liveness gate so we don't keep dispatching on a torn-down
     * connection. */
    if (htlc->fd == 0 || htlc->rcv == NULL || htlc->rcv == hx_rcv_hdr) {
        return;
    }

    /* Body bytes go directly after the header in htlc->in.buf.
     * hx_rcv_hdr called qbuf_set(&htlc->in, htlc->in.pos, len)
     * which left htlc->in.pos at SIZEOF_HL_HDR (where the
     * caller had advanced it to after writing the header) and
     * set htlc->in.len to the body byte count we still owe.
     * The body handler reads from htlc->in.buf[SIZEOF_HL_HDR..]
     * once we stage the bytes there. */
    if (body_len > 0) {
        memcpy (&htlc->in.buf[SIZEOF_HL_HDR], body, body_len);
    }
    htlc->in.pos = SIZEOF_HL_HDR + body_len;
    htlc->in.len = 0;

    htlc->rcv (htlc);

    /* Reset for the next frame. Mirrors the goto-reset leg in
     * control_on_readable. */
    if (htlc->fd != 0) {
        htlc->rcv = hx_rcv_hdr;
        qbuf_set (&htlc->in, 0, SIZEOF_HL_HDR);
    }
}

void
hx_bridge_dispatch_shutdown (struct htlc_conn *htlc, int reason)
{
    g_return_if_fail (htlc != NULL);

    /* Always log the reason BEFORE the close-race early return
     * below. The race is: hxnet's actor errored out and posted
     * Event::Shutdown to the GLib idle queue, but a synchronous
     * `hx_bridge_send_frame` failure path on the C side already
     * fired `hx_htlc_close` first — by the time the queued
     * Event::Shutdown gets dispatched here, the bridge is
     * uninstalled and the early return below kicks in to avoid
     * a second teardown. The reason code is the only signal
     * we have for WHY the actor exited (StreamError /
     * FrameTooLarge / etc.), so log it regardless of whether
     * we're going to act on it. Without this, the visible
     * failure mode is just "hxnet_connection_send_frame returned
     * -2" with no clue what killed the actor. */
    const char *reason_str;
    switch (reason) {
    case HXNET_SHUTDOWN_EOF:             reason_str = "EOF (peer closed)"; break;
    case HXNET_SHUTDOWN_STREAM_ERROR:    reason_str = "STREAM_ERROR (mid-stream IO error)"; break;
    case HXNET_SHUTDOWN_FRAME_TOO_LARGE: reason_str = "FRAME_TOO_LARGE (oversized wire len)"; break;
    case HXNET_SHUTDOWN_HANDLE_DROPPED:  reason_str = "HANDLE_DROPPED (we dropped the handle)"; break;
    default:                             reason_str = "(unknown reason code)"; break;
    }
    /* Visibility / level. A clean shutdown (EOF, HANDLE_DROPPED) is
     * expected during normal disconnect. So is ANY failure before the
     * session is fully up (`connected == 0`): connect refused, DNS,
     * magic / HOPE / TLS handshake, or a rejected login all surface as
     * STREAM_ERROR with the actor never reaching frame mode. Those are
     * routine and already get a user-facing error dialog from
     * hx_htlc_close, so keep them at g_message rather than reading
     * like a bug (a routine "connection refused" shouldn't spam an
     * alarming warning to the console). Only a STREAM_ERROR /
     * FRAME_TOO_LARGE *after* a working login (connected == 1) is the
     * noteworthy mid-session-drop signal — promote that to g_warning. */
    gboolean noteworthy = connected != 0
                          && reason != HXNET_SHUTDOWN_EOF
                          && reason != HXNET_SHUTDOWN_HANDLE_DROPPED;
    if (noteworthy) {
        g_warning ("hxnet_bridge: actor exited mid-session with reason=%d "
                   "%s (htlc->fd=%d) — see stderr for the Rust-side "
                   "ShutdownReason (io::Error string for StreamError).",
                   reason, reason_str, htlc->fd);
    } else {
        g_message ("hxnet_bridge: actor exited with reason=%d %s "
                   "(htlc->fd=%d connected=%d)",
                   reason, reason_str, htlc->fd, connected);
    }

    /* Drop in-flight shutdowns that the GLib idle queue dispatched
     * after `hx_htlc_close` already ran (e.g. the synchronous
     * send-path failure path called close, which uninstalled the
     * bridge, but hxnet had already forwarded an `Event::Shutdown`
     * onto the idle queue). Re-entering `hx_htlc_close` would run all
     * of its teardown a second time.
     *
     * The re-entry guard is `htlc->fd == 0`: hx_htlc_close sets fd to
     * 0 as its last act, so a second shutdown for the same connection
     * sees fd==0 and bails. We deliberately do NOT also gate on
     * `!hx_bridge_is_installed()` — bridge_on_shutdown_cb dispatches
     * *before* it tears the handle down, precisely so this path can
     * call hx_htlc_close (which then uninstalls). Gating on
     * is_installed() here was the bug behind connect-refused (and any
     * server-initiated shutdown) leaving the UI stuck: the handle was
     * cleared first, so this early-returned and hx_htlc_close never
     * ran. fd is -1 on the orchestrator path (no C-visible fd) and the
     * real fd on the legacy path — both non-zero, so both proceed. */
    if (htlc->fd == 0) {
        return;
    }

    /* expected = 1 means "clean close" — hx_htlc_close
     * suppresses the error dialog. EOF + HandleDropped are
     * clean; any other reason (stream error, oversized frame,
     * unknown code) surfaces to the user. */
    int expected;
    switch (reason) {
    case HXNET_SHUTDOWN_EOF:
    case HXNET_SHUTDOWN_HANDLE_DROPPED:
        expected = 1;
        break;
    case HXNET_SHUTDOWN_STREAM_ERROR:
    case HXNET_SHUTDOWN_FRAME_TOO_LARGE:
    default:
        expected = 0;
        break;
    }
    hx_htlc_close (htlc, expected);
}

/* ----------------------------------------------------------------- *
 * Lifecycle helpers (R3.3.e-4b)                                     *
 * ----------------------------------------------------------------- */

/*
 * Forward declarations for the hxnet FFI surface we wrap.
 * Hand-written instead of cbindgen — drift surfaces as a
 * link-time undefined symbol, same discipline as the other
 * hxnet / hxbridge / hotline-proto FFI consumers.
 *
 * Mirror of rust/crates/hxnet/src/ffi.rs.
 */
typedef struct hxnet_connection_opaque hxnet_connection_opaque;

typedef struct {
    guint32 type_;
    guint32 trans;
    guint32 flag;
    guint16 hc;
    guint16 _pad;
    guint32 body_len;
    guint8 *body_ptr;
} hxnet_frame_t;

typedef void (*hxnet_event_cb_t) (hxnet_connection_opaque *conn,
                                  hxnet_frame_t *frame, void *user_data);
typedef void (*hxnet_shutdown_cb_t) (hxnet_connection_opaque *conn, int reason,
                                     void *user_data);
/* Phase G state callback — fires once per ConnectionState
 * transition. Mirror of HxnetStateCallback in
 * rust/crates/hxnet/src/ffi.rs. `state` is a HXNET_BRIDGE_STATE_*
 * discriminant. */
typedef void (*hxnet_state_cb_t) (hxnet_connection_opaque *conn, guint32 state,
                                  void *user_data);
/* Phase G TLS TOFU: invoked post-handshake with the peer leaf cert's
 * "sha256:<hex>" fingerprint (NOT NUL-terminated — use fp_len).
 * Returns non-zero to accept, zero to reject. Mirror of
 * HxnetVerifyCertCallback in rust/crates/hxnet/src/ffi.rs. */
typedef int (*hxnet_verify_cert_cb_t) (const guint8 *fp, gsize fp_len,
                                       void *user_data);

/* ConnectionState discriminants the Phase G state callback maps
 * onto GtkhxConnectionState. Mirror of HXNET_STATE_* in
 * rust/crates/hxnet/src/ffi.rs (only the two with a coarse
 * GtkhxConnectionState equivalent are named here; the
 * intermediate handshake states are dropped). */
#define HXNET_BRIDGE_STATE_CONNECTED       2
#define HXNET_BRIDGE_STATE_LOGIN_SENDING   5
#define HXNET_BRIDGE_STATE_HANDSHAKE_DONE 10

#define HXNET_BRIDGE_CIPHER_NONE              0
#define HXNET_BRIDGE_CIPHER_BLOWFISH          1
#define HXNET_BRIDGE_CIPHER_CHACHA20_POLY1305 2
/* R3.3.e-4g: Hotline-frame-aware Blowfish (HopeBlowfishStream).
 * Use this kind for any HOPE-Blowfish handshake; the bare
 * HXNET_BRIDGE_CIPHER_BLOWFISH kind doesn't carry the per-
 * message rekey-marker logic and breaks against servers that
 * trip the marker (e.g. VesperNet/Janus). */
#define HXNET_BRIDGE_CIPHER_HOPE_BLOWFISH     3

/* HMAC algorithm tags for HOPE Blowfish — match
 * HXNET_MACALG_* in rust/crates/hxnet/src/ffi.rs. */
#define HXNET_BRIDGE_MACALG_SHA256 0
#define HXNET_BRIDGE_MACALG_SHA1   1
#define HXNET_BRIDGE_MACALG_MD5    2

#define HXNET_BRIDGE_COMPRESSION_NONE 0
#define HXNET_BRIDGE_COMPRESSION_GZIP 1
#define HXNET_BRIDGE_COMPRESSION_LZ4  2
#define HXNET_BRIDGE_COMPRESSION_ZSTD 3

typedef struct {
    guint32 cipher_kind;
    guint32 compression_kind;
    /* Per-direction Blowfish keys. HOPE derives distinct read /
     * write keys from the session keystream — using a single key
     * for both directions desynchronises the read keystream from
     * anything the server sends. Caught against VesperNet/Janus,
     * where the keys clearly differ. */
    guint32 blowfish_read_key_len;
    guint32 blowfish_write_key_len;
    guint8  blowfish_read_key[56];
    guint8  blowfish_write_key[56];
    guint8  blowfish_read_ivec[8];
    guint8  blowfish_write_ivec[8];
    /* OFB byte-offset into the current keystream block (0..7).
     * Threaded through so the bridge install can fire mid-block
     * without desynchronising the Rust BlowfishStream from the
     * server's keystream. */
    guint8  blowfish_read_num;
    guint8  blowfish_write_num;
    /* HOPE-Blowfish per-message rekey inputs (only consulted
     * when cipher_kind == HXNET_BRIDGE_CIPHER_HOPE_BLOWFISH).
     * `hope_macalg` is one of HXNET_BRIDGE_MACALG_*; the
     * session key is the HOPE-Step-1 SESSIONKEY chunk. */
    guint8  hope_macalg;
    guint8  _pad_macalg;
    guint32 hope_session_key_len;
    guint8  hope_session_key[64];
    guint8  aead_read_key[32];
    guint8  aead_write_key[32];
    guint64 aead_read_counter;
    guint64 aead_write_counter;
    guint8  aead_read_dir;
    guint8  aead_write_dir;
    guint8  _pad[6];
} hxnet_transform_config_t;

/* ABI pin — Rust's HxnetTransformConfig has matching size +
 * alignment const-asserts in rust/crates/hxnet/src/ffi.rs.
 * tests/unit/test_hxnet_ffi.c carries the full per-field
 * offset checks; here we just guard size + alignment so a
 * layout regression fails at production build time even when
 * the test suite isn't built. */
_Static_assert (sizeof (hxnet_transform_config_t) == 304,
                "hxnet_transform_config_t size drift — sync Rust "
                "rust/crates/hxnet/src/ffi.rs const-asserts");
_Static_assert (_Alignof (hxnet_transform_config_t) == 8,
                "hxnet_transform_config_t alignment drift — sync "
                "Rust rust/crates/hxnet/src/ffi.rs const-asserts");

extern hxnet_connection_opaque *
hxnet_connection_spawn_fd_with_transforms_and_callback (
    int fd, const hxnet_transform_config_t *config,
    hxnet_event_cb_t on_event, hxnet_shutdown_cb_t on_shutdown,
    void *user_data);

extern int hxnet_connection_send_frame (hxnet_connection_opaque *handle,
                                        const guint8 *data, guint32 len);
extern void hxnet_connection_destroy (hxnet_connection_opaque *handle);
extern void hxnet_frame_free (hxnet_frame_t *f);

/* Phase G: hxnet drives the whole plaintext lifecycle (DNS + TCP +
 * magic + LOGIN + LOGIN-reply) and replays the reply as a synthetic
 * frame. Mirror of hxnet_connection_open_plaintext in
 * rust/crates/hxnet/src/ffi.rs. */
extern hxnet_connection_opaque *hxnet_connection_open_plaintext (
    const guint8 *host, gsize host_len, guint16 port,
    const guint8 *login, gsize login_len,
    const guint8 *password, gsize password_len,
    const guint8 *name, gsize name_len,
    guint16 icon, guint16 version, guint16 caps, guint32 trans,
    hxnet_event_cb_t on_event, hxnet_shutdown_cb_t on_shutdown,
    hxnet_state_cb_t on_state, void *user_data);

/* Phase G HOPE: hxnet drives the full HOPE-Secure-Login handshake
 * (magic + step1 + step2 + cipher transition) and the encrypted
 * post-login stream. Mirror of hxnet_connection_open_hope in
 * rust/crates/hxnet/src/ffi.rs. */
extern hxnet_connection_opaque *hxnet_connection_open_hope (
    const guint8 *host, gsize host_len, guint16 port,
    const guint8 *login, gsize login_len,
    const guint8 *password, gsize password_len,
    const guint8 *name, gsize name_len,
    guint16 icon, guint16 version, guint16 caps, guint32 trans,
    const guint8 *cipher_alg, gsize cipher_alg_len,
    hxnet_event_cb_t on_event, hxnet_shutdown_cb_t on_shutdown,
    hxnet_state_cb_t on_state, void *user_data);

/* Phase G TLS: plaintext Hotline over TLS-from-byte-zero (Mobius /
 * Janus separate-port model). Mirror of
 * hxnet_connection_open_plaintext_tls in rust/crates/hxnet/src/ffi.rs.
 * Cert trust is the verify_cert callback (TOFU, post-handshake). */
extern hxnet_connection_opaque *hxnet_connection_open_plaintext_tls (
    const guint8 *host, gsize host_len, guint16 port,
    const guint8 *login, gsize login_len,
    const guint8 *password, gsize password_len,
    const guint8 *name, gsize name_len,
    guint16 icon, guint16 version, guint16 caps, guint32 trans,
    hxnet_event_cb_t on_event, hxnet_shutdown_cb_t on_shutdown,
    hxnet_state_cb_t on_state, hxnet_verify_cert_cb_t verify_cert,
    void *user_data);

/* hx_tls_orchestrator_verify_cert (production TOFU verify, defined in
 * network.c) and hx_orchestrator_register_login_task are both declared
 * in network.h, included above. */

/*
 * Single-connection state. gtkhx is single-conn today (the
 * MAX_CONN > 1 scaffolding in hx.h is a lie — see CLAUDE.md);
 * a global suffices. R3.5+'s multi-conn UI will move this into
 * htlc_conn alongside the existing cipher / compress state.
 */
static hxnet_connection_opaque *bridge_handle;
/* Reserved for R3.5+'s multi-conn rework where the bridge globals
 * become a per-htlc field; today it's set on install / cleared on
 * teardown but never read elsewhere in this file. Marked
 * G_GNUC_UNUSED so single-conn builds don't emit a dead-store
 * warning under -Wunused-but-set-variable. Same fate as
 * hx.h's MAX_CONN scaffold — it lives until multi-conn does. */
static struct htlc_conn        *bridge_htlc G_GNUC_UNUSED;

/* C-side trampolines hxnet calls on the GLib main thread per
 * Event::Frame / Event::Shutdown. They translate the FFI
 * shapes into the bridge's existing dispatch helpers, free the
 * frame body (per the on_event ownership contract), and clear
 * the global on shutdown so the install gate flips off. */
static void
bridge_on_event_cb (hxnet_connection_opaque *conn,
                    hxnet_frame_t *frame, void *user_data)
{
    struct htlc_conn *htlc = user_data;
    /* Only the currently-installed handle may dispatch. After an
     * uninstall+reinstall cycle, an event the GLib forwarder had
     * already queued from the OLD actor could otherwise be injected
     * into the NEW connection's rcv state machine and corrupt it
     * (hx_bridge_is_installed() is true again, so a downstream gate
     * wouldn't catch it). `conn` is the same handle pointer install
     * stored in bridge_handle, so this comparison is exact. The
     * frame is still freed below regardless, honouring the ownership
     * contract even when the event is dropped as stale. */
    if (conn == bridge_handle && frame && htlc) {
        hx_bridge_dispatch_frame (htlc, frame->type_, frame->trans,
                                  frame->flag, frame->hc, frame->body_ptr,
                                  frame->body_len);
    }
    /* Per the hxnet callback contract, the C side owns the
     * body memory behind body_ptr/body_len for the lifetime of
     * this call. Freeing here keeps the lifetime tight and
     * matches the "process and free inside the callback"
     * pattern documented on HxnetEventCallback. */
    hxnet_frame_free (frame);
}

static void
bridge_on_shutdown_cb (hxnet_connection_opaque *conn, int reason,
                       void *user_data)
{
    struct htlc_conn *htlc = user_data;

    /* Ignore a shutdown delivered by a stale actor. If an old
     * actor's shutdown was queued on the main loop and only runs
     * after a reconnect installed a new handle, `conn` no longer
     * matches bridge_handle; proceeding would clear/destroy the NEW
     * handle and tear down the live connection. The stale actor was
     * already destroyed when it was uninstalled, so there's nothing
     * to free here — just return. */
    if (conn != bridge_handle) {
        return;
    }

    /* The actor has already exited (that's what got us into this
     * callback); we need to tear down the hxnet handle and the C-side
     * connection state. The handle teardown (dropping the cmd
     * channel, pump task, and forwarder) happens via
     * hx_bridge_uninstall, reached through hx_htlc_close below — so
     * the lifetime stays deterministic within this callback.
     *
     * Dispatch FIRST, while the handle is still installed. The actor
     * has already exited (that's what got us here), so
     * hx_bridge_dispatch_shutdown's only liveness gate is
     * `htlc->fd != 0`; when that holds it runs hx_htlc_close, which
     * emits DISCONNECTED, clears the pending tasks, and calls
     * hx_bridge_uninstall — and uninstall is what destroys + clears
     * bridge_handle. So after dispatch the handle is normally already
     * gone.
     *
     * (Earlier this cleared bridge_handle first and then dispatched,
     * which combined with dispatch_shutdown's old !is_installed()
     * guard to skip hx_htlc_close entirely — that left connect-refused
     * and other server-initiated shutdowns stuck with a spinning
     * throbber and dangling task rows.)
     *
     * Destroying the handle from inside hx_htlc_close runs within this
     * forwarder closure — the same depth the previous explicit destroy
     * ran at. */
    if (htlc) {
        hx_bridge_dispatch_shutdown (htlc, reason);
    }

    /* Defensive: if dispatch didn't tear the handle down (htlc was
     * NULL, or fd was already 0 from a prior close so dispatch bailed)
     * release the actor's resources here so they're freed exactly
     * once. Normal server-initiated shutdowns already cleared it via
     * hx_htlc_close → hx_bridge_uninstall above. */
    if (bridge_handle) {
        hxnet_connection_opaque *to_destroy = bridge_handle;
        bridge_handle = NULL;
        bridge_htlc = NULL;
        hxnet_connection_destroy (to_destroy);
    }
}

/* Phase G state trampoline. hxnet fires this on the GLib main
 * thread once per ConnectionState transition; we map the
 * fine-grained discriminants onto the coarse GtkhxConnectionState
 * the toolbar / chat windows already listen to and emit on the
 * default session. The legacy GIOStream connect path emits the same
 * coarse sequence (CONNECTING in hx_connect → TCP_CONNECTED in
 * on_async_connected → HANDSHAKE_DONE in send_login); the
 * orchestrator just sources the transitions from the Rust state
 * machine. Intermediate states (magic / login-sending /
 * login-reply-wait / HOPE) have no coarse equivalent and are
 * dropped — the throbber doesn't need that granularity, and
 * CONNECTING was already emitted in hx_connect_via_orchestrator. */
static void
bridge_on_state_cb (hxnet_connection_opaque *conn G_GNUC_UNUSED, guint32 state,
                    void *user_data)
{
    GtkhxSession *sess = gtkhx_session_get_default ();
    switch (state) {
    case HXNET_BRIDGE_STATE_CONNECTED:
        gtkhx_session_emit_connection_state (sess,
                                             GTKHX_CONNECTION_TCP_CONNECTED);
        break;
    case HXNET_BRIDGE_STATE_LOGIN_SENDING:
        /* Magic done, credentials about to go out — the orchestrator's
         * equivalent of the legacy path's send_login moment. Drive the
         * same two UI effects, in the same order, so the Tasks window
         * looks identical to legacy: emit HANDSHAKE_DONE (which deletes
         * the coarse "Connecting" task), then register the "login"
         * protocol task. The login task must exist before the replayed
         * LOGIN/step-2 reply frame arrives to dispatch to it; that
         * frame is emitted strictly after this state on the same
         * ordered event channel, so registering here is in time. */
        gtkhx_session_emit_connection_state (sess,
                                             GTKHX_CONNECTION_HANDSHAKE_DONE);
        hx_orchestrator_register_login_task ((struct htlc_conn *) user_data);
        break;
    case HXNET_BRIDGE_STATE_HANDSHAKE_DONE:
        /* Rust's end-of-handshake state. No view transition here: the
         * coarse HANDSHAKE_DONE already fired at LOGIN_SENDING above,
         * and login completion is signalled by LOGIN_READY, which
         * rcv_task_login emits when the replayed reply dispatches. */
        break;
    default:
        break;
    }
}

gboolean
hx_bridge_install_orchestrated_plaintext (struct htlc_conn *htlc,
                                          const char *host, guint16 port,
                                          const char *login, const char *pass,
                                          const char *name, guint16 icon,
                                          guint16 version, guint16 caps,
                                          guint32 trans)
{
    g_return_val_if_fail (htlc != NULL, FALSE);
    g_return_val_if_fail (host != NULL && *host, FALSE);

    if (bridge_handle) {
        g_critical ("hxnet_bridge: orchestrated install attempted while a "
                    "connection is already installed; refusing");
        return FALSE;
    }

    login = login ? login : "";
    pass  = pass  ? pass  : "";
    name  = name  ? name  : "";

    /* open_plaintext spawns the lifecycle task and wires the
     * forwarder synchronously; events don't fire until we return to
     * the GLib main loop. Storing bridge_handle before that return
     * is what makes the orchestrator's replayed LOGIN-reply frame
     * pass hx_bridge_dispatch_frame's hx_bridge_is_installed() gate.
     * user_data is the htlc for all three callbacks. */
    hxnet_connection_opaque *h = hxnet_connection_open_plaintext (
        (const guint8 *) host, strlen (host), port,
        (const guint8 *) login, strlen (login),
        (const guint8 *) pass, strlen (pass),
        (const guint8 *) name, strlen (name),
        icon, version, caps, trans,
        bridge_on_event_cb, bridge_on_shutdown_cb, bridge_on_state_cb, htlc);
    if (!h) {
        /* open_plaintext logs its own g_critical on the failure
         * paths (NULL/empty host, non-UTF-8 host, trans==0, runtime
         * panic). Leave the bridge uninstalled. */
        return FALSE;
    }
    bridge_handle = h;
    bridge_htlc   = htlc;
    return TRUE;
}

gboolean
hx_bridge_install_orchestrated_hope (struct htlc_conn *htlc,
                                     const char *host, guint16 port,
                                     const char *login, const char *pass,
                                     const char *name, guint16 icon,
                                     guint16 version, guint16 caps,
                                     guint32 trans, const char *cipher_alg)
{
    g_return_val_if_fail (htlc != NULL, FALSE);
    g_return_val_if_fail (host != NULL && *host, FALSE);
    /* cipher_alg may be NULL/empty: that selects HOPE secure-login with
     * no transport cipher (HMAC auth over plaintext — mhxd's
     * non-cipher_only mode). The FFI treats len==0 as "no cipher". */
    g_return_val_if_fail (cipher_alg != NULL, FALSE);

    if (bridge_handle) {
        g_critical ("hxnet_bridge: orchestrated HOPE install attempted while "
                    "a connection is already installed; refusing");
        return FALSE;
    }

    login = login ? login : "";
    pass  = pass  ? pass  : "";
    name  = name  ? name  : "";

    /* Same synchronous-install-before-return discipline as the
     * plaintext variant: the bridge handle must be live before the
     * forwarder can deliver the replayed step-2 reply. */
    hxnet_connection_opaque *h = hxnet_connection_open_hope (
        (const guint8 *) host, strlen (host), port,
        (const guint8 *) login, strlen (login),
        (const guint8 *) pass, strlen (pass),
        (const guint8 *) name, strlen (name),
        icon, version, caps, trans,
        (const guint8 *) cipher_alg, strlen (cipher_alg),
        bridge_on_event_cb, bridge_on_shutdown_cb, bridge_on_state_cb, htlc);
    if (!h) {
        return FALSE;
    }
    bridge_handle = h;
    bridge_htlc   = htlc;
    return TRUE;
}

/* TLS TOFU trampoline: hxnet calls this on the lifecycle task with the
 * peer cert fingerprint ONLY when WebPKI validation against the native
 * roots failed (a CA-valid cert is trusted silently and never reaches
 * here); route to the production verify, which keys on
 * htlc->serverhost/serverport. */
static int
bridge_on_verify_cert_cb (const guint8 *fp, gsize fp_len, void *user_data)
{
    struct htlc_conn *htlc = user_data;
    if (!htlc || !fp) {
        return 0; /* reject: no context / no fingerprint */
    }
    g_autofree char *fp_str = g_strndup ((const char *) fp, fp_len);
    return hx_tls_orchestrator_verify_cert (htlc, fp_str) ? 1 : 0;
}

gboolean
hx_bridge_install_orchestrated_plaintext_tls (struct htlc_conn *htlc,
                                              const char *host, guint16 port,
                                              const char *login,
                                              const char *pass,
                                              const char *name, guint16 icon,
                                              guint16 version, guint16 caps,
                                              guint32 trans)
{
    g_return_val_if_fail (htlc != NULL, FALSE);
    g_return_val_if_fail (host != NULL && *host, FALSE);

    if (bridge_handle) {
        g_critical ("hxnet_bridge: orchestrated TLS install attempted while a "
                    "connection is already installed; refusing");
        return FALSE;
    }

    login = login ? login : "";
    pass  = pass  ? pass  : "";
    name  = name  ? name  : "";

    hxnet_connection_opaque *h = hxnet_connection_open_plaintext_tls (
        (const guint8 *) host, strlen (host), port,
        (const guint8 *) login, strlen (login),
        (const guint8 *) pass, strlen (pass),
        (const guint8 *) name, strlen (name),
        icon, version, caps, trans,
        bridge_on_event_cb, bridge_on_shutdown_cb, bridge_on_state_cb,
        bridge_on_verify_cert_cb, htlc);
    if (!h) {
        return FALSE;
    }
    bridge_handle = h;
    bridge_htlc   = htlc;
    return TRUE;
}

gboolean
hx_bridge_install_passthrough (struct htlc_conn *htlc, int fd)
{
    g_return_val_if_fail (htlc != NULL, FALSE);
    g_return_val_if_fail (fd >= 0, FALSE);

    /* Ownership contract (same as hx_bridge_install_with_hope_state):
     * the caller hands us an fd it has already dup()'d; on a pre-spawn
     * failure we still own it and must close it, on success the spawned
     * actor owns it via TcpStream::from_raw_fd. */
    if (bridge_handle) {
        g_critical ("hxnet_bridge: install attempted while a connection is "
                    "already installed; refusing");
        goto fail_close_fd;
    }

    /* Passthrough config — cipher and compression both NONE.
     * The Rust side copies the struct, so it's safe to leave
     * everything zeroed beyond the kind tags. */
    hxnet_transform_config_t cfg;
    memset (&cfg, 0, sizeof (cfg));
    cfg.cipher_kind      = HXNET_BRIDGE_CIPHER_NONE;
    cfg.compression_kind = HXNET_BRIDGE_COMPRESSION_NONE;

    hxnet_connection_opaque *h
        = hxnet_connection_spawn_fd_with_transforms_and_callback (
            fd, &cfg, bridge_on_event_cb, bridge_on_shutdown_cb, htlc);
    if (!h) {
        /* The FFI returns NULL on two classes of failure: pre-adoption
         * validation (NULL config / callbacks, fd < 0, unknown
         * cipher/compression kind), which fires BEFORE
         * TcpStream::from_raw_fd takes the fd; and post-adoption errors
         * (set_nonblocking, from_std, compose, spawn_boxed), which fire
         * AFTER. We deliberately do NOT close(fd) here because this call
         * site pre-validates every pre-adoption condition — fd >= 0 (the
         * g_return_val_if_fail above), a non-NULL config with the fixed
         * NONE/NONE kinds, and non-NULL callbacks — so the only NULL
         * returns reachable from here are post-adoption, where the
         * dropped stream's Drop already closed the fd and a close() here
         * would be a double-close. If new pre-adoption validation is ever
         * added to the FFI, this contract (and this no-close path) must
         * be revisited. */
        return FALSE;
    }
    bridge_handle = h;
    bridge_htlc   = htlc;
    return TRUE;

fail_close_fd:
    /* Pre-spawn failure: the duped fd never reached the Rust actor,
     * so we still own it. Close to honour the install-on-success /
     * close-on-failure contract. */
    close (fd);
    return FALSE;
}

/* R3.3.e-4d HOPE-state install. Builds an
 * hxnet_transform_config_t from the negotiated cipher state
 * already living on htlc, then spawns the actor with that
 * stack. */
#include "cipher.h" /* CIPHER_*, chacha_aead_state */
#include "cipher_aead.h" /* CIPHER_AEAD_DIR_* tags */
#include "compress.h" /* COMPRESS_* */
#include "protocol.h" /* struct htlc_conn cipher_*_key fields */

/* Same Rust-side function as hxcrypto-stream's
 * gtkhx_blowfish_ofb64_save_state. Lets us read the live OFB
 * ivec without copying the whole BlowfishOfb64State. */
extern void
gtkhx_blowfish_ofb64_save_state (const void *state, guint8 *out_ivec,
                                 guint32 *out_num);

gboolean
hx_bridge_install_with_hope_state (struct htlc_conn *htlc, int fd)
{
    g_return_val_if_fail (htlc != NULL, FALSE);
    g_return_val_if_fail (fd >= 0, FALSE);

    /* Ownership contract: callers (network.c::install_check_idle)
     * dup() the htlc fd and hand the dup to us; on every failure
     * path that returns FALSE we close it, on success the
     * spawned hxnet actor owns it via TcpStream::from_raw_fd and
     * closes it on its own Drop. The pre-spawn checks below all
     * jump to fail_close_fd; the post-spawn FFI failure has its
     * own NULL-handle close because at that point the spawn may
     * or may not have adopted the fd. */

    if (bridge_handle) {
        g_critical ("hxnet_bridge: install attempted while a connection is "
                    "already installed; refusing");
        goto fail_close_fd;
    }

    /* The encode and decode sides must agree on cipher /
     * compression — HOPE negotiates one algorithm per layer for
     * the whole connection. Refuse asymmetric configs up front;
     * they'd produce a wire that can't round-trip. */
    if (htlc->cipher_encode_type != htlc->cipher_decode_type) {
        g_critical ("hxnet_bridge: asymmetric cipher (encode=%d decode=%d) "
                    "unsupported",
                    htlc->cipher_encode_type, htlc->cipher_decode_type);
        goto fail_close_fd;
    }
    if (htlc->compress_encode_type != htlc->compress_decode_type) {
        g_critical ("hxnet_bridge: asymmetric compression (encode=%d "
                    "decode=%d) unsupported",
                    htlc->compress_encode_type, htlc->compress_decode_type);
        goto fail_close_fd;
    }

    hxnet_transform_config_t cfg;
    memset (&cfg, 0, sizeof (cfg));

    /* Cipher slot. */
    switch (htlc->cipher_encode_type) {
    case CIPHER_NONE:
        cfg.cipher_kind = HXNET_BRIDGE_CIPHER_NONE;
        break;
    case CIPHER_BLOWFISH: {
        /* R3.3.e-4g: HOPE-Blowfish goes through the
         * HopeBlowfishStream adapter which carries the per-
         * message rekey-marker logic. The bare
         * HXNET_BRIDGE_CIPHER_BLOWFISH kind is reserved for
         * future non-HOPE Blowfish use cases (e.g. a Blowfish
         * HTXF subchannel); HOPE always wants the HOPE-aware
         * variant. */
        cfg.cipher_kind = HXNET_BRIDGE_CIPHER_HOPE_BLOWFISH;
        /* HOPE derives DISTINCT read and write Blowfish keys from
         * the session keystream. The C side stores them on
         * htlc->cipher_decode_key (incoming wire — what we read
         * from the server) and htlc->cipher_encode_key (outgoing
         * — what we send to the server). The previous
         * single-key code worked only against servers that
         * happened to derive identical encode / decode keys; it
         * broke against VesperNet/Janus, whose keys differ. */
        if (htlc->cipher_encode_keylen == 0
            || htlc->cipher_encode_keylen > sizeof (cfg.blowfish_write_key)) {
            g_critical ("hxnet_bridge: invalid blowfish write key length %u",
                        htlc->cipher_encode_keylen);
            goto fail_close_fd;
        }
        if (htlc->cipher_decode_keylen == 0
            || htlc->cipher_decode_keylen > sizeof (cfg.blowfish_read_key)) {
            g_critical ("hxnet_bridge: invalid blowfish read key length %u",
                        htlc->cipher_decode_keylen);
            goto fail_close_fd;
        }
        /* cipher_*_init can leave htlc->cipher_*_state.stream
         * NULL on allocation failure — cipher.c explicitly
         * checks for NULL in its consumers. Mirror that here
         * so we don't hand a NULL through to the Rust save
         * helper. */
        if (htlc->cipher_encode_state.stream == NULL
            || htlc->cipher_decode_state.stream == NULL) {
            g_critical ("hxnet_bridge: blowfish state not initialised "
                        "(encode=%p decode=%p)",
                        htlc->cipher_encode_state.stream,
                        htlc->cipher_decode_state.stream);
            goto fail_close_fd;
        }
        cfg.blowfish_write_key_len = htlc->cipher_encode_keylen;
        memcpy (cfg.blowfish_write_key, htlc->cipher_encode_key,
                htlc->cipher_encode_keylen);
        cfg.blowfish_read_key_len = htlc->cipher_decode_keylen;
        memcpy (cfg.blowfish_read_key, htlc->cipher_decode_key,
                htlc->cipher_decode_keylen);
        /* Snapshot the live OFB ivec + num on each direction so
         * the negotiated stream position survives the handoff.
         * num is the byte offset (0..7) into the current
         * keystream block — losing it would desync the Rust
         * BlowfishStream from the server's keystream whenever
         * install fires mid-block. See the doc-comment on
         * blowfish_*_num in hxnet_transform_config_t. */
        guint32 write_num = 0;
        guint32 read_num  = 0;
        gtkhx_blowfish_ofb64_save_state (htlc->cipher_encode_state.stream,
                                         cfg.blowfish_write_ivec,
                                         &write_num);
        gtkhx_blowfish_ofb64_save_state (htlc->cipher_decode_state.stream,
                                         cfg.blowfish_read_ivec,
                                         &read_num);
        /* save_state writes a u32 but the field is u8; num is
         * always 0..7 by the OFB-64 block size invariant. Mask
         * defensively in case the Rust side ever changes that. */
        cfg.blowfish_write_num = (guint8) (write_num & 7);
        cfg.blowfish_read_num  = (guint8) (read_num  & 7);

        /* R3.3.e-4g: populate the HOPE per-message rekey inputs.
         * Translate the C-side `macalg` string to the protocol-
         * level tag the Rust adapter consumes. Anything other
         * than the three known names fails closed — the legacy
         * code's `hmac_xxx` would silently return length 0
         * here, which on the Rust side surfaces as an
         * InvalidData read error. */
        if (strcmp (htlc->macalg, "HMAC-SHA256") == 0) {
            cfg.hope_macalg = HXNET_BRIDGE_MACALG_SHA256;
        } else if (strcmp (htlc->macalg, "HMAC-SHA1") == 0) {
            cfg.hope_macalg = HXNET_BRIDGE_MACALG_SHA1;
        } else if (strcmp (htlc->macalg, "HMAC-MD5") == 0) {
            cfg.hope_macalg = HXNET_BRIDGE_MACALG_MD5;
        } else {
            g_critical ("hxnet_bridge: HOPE-Blowfish: unsupported MAC "
                        "algorithm \"%s\"", htlc->macalg);
            goto fail_close_fd;
        }
        if (htlc->sklen == 0
            || htlc->sklen > sizeof (cfg.hope_session_key)) {
            g_critical ("hxnet_bridge: HOPE-Blowfish: invalid session "
                        "key length %u", (unsigned) htlc->sklen);
            goto fail_close_fd;
        }
        cfg.hope_session_key_len = htlc->sklen;
        memcpy (cfg.hope_session_key, htlc->sessionkey, htlc->sklen);
        break;
    }
    case CIPHER_CHACHA20_POLY1305: {
        cfg.cipher_kind = HXNET_BRIDGE_CIPHER_CHACHA20_POLY1305;
        /* Validate the AEAD direction tags BEFORE handing fd to
         * the Rust FFI. The FFI validates them too, but its
         * order-of-operations is "validate direction tags →
         * TcpStream::from_raw_fd → adopt fd". On a tag-validation
         * failure the FFI returns NULL without having adopted the
         * fd; our post-spawn NULL branch assumes adoption and
         * skips close (avoiding double-close on the common
         * failure paths that happen after from_raw_fd). Without
         * this front-loaded check, a corrupted chacha.dir would
         * leak the duped fd.
         *
         * Tags come from src/cipher_aead.h:
         *   CIPHER_AEAD_DIR_SERVER_TO_CLIENT = 0
         *   CIPHER_AEAD_DIR_CLIENT_TO_SERVER = 1
         *
         * Decode (read) must be S2C; encode (write) must be C2S.
         * Equal directions would re-use the same nonce stream
         * in both directions, breaking the AEAD security
         * argument outright. */
        if (htlc->cipher_decode_state.chacha.dir
                != CIPHER_AEAD_DIR_SERVER_TO_CLIENT
            || htlc->cipher_encode_state.chacha.dir
                != CIPHER_AEAD_DIR_CLIENT_TO_SERVER) {
            g_critical ("hxnet_bridge: AEAD direction tags invalid or "
                        "swapped (decode=%u expected %u, encode=%u "
                        "expected %u)",
                        htlc->cipher_decode_state.chacha.dir,
                        CIPHER_AEAD_DIR_SERVER_TO_CLIENT,
                        htlc->cipher_encode_state.chacha.dir,
                        CIPHER_AEAD_DIR_CLIENT_TO_SERVER);
            goto fail_close_fd;
        }
        /* chacha_aead_state field-for-field matches AeadState in
         * hxcrypto-aead — pinned by the _Static_assert on
         * sizeof(chacha_aead_state) == 48 in cipher.h. We could
         * memcpy the whole struct, but explicit field copies
         * make the mapping obvious to future readers. */
        memcpy (cfg.aead_write_key,
                htlc->cipher_encode_state.chacha.key,
                sizeof (cfg.aead_write_key));
        cfg.aead_write_counter = htlc->cipher_encode_state.chacha.counter;
        cfg.aead_write_dir     = htlc->cipher_encode_state.chacha.dir;
        memcpy (cfg.aead_read_key, htlc->cipher_decode_state.chacha.key,
                sizeof (cfg.aead_read_key));
        cfg.aead_read_counter = htlc->cipher_decode_state.chacha.counter;
        cfg.aead_read_dir     = htlc->cipher_decode_state.chacha.dir;
        break;
    }
    default:
        g_critical ("hxnet_bridge: unsupported cipher type %d",
                    htlc->cipher_encode_type);
        goto fail_close_fd;
    }

    /* Compression slot. */
    switch (htlc->compress_encode_type) {
    case COMPRESS_NONE:
        cfg.compression_kind = HXNET_BRIDGE_COMPRESSION_NONE;
        break;
    case COMPRESS_GZIP:
        cfg.compression_kind = HXNET_BRIDGE_COMPRESSION_GZIP;
        break;
    case COMPRESS_LZ4:
        cfg.compression_kind = HXNET_BRIDGE_COMPRESSION_LZ4;
        break;
    case COMPRESS_ZSTD:
        cfg.compression_kind = HXNET_BRIDGE_COMPRESSION_ZSTD;
        break;
    default:
        g_critical ("hxnet_bridge: unsupported compression type %d",
                    htlc->compress_encode_type);
        goto fail_close_fd;
    }

    hxnet_connection_opaque *h
        = hxnet_connection_spawn_fd_with_transforms_and_callback (
            fd, &cfg, bridge_on_event_cb, bridge_on_shutdown_cb, htlc);
    if (!h) {
        /* The FFI returns NULL on two classes of failure: pre-adoption
         * validation (NULL config / callbacks, fd < 0, unknown
         * cipher/compression kind), which fires BEFORE
         * TcpStream::from_raw_fd takes the fd; and post-adoption errors
         * (set_nonblocking, from_std, compose, spawn_boxed), which fire
         * AFTER. We deliberately do NOT close(fd) here: the code above
         * validated fd >= 0 and the asymmetric-config guard, and the
         * cipher/compression kinds handed to the FFI are the fixed
         * HXNET_BRIDGE_* constants assigned above (always a known hxnet
         * kind), so the only NULL returns reachable from here are
         * post-adoption, where the dropped stream's Drop already closed
         * the fd and a close() here would be a double-close. If the kind
         * mappings or the FFI's pre-adoption validation change, this
         * contract (and this no-close path) must be revisited. */
        return FALSE;
    }
    bridge_handle = h;
    bridge_htlc   = htlc;
    return TRUE;

fail_close_fd:
    /* Pre-spawn failure: the duped fd never reached the Rust
     * actor, so we still own it. Close to honour the
     * install-on-success / close-on-failure contract documented
     * at the top of this function. */
    close (fd);
    return FALSE;
}

gboolean
hx_bridge_is_installed (void)
{
    return bridge_handle != NULL;
}

int
hx_bridge_send_frame (const guint8 *data, guint32 len)
{
    if (!bridge_handle) {
        g_critical ("hxnet_bridge: send_frame called with no installed "
                    "connection");
        /* Distinct sentinel — production hxnet returns the
         * HXNET_SEND_* set (0, -1, -2, -3); HX_BRIDGE_SEND_NOT_INSTALLED
         * sits outside that range so callers can distinguish
         * "bridge not installed" from kernel backpressure / closed
         * channel. */
        return HX_BRIDGE_SEND_NOT_INSTALLED;
    }
    int rc = hxnet_connection_send_frame (bridge_handle, data, len);
    if (rc != 0) {
        g_critical ("hxnet_bridge: hxnet_connection_send_frame returned %d",
                    rc);
    }
    return rc;
}

void
hx_bridge_uninstall (void)
{
    if (!bridge_handle) {
        return;
    }
    hxnet_connection_opaque *h = bridge_handle;
    bridge_handle = NULL;
    bridge_htlc   = NULL;
    hxnet_connection_destroy (h);
}
