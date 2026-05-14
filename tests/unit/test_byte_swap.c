/*
 * tests/unit/test_byte_swap.c — verify the HN16 / HN32 byte-swap
 * macros that protocol.h uses to read big-endian Hotline wire fields
 * into host-order integers.
 *
 * The Hotline wire format is big-endian. On little-endian hosts
 * (every modern desktop), HN16/HN32 swap on the way in/out; on
 * big-endian hosts they're a straight copy. Either way the contract
 * is identical: write a 4-byte big-endian sequence, HN32 it into a
 * uint32_t, expect the obvious host-order integer.
 *
 * Why bother testing this? Two reasons:
 *   1. The macros are byte-pointer arithmetic — easy to typo.
 *   2. They're the load-bearing piece of every chunk parse in
 *      rcv.c. If the byte-order code goes wrong, every protocol
 *      test below this layer also fails, but in confusing ways.
 *      Catching it at this seam is much faster.
 *
 * The macros take generic void-cast pointers in either direction;
 * we cover both "read from a wire byte buffer into a u_int32_t" and
 * "round-trip back out to a fresh byte buffer" so we don't just
 * exercise one direction.
 */

#include "config.h"
#include <string.h>
#include <glib.h>
#include <sys/types.h>
#include "protocol.h"

/* ---------- HN32 ---------- */

static void
test_hn32_zero (void)
{
    const guint8 wire[4] = { 0x00, 0x00, 0x00, 0x00 };
    guint32 host = 0xdeadbeef;
    HN32 (&host, wire);
    g_assert_cmphex (host, ==, 0x00000000u);
}

static void
test_hn32_one (void)
{
    const guint8 wire[4] = { 0x00, 0x00, 0x00, 0x01 };
    guint32 host = 0;
    HN32 (&host, wire);
    g_assert_cmphex (host, ==, 0x00000001u);
}

static void
test_hn32_max (void)
{
    const guint8 wire[4] = { 0xff, 0xff, 0xff, 0xff };
    guint32 host = 0;
    HN32 (&host, wire);
    g_assert_cmphex (host, ==, 0xffffffffu);
}

static void
test_hn32_canonical_pattern (void)
{
    /* The classic byte-order test pattern; if this fails on a
	 * little-endian host the swap arithmetic is broken. */
    const guint8 wire[4] = { 0x12, 0x34, 0x56, 0x78 };
    guint32 host = 0;
    HN32 (&host, wire);
    g_assert_cmphex (host, ==, 0x12345678u);
}

static void
test_hn32_high_byte_only (void)
{
    const guint8 wire[4] = { 0x80, 0x00, 0x00, 0x00 };
    guint32 host = 0;
    HN32 (&host, wire);
    g_assert_cmphex (host, ==, 0x80000000u);
}

static void
test_hn32_low_byte_only (void)
{
    const guint8 wire[4] = { 0x00, 0x00, 0x00, 0x80 };
    guint32 host = 0;
    HN32 (&host, wire);
    g_assert_cmphex (host, ==, 0x00000080u);
}

/* Real Hotline header values we stare at in proto traces. Byte-for-byte
 * what mhxd / hlserver send for HTLC_HDR_LOGIN, HTLS_HDR_TASK,
 * HTLC_HDR_PING, etc. */
static void
test_hn32_real_protocol_constants (void)
{
    struct {
        guint8 wire[4];
        guint32 expected;
    } cases[] = {
        { { 0x00, 0x00, 0x00, 0x6b }, 0x0000006bu }, /* HTLC_HDR_LOGIN */
        { { 0x00, 0x01, 0x00, 0x00 }, 0x00010000u }, /* HTLS_HDR_TASK */
        { { 0x00, 0x00, 0x01, 0xf4 }, 0x000001f4u }, /* HTLC_HDR_PING */
        { { 0x00, 0x00, 0x00, 0x65 }, 0x00000065u }, /* HTLC_HDR_NEWS_GETFILE */
    };
    for (gsize i = 0; i < G_N_ELEMENTS (cases); i++) {
        guint32 host = 0xa5a5a5a5u;
        HN32 (&host, cases[i].wire);
        g_assert_cmphex (host, ==, cases[i].expected);
    }
}

/* HN32 round-trip: host → wire → host. The macro is symmetric in the
 * sense that swapping into a 4-byte buffer and back lands on the
 * original integer. */
static void
test_hn32_round_trip (void)
{
    const guint32 inputs[] = {
        0x00000000u, 0x00000001u, 0x12345678u, 0xa5a5a5a5u,
        0xdeadbeefu, 0xffffffffu, 0x80000000u, 0x00000080u,
    };
    for (gsize i = 0; i < G_N_ELEMENTS (inputs); i++) {
        guint8 wire[4] = { 0 };
        guint32 host = 0;
        HN32 (wire, &inputs[i]);
        HN32 (&host, wire);
        g_assert_cmphex (host, ==, inputs[i]);
    }
}

