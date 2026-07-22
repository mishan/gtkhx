#ifndef HX_TEST_HTLC_RECV_BUF_H
#define HX_TEST_HTLC_RECV_BUF_H 1

/*
 * Test-only receive/scratch buffer for a struct htlc_conn.
 *
 * Production no longer carries a receive staging buffer on
 * struct htlc_conn — the hxnet bridge assembles each frame into a
 * transient block and hands it to the handlers as an explicit
 * (frame, frame_len) slice. The Tier 2 (proto) and Tier 3
 * (integration) tests, however, still want a per-connection qbuf to
 * pack a canned frame into (proto) or to read a server reply into
 * (integration), then walk with dh_start / feed to the proto_helpers
 * extractors.
 *
 * hx_test_in() hands back a per-htlc struct qbuf kept in a side table
 * keyed by the htlc pointer (lazily created, zeroed on first use). It
 * replaces the old htlc->in field verbatim: every `htlc->in.buf` /
 * `htlc->in.pos` / `htlc->in.len` becomes `hx_test_in(htlc)->buf` etc.
 *
 * hx_test_in_free() frees the qbuf's buffer and drops the table entry;
 * the test teardown helpers (wire_fixture_free, htlc_free,
 * integration_release_htlc) call it. Single-threaded sequential test
 * use only — the table is a plain global.
 */

#include <glib.h>
#include "protocol.h" /* struct qbuf, struct htlc_conn */

struct qbuf *hx_test_in (const struct htlc_conn *htlc);
void hx_test_in_free (const struct htlc_conn *htlc);

#endif /* HX_TEST_HTLC_RECV_BUF_H */
