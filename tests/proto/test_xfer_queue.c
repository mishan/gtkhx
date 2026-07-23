/*
 * tests/proto/test_xfer_queue.c — drive hx_xfer_queue_extract
 * against canned HTLS_HDR_QUEUE payloads.
 *
 * Servers send XFER_QUEUE replies during file transfers to indicate
 * "you're in queue position N" or, on queueid==0, "you're at the
 * front of the queue, start the transfer." The handler matches
 * htxf_with_ref(ref) against our local transfer table and updates
 * the htxf->queue field.
 *
 * Both chunks are 32-bit:
 *   HTLS_DATA_HTXF_REF — file-transfer reference
 *   HTLS_DATA_QUEUE    — queue position (0 = ready to start)
 */

#include "config.h"
#include <string.h>
#include <glib.h>
#include "protocol.h"
#include "hotline.h"
#include "proto_helpers.h"
#include "wire_fixture.h"

static void
test_xfer_queue_extracts_ref_and_queueid (void)
{
    struct htlc_conn htlc;
    const guint32 ref_wire = g_htonl(0xdeadbeefu);
    const guint32 queue_wire = g_htonl(5);

    wire_fixture_init (&htlc, HTLS_HDR_QUEUE, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_HTXF_REF, sizeof (ref_wire),
                            &ref_wire);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_QUEUE, sizeof (queue_wire),
                            &queue_wire);

    struct hx_xfer_queue_msg xq;
    g_assert_true (hx_xfer_queue_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, &xq));
    g_assert_cmphex (xq.ref, ==, 0xdeadbeefu);
    g_assert_cmphex (xq.queueid, ==, 5);

    wire_fixture_free (&htlc);
}

/* queueid == 0 is the "you're ready, start the transfer" signal.
 * The downstream handler (rcv.c) interprets this by calling
 * xfer_ready_write(htxf). Pin it down: the wire encoding is
 * literally the int 0, the handler doesn't have a separate
 * "I'm ready" chunk type. */
static void
test_xfer_queue_queueid_zero_is_valid (void)
{
    struct htlc_conn htlc;
    const guint32 ref_wire = g_htonl(1);
    const guint32 queue_wire = g_htonl(0);

    wire_fixture_init (&htlc, HTLS_HDR_QUEUE, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_HTXF_REF, sizeof (ref_wire),
                            &ref_wire);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_QUEUE, sizeof (queue_wire),
                            &queue_wire);

    struct hx_xfer_queue_msg xq;
    g_assert_true (hx_xfer_queue_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, &xq));
    g_assert_cmphex (xq.ref, ==, 1);
    g_assert_cmphex (xq.queueid, ==, 0);

    wire_fixture_free (&htlc);
}

static void
test_xfer_queue_missing_ref_defaults_to_zero (void)
{
    /* Server malformedly sends only QUEUE without REF — the
	 * extractor still parses; the rcv.c handler then fails the
	 * htxf_with_ref(0) lookup and warns. */
    struct htlc_conn htlc;
    const guint32 queue_wire = g_htonl(3);

    wire_fixture_init (&htlc, HTLS_HDR_QUEUE, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_QUEUE, sizeof (queue_wire),
                            &queue_wire);

    struct hx_xfer_queue_msg xq;
    g_assert_true (hx_xfer_queue_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, &xq));
    g_assert_cmphex (xq.ref, ==, 0);
    g_assert_cmphex (xq.queueid, ==, 3);

    wire_fixture_free (&htlc);
}

static void
test_xfer_queue_empty_payload_zero_defaults (void)
{
    struct htlc_conn htlc;
    wire_fixture_init (&htlc, HTLS_HDR_QUEUE, 1, 0);

    struct hx_xfer_queue_msg xq;
    g_assert_true (hx_xfer_queue_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, &xq));
    g_assert_cmphex (xq.ref, ==, 0);
    g_assert_cmphex (xq.queueid, ==, 0);

    wire_fixture_free (&htlc);
}

static void
test_xfer_queue_unrelated_chunks_skipped (void)
{
    struct htlc_conn htlc;
    const guint32 ref_wire = g_htonl(7);
    const guint16 some_uid = g_htons(99);

    wire_fixture_init (&htlc, HTLS_HDR_QUEUE, 1, 0);
    /* DATA_UID and DATA_NAME chunks aren't in the XFER_QUEUE
	 * message, but the dh_start walker should ignore them. */
    wire_fixture_add_chunk (&htlc, HTLS_DATA_UID, sizeof (some_uid), &some_uid);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_HTXF_REF, sizeof (ref_wire),
                            &ref_wire);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_NAME, 4, "spam");

    struct hx_xfer_queue_msg xq;
    g_assert_true (hx_xfer_queue_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, &xq));
    g_assert_cmphex (xq.ref, ==, 7);
    g_assert_cmphex (xq.queueid, ==, 0);

    wire_fixture_free (&htlc);
}

static void
test_xfer_queue_max_uint32_round_trips (void)
{
    /* Pin down 32-bit encoding round-trip — REF and QUEUE values
	 * around the high-bit boundary used to occasionally show up as
	 * sign-extended in early Hotline implementations. dh_getint
	 * uses HN32 which is unsigned. */
    struct htlc_conn htlc;
    const guint32 ref_wire = g_htonl(0xffffffffu);
    const guint32 queue_wire = g_htonl(0x80000000u);

    wire_fixture_init (&htlc, HTLS_HDR_QUEUE, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_HTXF_REF, sizeof (ref_wire),
                            &ref_wire);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_QUEUE, sizeof (queue_wire),
                            &queue_wire);

    struct hx_xfer_queue_msg xq;
    g_assert_true (hx_xfer_queue_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, &xq));
    g_assert_cmphex (xq.ref, ==, 0xffffffffu);
    g_assert_cmphex (xq.queueid, ==, 0x80000000u);

    wire_fixture_free (&htlc);
}

static void
test_xfer_queue_null_out_returns_false (void)
{
    struct htlc_conn htlc;
    wire_fixture_init (&htlc, HTLS_HDR_QUEUE, 1, 0);

    g_assert_false (hx_xfer_queue_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, NULL));

    wire_fixture_free (&htlc);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/proto/xfer_queue/extracts_ref_and_queueid",
                     test_xfer_queue_extracts_ref_and_queueid);
    g_test_add_func ("/proto/xfer_queue/queueid_zero_is_valid",
                     test_xfer_queue_queueid_zero_is_valid);
    g_test_add_func ("/proto/xfer_queue/missing_ref_defaults_to_zero",
                     test_xfer_queue_missing_ref_defaults_to_zero);
    g_test_add_func ("/proto/xfer_queue/empty_payload_zero_defaults",
                     test_xfer_queue_empty_payload_zero_defaults);
    g_test_add_func ("/proto/xfer_queue/unrelated_chunks_skipped",
                     test_xfer_queue_unrelated_chunks_skipped);
    g_test_add_func ("/proto/xfer_queue/max_uint32_round_trips",
                     test_xfer_queue_max_uint32_round_trips);
    g_test_add_func ("/proto/xfer_queue/null_out_returns_false",
                     test_xfer_queue_null_out_returns_false);

    return g_test_run ();
}
