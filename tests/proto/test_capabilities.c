/*
 * tests/proto/test_capabilities.c — pin the wire shape of the
 * DATA_CAPABILITIES negotiation (fogWraith/Hotline
 * Docs/Protocol/Capabilities.md).
 *
 * The negotiation has two halves:
 *
 *   send  client sets bits for caps it wants in HTLC_DATA_CAPABILITIES
 *         (0x01f0), big-endian unsigned integer body
 *   recv  server echoes back the bits it agrees to in HTLS_DATA_CAPABILITIES
 *         on the LOGIN reply (same code point)
 *
 * Send-side: drive hlpack with the same arguments network.c uses on
 * the legacy LOGIN, then walk the packed bytes via dh_start and
 * assert the OPTIONS chunk landed with the expected u16-big-endian
 * payload.
 *
 * Recv-side: hand-build a LOGIN TASK reply via wire_fixture with a
 * DATA_CAPABILITIES chunk, walk it the same way the parser in
 * rcv.c::rcv_task_login does, and verify the decoded caps mask.
 * Cover both 2-byte (typical) and 8-byte (variable-width-extension)
 * encodings the spec allows.
 *
 * Also pin the numeric constants — the bit values are protocol-
 * facing, renumbering them silently turns into wire-incompat.
 */

#include "config.h"
#include <string.h>
#include <stdarg.h>
#include <netinet/in.h>
#include <glib.h>
#include "protocol.h"
#include "hotline.h"
#include "proto_helpers.h"
#include "wire_fixture.h"

/* ---------- send side: hlpack + dh_start round trip ---------- */

/* Pack straight into htlc->in so dh_start can walk it — hlpack now
 * returns a fresh buffer (there's no htlc->out send buffer anymore). */
static void
hlpack_v (struct htlc_conn *htlc, guint32 type, guint32 flag, int hc, ...)
{
    va_list ap;
    va_start (ap, hc);
    gsize len = 0;
    guint8 *buf = hlpack (htlc, type, flag, hc, ap, &len);
    va_end (ap);

    g_free (htlc->in.buf);
    htlc->in.buf = buf;
    htlc->in.pos = len;
}

static void
htlc_init (struct htlc_conn *htlc, guint32 starting_trans)
{
    memset (htlc, 0, sizeof (*htlc));
    htlc->trans = starting_trans;
}

static void
htlc_free (struct htlc_conn *htlc)
{
    g_free (htlc->in.buf);
    htlc->in.buf = NULL;
}

/* The minimum cap chunk we'd send on a legacy LOGIN: u16 big-endian
 * holding just CAP_TEXT_ENCODING. Pin the on-wire layout. */
static void
test_send_capabilities_chunk_layout (void)
{
    struct htlc_conn htlc;
    htlc_init (&htlc, 1);

    guint16 caps16 = htons (HTLC_CAP_TEXT_ENCODING);
    hlpack_v (&htlc, HTLC_HDR_LOGIN, 0, /*hc=*/1, (int)HTLC_DATA_CAPABILITIES,
              2, &caps16);


    int found = 0;
    dh_start (htlc.in.buf, htlc.in.pos)
    {
        found++;
        g_assert_cmphex (_type, ==, HTLC_DATA_CAPABILITIES);
        g_assert_cmpuint (_len, ==, 2);
        /* Big-endian decode of the 2-byte payload. */
        guint16 wire = (guint16)dh->data[0] << 8 | (guint16)dh->data[1];
        g_assert_cmphex (wire, ==, HTLC_CAP_TEXT_ENCODING);
    }
    dh_end ();
    g_assert_cmpint (found, ==, 1);

    htlc_free (&htlc);
}

/* Sending multiple bits is the typical real-world shape — once large
 * files lands as Phase E ∞, we'd advertise 0x0003. Verify both bits
 * survive the encode/decode. */
static void
test_send_multiple_caps_bits (void)
{
    struct htlc_conn htlc;
    htlc_init (&htlc, 1);

    guint16 caps16
        = htons (HTLC_CAP_LARGE_FILES | HTLC_CAP_TEXT_ENCODING);
    hlpack_v (&htlc, HTLC_HDR_LOGIN, 0, /*hc=*/1, (int)HTLC_DATA_CAPABILITIES,
              2, &caps16);


    dh_start (htlc.in.buf, htlc.in.pos)
    {
        guint16 wire = (guint16)dh->data[0] << 8 | (guint16)dh->data[1];
        g_assert_cmphex (wire & HTLC_CAP_LARGE_FILES, ==,
                         HTLC_CAP_LARGE_FILES);
        g_assert_cmphex (wire & HTLC_CAP_TEXT_ENCODING, ==,
                         HTLC_CAP_TEXT_ENCODING);
    }
    dh_end ();

    htlc_free (&htlc);
}

/* ---------- recv side: drive the real production decoder ----------
 *
 * Pre-refactor this test had its own copy of the variable-width
 * big-endian decoder that rcv.c::rcv_task_login uses. The whole
 * point of this test is to PIN the production behaviour, so calling
 * the production helper (hl_capabilities_decode in proto_helpers)
 * is strictly better than mimicking it — a future change to the
 * decoder shows up here as either a test pass with the new
 * semantics or a test-vector-and-decoder simultaneous drift, but
 * never a silent divergence where the test mimics out-of-date
 * behaviour. */
