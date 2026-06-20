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

#include <glib.h>

#include "hxnet_bridge.h"
#include "protocol.h"
#include "proto_helpers.h"

/* Forward declaration of the production header decoder
 * (proto_helpers.c). hx_rcv_hdr decodes the buffered header by
 * calling this; we don't need to call it directly here. */
extern void hx_rcv_hdr (struct htlc_conn *htlc);

/* Forward declaration of the production teardown. Defined in
 * network.c; we trampoline to it from the shutdown bridge.
 * Marked `expected` semantically by the reason translation
 * inside hx_bridge_dispatch_shutdown. */
extern void hx_htlc_close (struct htlc_conn *htlc, int expected);

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
    /* len2: legacy unused field, set to zero. */
    dst[16] = 0;
    dst[17] = 0;
    dst[18] = 0;
    dst[19] = 0;
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

#define HXNET_BRIDGE_CIPHER_NONE      0
#define HXNET_BRIDGE_COMPRESSION_NONE 0

typedef struct {
    guint32 cipher_kind;
    guint32 compression_kind;
    guint32 blowfish_key_len;
    guint8  blowfish_key[56];
    guint8  blowfish_read_ivec[8];
    guint8  blowfish_write_ivec[8];
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
_Static_assert (sizeof (hxnet_transform_config_t) == 176,
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

/*
 * Single-connection state. gtkhx is single-conn today (the
 * MAX_CONN > 1 scaffolding in hx.h is a lie — see CLAUDE.md);
 * a global suffices. R3.5+'s multi-conn UI will move this into
 * htlc_conn alongside the existing cipher / compress state.
 */
static hxnet_connection_opaque *bridge_handle;
static struct htlc_conn        *bridge_htlc;

/* C-side trampolines hxnet calls on the GLib main thread per
 * Event::Frame / Event::Shutdown. They translate the FFI
 * shapes into the bridge's existing dispatch helpers, free the
 * frame body (per the on_event ownership contract), and clear
 * the global on shutdown so the install gate flips off. */
static void
bridge_on_event_cb (hxnet_connection_opaque *conn G_GNUC_UNUSED,
                    hxnet_frame_t *frame, void *user_data)
{
    struct htlc_conn *htlc = user_data;
    if (frame && htlc) {
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
bridge_on_shutdown_cb (hxnet_connection_opaque *conn G_GNUC_UNUSED, int reason,
                       void *user_data)
{
    struct htlc_conn *htlc = user_data;

    /* Tear down the hxnet handle here — the actor's already
     * exited (that's what got us into this callback), so
     * destroying the handle drops the cmd channel, the pump
     * task, and the forwarder all together. Doing the destroy
     * in this callback (instead of leaving it to a follow-up
     * hx_bridge_uninstall) means the lifetime stays
     * deterministic: the pump's JoinHandle and the
     * MainForwarder are released before we return to the
     * forwarder's outer scope, so there's no path where the
     * handle survives the shutdown event.
     *
     * Order:
     *
     *   1. Snapshot the handle pointer + clear the globals.
     *      Clearing first prevents re-entry via
     *      hx_bridge_send_frame / hx_bridge_uninstall during
     *      the dispatch (hx_htlc_close → hx_bridge_uninstall
     *      below now sees no handle and short-circuits, as
     *      designed).
     *   2. Dispatch into hx_htlc_close. The legacy GIOStream
     *      teardown code still runs (control_remove_all_sources
     *      etc.) — it's a no-op for the hxnet path but does no
     *      harm; mixing teardown sites isn't worth the
     *      regression risk of branching it.
     *   3. Destroy the snapshotted handle. Safe to do after
     *      hx_htlc_close because nothing in the legacy
     *      teardown path touches the hxnet globals (we
     *      cleared them in step 1). */
    hxnet_connection_opaque *to_destroy = bridge_handle;
    bridge_handle = NULL;
    bridge_htlc = NULL;

    if (htlc) {
        hx_bridge_dispatch_shutdown (htlc, reason);
    }
    if (to_destroy) {
        hxnet_connection_destroy (to_destroy);
    }
}

gboolean
hx_bridge_install_passthrough (struct htlc_conn *htlc, int fd)
{
    g_return_val_if_fail (htlc != NULL, FALSE);
    g_return_val_if_fail (fd >= 0, FALSE);

    if (bridge_handle) {
        g_critical ("hxnet_bridge: install attempted while a connection is "
                    "already installed; refusing");
        return FALSE;
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
        /* hxnet logs via g_critical itself on the failure
         * paths; we just refuse the install. */
        return FALSE;
    }
    bridge_handle = h;
    bridge_htlc   = htlc;
    return TRUE;
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
