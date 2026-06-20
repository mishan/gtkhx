/*
 * hxnet_bridge.h — translation layer between hxnet's callback FFI
 * and gtkhx's existing rcv state machine. Phase R3.3.e-4a.
 *
 * hxnet emits plaintext Hotline frames as HxnetFrame structs
 * delivered to a C callback on the GLib main thread. The
 * existing rcv path in src/rcv.c reads from htlc->in (a 22-byte
 * header buffer; resized for the body after hx_rcv_hdr decodes
 * the type) and runs body handlers via htlc->rcv. These helpers
 * marshal an HxnetFrame into the rcv state machine without
 * involving hx_decode (the cipher / compression layers are
 * already handled by hxnet's transform stack — the bytes
 * arriving here are plaintext).
 *
 * The bridge is intentionally side-effect-free outside of htlc:
 * it touches htlc->in, htlc->rcv, and the body handlers; it
 * does NOT call hxnet_frame_free (the caller's on_event closure
 * owns the lifetime of the supplied frame fields), and it does
 * NOT touch any GIOStream / GPollable state. R3.3.e-4b will
 * wire it into network.c's connection lifecycle.
 *
 * Standalone (R3.3.e-4a) status: this module is dead from
 * production's perspective today. It's exercised by the tier-1
 * tests in tests/unit/test_hxnet_bridge.c so the translation
 * contract is locked before R3.3.e-4b's lifecycle refactor
 * lands on top.
 */

#ifndef _HXNET_BRIDGE_H
#define _HXNET_BRIDGE_H

#include <glib.h>
#include <stdint.h>

#include "hx.h"
#include "protocol.h"

G_BEGIN_DECLS

/*
 * Pack a Hotline 22-byte message header into `dst`.
 *
 *   dst        : caller-supplied buffer of at least SIZEOF_HL_HDR
 *                bytes. Must be writable.
 *   type/trans/flag/hc : host-order fields from a decoded
 *                HxnetFrame.
 *   body_len   : body byte count (no hc adjustment — this is the
 *                hxnet "body" length, matching
 *                hotline_proto::parse::HeaderDecoded.body_len).
 *
 * The wire `len` field is encoded as `body_len + sizeof(hc)` so
 * production's hl_hdr_decode reports the same body_len back via
 * its `body_len_out` parameter. `len2` is set to zero (legacy
 * unused field).
 *
 * Visibility: declared here for testability — tests/unit/
 * test_hxnet_bridge.c exercises the round-trip against
 * hl_hdr_decode without standing up a struct htlc_conn.
 */
extern void hx_bridge_pack_header (guint8 *dst, guint32 type, guint32 trans,
                                   guint32 flag, guint16 hc, guint32 body_len);

/*
 * Run a plaintext Hotline frame through the rcv state machine.
 *
 * Stages the packed 22-byte header + body bytes into
 * `htlc->in`, calls `hx_rcv_hdr` (which decodes the header and
 * sets `htlc->rcv` to the appropriate body handler), then
 * dispatches the body handler if one was selected and a body is
 * present. Leaves `htlc->in` in the "expecting next header"
 * state when the dispatch returns (matches what
 * control_on_readable's reset-leg leaves behind today, so a
 * follow-up call works cleanly).
 *
 * `body` may be NULL when `body_len == 0`; otherwise it must
 * point at `body_len` readable bytes. The bytes are copied out
 * of `body` before the body handler runs, so the caller may
 * free `body` immediately on return.
 */
extern void hx_bridge_dispatch_frame (struct htlc_conn *htlc, guint32 type,
                                      guint32 trans, guint32 flag, guint16 hc,
                                      const guint8 *body, guint32 body_len);

/*
 * Translate an hxnet shutdown reason into hx_htlc_close's
 * `expected` flag and tear the connection down.
 *
 *   HXNET_SHUTDOWN_EOF (0)             → expected = 1 (clean
 *                                        peer-side close)
 *   HXNET_SHUTDOWN_HANDLE_DROPPED (3)  → expected = 1 (we
 *                                        initiated the close)
 *   HXNET_SHUTDOWN_STREAM_ERROR (1)    → expected = 0
 *   HXNET_SHUTDOWN_FRAME_TOO_LARGE (2) → expected = 0
 *
 * Unknown reason codes are conservatively treated as
 * unexpected so the user sees the error dialog rather than a
 * silent disconnect.
 */
extern void hx_bridge_dispatch_shutdown (struct htlc_conn *htlc, int reason);

G_END_DECLS

#endif /* _HXNET_BRIDGE_H */