static guint64
decode_caps_from_reply (struct htlc_conn *htlc)
{
    guint64 caps = 0;
    dh_start (htlc->in.buf, htlc->in.pos)
    {
        if (_type != HTLS_DATA_CAPABILITIES) {
            continue;
        }
        caps = hl_capabilities_decode (dh->data, _len);
    }
    dh_end ();
    return caps;
}

/* Server echoes back the typical 2-byte cap mask with bit 1 set. */
static void
test_recv_caps_2byte_text_encoding (void)
{
    struct htlc_conn htlc;
    memset (&htlc, 0, sizeof htlc);
    wire_fixture_init (&htlc, HTLS_HDR_TASK, /*trans=*/1, /*flag=*/0);

    guint16 caps_be = htons (HTLC_CAP_TEXT_ENCODING);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_CAPABILITIES, 2, &caps_be);

    guint64 caps = decode_caps_from_reply (&htlc);
    g_assert_cmphex (caps, ==, HTLC_CAP_TEXT_ENCODING);
    g_assert_true (caps & HTLC_CAP_TEXT_ENCODING);
    g_assert_false (caps & HTLC_CAP_LARGE_FILES);

    wire_fixture_free (&htlc);
}

/* Server echoes multiple bits — proves bits 0 and 1 round-trip
 * together. */
static void
test_recv_caps_multiple_bits (void)
{
    struct htlc_conn htlc;
    memset (&htlc, 0, sizeof htlc);
    wire_fixture_init (&htlc, HTLS_HDR_TASK, /*trans=*/1, /*flag=*/0);

    guint16 caps_be
        = htons (HTLC_CAP_LARGE_FILES | HTLC_CAP_TEXT_ENCODING);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_CAPABILITIES, 2, &caps_be);

    guint64 caps = decode_caps_from_reply (&htlc);
    g_assert_true (caps & HTLC_CAP_LARGE_FILES);
    g_assert_true (caps & HTLC_CAP_TEXT_ENCODING);
}

/* The spec permits a variable-width body up to 8 bytes (64 bits).
 * A server that opts for the long form should still decode
 * correctly. Pin the wide-decode path. */
static void
test_recv_caps_8byte_wide_form (void)
{
    struct htlc_conn htlc;
    memset (&htlc, 0, sizeof htlc);
    wire_fixture_init (&htlc, HTLS_HDR_TASK, /*trans=*/1, /*flag=*/0);

    /* High word = some hypothetical future bit; low word = our
	 * familiar TEXT_ENCODING. The shift-and-OR loop must walk
	 * the whole 8 bytes to preserve the high bits. */
    guint8 body[8]
        = { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, HTLC_CAP_TEXT_ENCODING };
    wire_fixture_add_chunk (&htlc, HTLS_DATA_CAPABILITIES, 8, body);

    guint64 caps = decode_caps_from_reply (&htlc);
    g_assert_cmphex (caps, ==, 0x8000000000000000ull | HTLC_CAP_TEXT_ENCODING);
    g_assert_true (caps & HTLC_CAP_TEXT_ENCODING);

    wire_fixture_free (&htlc);
}

/* The LOGIN reply may omit DATA_CAPABILITIES entirely (server
 * doesn't support the extension, or supports it but didn't agree
 * to any of our advertised bits). In that case the decoded caps
 * mask is 0 — session falls back to standard mode. */
static void
test_recv_caps_absent_field_means_zero (void)
{
    struct htlc_conn htlc;
    memset (&htlc, 0, sizeof htlc);
    wire_fixture_init (&htlc, HTLS_HDR_TASK, /*trans=*/1, /*flag=*/0);

    /* Some other unrelated chunk, no CAPABILITIES. */
    guint16 version_be = htons (190);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_VERSION, 2, &version_be);

    guint64 caps = decode_caps_from_reply (&htlc);
    g_assert_cmphex (caps, ==, 0);

    wire_fixture_free (&htlc);
}

/* A 1-byte cap body — unusual but legal under the spec's variable-
 * width clause. The decoder must promote it correctly. */
static void
test_recv_caps_1byte_form (void)
{
    struct htlc_conn htlc;
    memset (&htlc, 0, sizeof htlc);
    wire_fixture_init (&htlc, HTLS_HDR_TASK, /*trans=*/1, /*flag=*/0);

    guint8 body[1] = { HTLC_CAP_TEXT_ENCODING };
    wire_fixture_add_chunk (&htlc, HTLS_DATA_CAPABILITIES, 1, body);

    guint64 caps = decode_caps_from_reply (&htlc);
    g_assert_cmphex (caps, ==, HTLC_CAP_TEXT_ENCODING);

    wire_fixture_free (&htlc);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/capabilities/send/chunk_layout",
                     test_send_capabilities_chunk_layout);
    g_test_add_func ("/capabilities/send/multiple_bits",
                     test_send_multiple_caps_bits);

    g_test_add_func ("/capabilities/recv/2byte_text_encoding",
                     test_recv_caps_2byte_text_encoding);
    g_test_add_func ("/capabilities/recv/multiple_bits",
                     test_recv_caps_multiple_bits);
    g_test_add_func ("/capabilities/recv/8byte_wide_form",
                     test_recv_caps_8byte_wide_form);
    g_test_add_func ("/capabilities/recv/absent_field_means_zero",
                     test_recv_caps_absent_field_means_zero);
    g_test_add_func ("/capabilities/recv/1byte_form",
                     test_recv_caps_1byte_form);

    return g_test_run ();
}