/* HN32 always stores big-endian regardless of host order — i.e. the
 * MSB of the integer ends up at wire[0]. This is the part of the
 * contract callers actually rely on (writing wire format). */
static void
test_hn32_writes_big_endian (void)
{
    const guint32 host = 0x12345678u;
    guint8 wire[4] = { 0 };
    HN32 (wire, &host);
    g_assert_cmphex (wire[0], ==, 0x12);
    g_assert_cmphex (wire[1], ==, 0x34);
    g_assert_cmphex (wire[2], ==, 0x56);
    g_assert_cmphex (wire[3], ==, 0x78);
}

/* ---------- HN16 ---------- */

static void
test_hn16_zero (void)
{
    const guint8 wire[2] = { 0x00, 0x00 };
    guint16 host = 0xbeef;
    HN16 (&host, wire);
    g_assert_cmphex (host, ==, 0x0000u);
}

static void
test_hn16_canonical_pattern (void)
{
    const guint8 wire[2] = { 0x12, 0x34 };
    guint16 host = 0;
    HN16 (&host, wire);
    g_assert_cmphex (host, ==, 0x1234u);
}

static void
test_hn16_max (void)
{
    const guint8 wire[2] = { 0xff, 0xff };
    guint16 host = 0;
    HN16 (&host, wire);
    g_assert_cmphex (host, ==, 0xffffu);
}

static void
test_hn16_real_protocol_constants (void)
{
    struct {
        guint8 wire[2];
        guint16 expected;
    } cases[] = {
        { { 0x00, 0x6e }, 0x006eu }, /* HTLC/S_DATA_ACCESS */
        { { 0x00, 0x64 }, 0x0064u }, /* HTLS_DATA_TASKERROR */
        { { 0x00, 0xbe }, 0x00beu }, /* HTLS_DATA_VERSION = 190 (Hotline 1.9) */
        { { 0x00, 0x96 }, 0x0096u }, /* HTLS_DATA_VERSION = 150 (Hotline 1.5) */
    };
    for (gsize i = 0; i < G_N_ELEMENTS (cases); i++) {
        guint16 host = 0xa5a5;
        HN16 (&host, cases[i].wire);
        g_assert_cmphex (host, ==, cases[i].expected);
    }
}

static void
test_hn16_round_trip (void)
{
    const guint16 inputs[] = {
        0x0000u, 0x0001u, 0x1234u, 0xa5a5u, 0xbeefu, 0xffffu, 0x8000u, 0x0080u,
    };
    for (gsize i = 0; i < G_N_ELEMENTS (inputs); i++) {
        guint8 wire[2] = { 0 };
        guint16 host = 0;
        HN16 (wire, &inputs[i]);
        HN16 (&host, wire);
        g_assert_cmphex (host, ==, inputs[i]);
    }
}

static void
test_hn16_writes_big_endian (void)
{
    const guint16 host = 0x1234u;
    guint8 wire[2] = { 0 };
    HN16 (wire, &host);
    g_assert_cmphex (wire[0], ==, 0x12);
    g_assert_cmphex (wire[1], ==, 0x34);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/byte_swap/hn32_zero", test_hn32_zero);
    g_test_add_func ("/byte_swap/hn32_one", test_hn32_one);
    g_test_add_func ("/byte_swap/hn32_max", test_hn32_max);
    g_test_add_func ("/byte_swap/hn32_canonical_pattern",
                     test_hn32_canonical_pattern);
    g_test_add_func ("/byte_swap/hn32_high_byte_only",
                     test_hn32_high_byte_only);
    g_test_add_func ("/byte_swap/hn32_low_byte_only", test_hn32_low_byte_only);
    g_test_add_func ("/byte_swap/hn32_real_protocol_constants",
                     test_hn32_real_protocol_constants);
    g_test_add_func ("/byte_swap/hn32_round_trip", test_hn32_round_trip);
    g_test_add_func ("/byte_swap/hn32_writes_big_endian",
                     test_hn32_writes_big_endian);

    g_test_add_func ("/byte_swap/hn16_zero", test_hn16_zero);
    g_test_add_func ("/byte_swap/hn16_canonical_pattern",
                     test_hn16_canonical_pattern);
    g_test_add_func ("/byte_swap/hn16_max", test_hn16_max);
    g_test_add_func ("/byte_swap/hn16_real_protocol_constants",
                     test_hn16_real_protocol_constants);
    g_test_add_func ("/byte_swap/hn16_round_trip", test_hn16_round_trip);
    g_test_add_func ("/byte_swap/hn16_writes_big_endian",
                     test_hn16_writes_big_endian);

    return g_test_run ();
}
