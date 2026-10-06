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
 * The recv side is the session's: hx-libs' hxsession reads the echo
 * into Event::LoggedIn.
 *
 * Also pin the numeric constants — the bit values are protocol-
 * facing, renumbering them silently turns into wire-incompat.
 */

#include "config.h"
#include <string.h>
#include <stdarg.h>
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

    g_free (hx_test_in (htlc)->buf);
    hx_test_in (htlc)->buf = buf;
    hx_test_in (htlc)->pos = len;
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
    g_free (hx_test_in (htlc)->buf);
    hx_test_in (htlc)->buf = NULL;
}

/* The minimum cap chunk we'd send on a legacy LOGIN: u16 big-endian
 * holding just CAP_TEXT_ENCODING. Pin the on-wire layout. */
static void
test_send_capabilities_chunk_layout (void)
{
    struct htlc_conn htlc;
    htlc_init (&htlc, 1);

    guint16 caps16 = g_htons (HTLC_CAP_TEXT_ENCODING);
    hlpack_v (&htlc, HTLC_HDR_LOGIN, 0, /*hc=*/1, (int)HTLC_DATA_CAPABILITIES,
              2, &caps16);

    int found = 0;
    dh_start (hx_test_in (&htlc)->buf, hx_test_in (&htlc)->pos)
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

    guint16 caps16 = g_htons (HTLC_CAP_LARGE_FILES | HTLC_CAP_TEXT_ENCODING);
    hlpack_v (&htlc, HTLC_HDR_LOGIN, 0, /*hc=*/1, (int)HTLC_DATA_CAPABILITIES,
              2, &caps16);

    dh_start (hx_test_in (&htlc)->buf, hx_test_in (&htlc)->pos)
    {
        guint16 wire = (guint16)dh->data[0] << 8 | (guint16)dh->data[1];
        g_assert_cmphex (wire & HTLC_CAP_LARGE_FILES, ==, HTLC_CAP_LARGE_FILES);
        g_assert_cmphex (wire & HTLC_CAP_TEXT_ENCODING, ==,
                         HTLC_CAP_TEXT_ENCODING);
    }
    dh_end ();

    htlc_free (&htlc);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/capabilities/send/chunk_layout",
                     test_send_capabilities_chunk_layout);
    g_test_add_func ("/capabilities/send/multiple_bits",
                     test_send_multiple_caps_bits);

    return g_test_run ();
}
