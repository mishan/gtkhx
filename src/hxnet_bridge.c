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
