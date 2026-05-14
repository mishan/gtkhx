#ifndef HX_WIRE_FIXTURE_H
#define HX_WIRE_FIXTURE_H 1

/*
 * Helpers for building canned Hotline wire-format buffers that the
 * Tier 2 protocol-parser tests feed into rcv.c handlers.
 *
 * The handlers in rcv.c read from htlc->in.buf and walk the chunk
 * list with the dh_start / dh_end macros from protocol.h. The macro
 * assumes:
 *
 *   - htlc->in.buf points at a buffer that begins with a 22-byte
 *     hl_hdr (type, trans, flag, len, len2, hc).
 *   - htlc->in.pos is the total number of bytes the buffer holds.
 *   - Each chunk after the header is hl_data_hdr (type, len) +
 *     payload; types and lengths are big-endian on the wire.
 *
 * wire_fixture_init() zeroes a struct htlc_conn, builds the header,
 * and points htlc->in.buf at an internally-managed buffer. Each
 * wire_fixture_add_chunk() call appends one data chunk and updates
 * the header's hc / len fields. wire_fixture_free() releases the
 * buffer.
 *
 * Single-shot — one fixture per test. The internal buffer is freed
 * by wire_fixture_free().
 */

#include <glib.h>
#include "protocol.h"

/*
 * Initialise htlc->in with a fresh hl_hdr. Pass `flag = 1` to mark
 * the message as a task-error reply (which is what task_error()
 * expects).
 */
extern void wire_fixture_init (struct htlc_conn *htlc, guint32 type,
                               guint32 trans, guint32 flag);

/*
 * Append one hl_data_hdr-prefixed chunk to htlc->in.buf and update
 * the header's hc + len. `data` may be NULL when `len == 0`.
 */
extern void wire_fixture_add_chunk (struct htlc_conn *htlc, guint16 type,
                                    guint16 len, const void *data);

/*
 * Free the buffer wire_fixture_init / _add_chunk allocated and zero
 * htlc->in. Safe to call multiple times.
 */
extern void wire_fixture_free (struct htlc_conn *htlc);

#endif /* HX_WIRE_FIXTURE_H */
