/*
 * tests/unit/test_hl_code.c — verify the Hotline LOGIN / PASSWORD
 * obfuscation transform. Strictly speaking it isn't encryption —
 * it's a bitwise NOT per byte (XOR with 0xff) — but the wire calls
 * it that, and the contract that matters here is:
 *
 *   1. encode and decode are the same operation (a round-trip
 *      lands on the original bytes).
 *   2. each output byte is the bitwise complement of the
 *      corresponding input byte (no carry, no chaining).
 *   3. zero-length input is a no-op.
 *   4. in-place transforms (dst == src) work.
 *   5. the dst buffer can be larger than len without spilling.
 *
 * If any of these break, every login attempt and every admin user-
 * edit ciphers to the wrong byte stream and the server rejects it.
 * The integration tests would catch the symptom; this one localises
 * the diagnosis to a 5-line function.
 */

#include "config.h"
#include <string.h>
#include <glib.h>
#include "hl_code.h"

/* Each byte ends up complemented. The protocol's "encrypted-login"
 * representation of an ASCII login is literally ~login[i] for each
 * byte. */
static void
test_basic_complement (void)
{
    const guint8 plain[] = { 'g', 'u', 'e', 's', 't' };
    guint8 cipher[5];

    hl_code (cipher, plain, sizeof plain);

    g_assert_cmphex (cipher[0], ==, (guint8) ~'g');
    g_assert_cmphex (cipher[1], ==, (guint8) ~'u');
    g_assert_cmphex (cipher[2], ==, (guint8) ~'e');
    g_assert_cmphex (cipher[3], ==, (guint8) ~'s');
    g_assert_cmphex (cipher[4], ==, (guint8) ~'t');
}

/* Round-tripping is the identity. Run a variety of byte patterns
 * (some chosen to land on tricky 0x00 / 0xff edges after one pass)
 * through encode then decode and verify the original buffer comes
 * out. */
static void
test_round_trip (void)
{
    const guint8 *inputs[] = {
        (const guint8 *)"",                  /* empty                  */
        (const guint8 *)"a",                 /* single byte            */
        (const guint8 *)"guest",             /* the classic login      */
        (const guint8 *)"P@ssw0rd!",         /* mixed punct + digits   */
        (const guint8 *)"\x00\x00\x00\x00",  /* all zero               */
        (const guint8 *)"\xff\xff\xff\xff",  /* all ones               */
        (const guint8 *)"\x80\x81\x82\x83",  /* high-bit set           */
    };
    const size_t lens[] = { 0, 1, 5, 9, 4, 4, 4 };

    for (gsize i = 0; i < G_N_ELEMENTS (inputs); i++) {
        guint8 cipher[32];
        guint8 plain[32];

        memset (cipher, 0xa5, sizeof cipher);
        memset (plain,  0xa5, sizeof plain);

        hl_code (cipher, inputs[i], lens[i]);
        hl_code (plain,  cipher,    lens[i]);

        g_assert_cmpint (memcmp (plain, inputs[i], lens[i]), ==, 0);
    }
}

/* len == 0 must be a no-op. The wrappers in rcv.c / commands.c pass
 * the chunk's declared length straight through, and zero-length
 * LOGIN / PASSWORD chunks are legal (anonymous-guest convention on
 * some servers). */
static void
test_zero_length_is_noop (void)
{
    guint8 dst[4] = { 0xaa, 0xbb, 0xcc, 0xdd };
    const guint8 src[4] = { 0x00, 0x00, 0x00, 0x00 };

    hl_code (dst, src, 0);

    g_assert_cmphex (dst[0], ==, 0xaa);
    g_assert_cmphex (dst[1], ==, 0xbb);
    g_assert_cmphex (dst[2], ==, 0xcc);
    g_assert_cmphex (dst[3], ==, 0xdd);
}

/* In-place transforms (dst == src) are used by usermod.c when it
 * encodes the admin-supplied login/password buffers in-place before
 * shipping. Verify that's safe — the inner loop does dst++, src++
 * with no temporary, so an in-place call had better leave the
 * buffer correctly complemented end-to-end. */
static void
test_in_place_transform (void)
{
    guint8 buf[8] = { 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h' };

    hl_code (buf, buf, sizeof buf);

    g_assert_cmphex (buf[0], ==, (guint8) ~'a');
    g_assert_cmphex (buf[7], ==, (guint8) ~'h');

    /* Round-trip back, in place, and we should be where we started. */
    hl_code (buf, buf, sizeof buf);

    g_assert_cmphex (buf[0], ==, 'a');
    g_assert_cmphex (buf[1], ==, 'b');
    g_assert_cmphex (buf[7], ==, 'h');
}

/* The dst buffer can be larger than len. Make sure trailing bytes
 * past len don't get scribbled — would matter if hl_code grew a
 * NUL-pad loop or a memset call. */
static void
test_does_not_overrun_dst (void)
{
    guint8 dst[8] = { 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55 };
    const guint8 src[4] = { 0x12, 0x34, 0x56, 0x78 };

    hl_code (dst, src, 4);

    g_assert_cmphex (dst[0], ==, 0xed); /* ~0x12 */
    g_assert_cmphex (dst[1], ==, 0xcb); /* ~0x34 */
    g_assert_cmphex (dst[2], ==, 0xa9); /* ~0x56 */
    g_assert_cmphex (dst[3], ==, 0x87); /* ~0x78 */
    /* Untouched trailers. */
    g_assert_cmphex (dst[4], ==, 0x55);
    g_assert_cmphex (dst[5], ==, 0x55);
    g_assert_cmphex (dst[6], ==, 0x55);
    g_assert_cmphex (dst[7], ==, 0x55);
}

/* Spot-check a known-from-traces input/output pair. The "guest"
 * login encodes to the byte sequence below on the wire — confirmed
 * by staring at mhxd proto traces. If we regress this exactly,
 * every login breaks. */
static void
test_guest_login_wire_pattern (void)
{
    const guint8 plain[]  = { 'g', 'u', 'e', 's', 't' };
    const guint8 expect[] = { 0x98, 0x8a, 0x9a, 0x8c, 0x8b };
    guint8 cipher[5];

    hl_code (cipher, plain, sizeof plain);

    g_assert_cmpint (memcmp (cipher, expect, sizeof expect), ==, 0);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/hl_code/basic_complement", test_basic_complement);
    g_test_add_func ("/hl_code/round_trip", test_round_trip);
    g_test_add_func ("/hl_code/zero_length_is_noop", test_zero_length_is_noop);
    g_test_add_func ("/hl_code/in_place_transform", test_in_place_transform);
    g_test_add_func ("/hl_code/does_not_overrun_dst",
                     test_does_not_overrun_dst);
    g_test_add_func ("/hl_code/guest_login_wire_pattern",
                     test_guest_login_wire_pattern);

    return g_test_run ();
}
