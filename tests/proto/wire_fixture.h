#ifndef HX_WIRE_FIXTURE_H
#define HX_WIRE_FIXTURE_H 1

/*
 * Helpers for building canned Hotline wire-format buffers that the
 * Tier 2 protocol-parser tests feed into rcv.c handlers.
 *
 * The proto_helpers extractors take an explicit (buf, len) frame
 * slice and walk the chunk list with the dh_start / dh_end macros
 * from protocol.h. The fixture builds that slice:
 *
 *   - the buffer begins with a 22-byte hl_hdr (type, trans, flag,
 *     len, len2, hc).
 *   - the buffer length is the total number of bytes it holds.
 *   - Each chunk after the header is hl_data_hdr (type, len) +
 *     payload; types and lengths are big-endian on the wire.
 *
 * The buffer lives in the test-only per-htlc receive qbuf
 * (hx_test_in(), see htlc_recv_buf.h) — production struct htlc_conn
 * no longer carries a receive buffer. wire_fixture_init() zeroes the
 * struct htlc_conn and builds the header; each wire_fixture_add_chunk()
 * appends one data chunk and updates the header's hc / len fields;
 * tests read the built slice via hx_test_in(htlc)->buf / ->pos.
 * wire_fixture_free() releases the buffer.
 *
 * Single-shot — one fixture per test. The internal buffer is freed
 * by wire_fixture_free().
 */

#include <glib.h>
#include "protocol.h"
#include "htlc_recv_buf.h" /* hx_test_in — where the built frame lives */

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
