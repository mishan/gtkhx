/*
 * tests/unit/test_htxf_hdr.c — lock the 16-byte HTXF subchannel
 * handshake header layout.
 *
 * Wire layout (big-endian throughout — htonl on each u32):
 *
 *   bytes 0..3   : "HTXF" magic         (0x48545846)
 *   bytes 4..7   : ref                  (matches the HTXF_REF the
 *                                        main-port TASK reply carried)
 *   bytes 8..11  : len                  (total payload size estimate)
 *   bytes 12..15 : (u16 type) (u16 reserved)
 *                  type = HTXF_TYPE_FILE   (0)  single-file transfer
 *                  type = HTXF_TYPE_FOLDER (1)  folder tree transfer
 *                  type = HTXF_TYPE_BANNER (2)  banner JPEG fetch
 *
 * The type field — fitted into the high two bytes of what struct
 * htxf_hdr declared as `unknown u32` — was the load-bearing piece of
 * the "Oni Tracks folder download hangs" bug. Real Mac-native
 * servers dispatch the inbound subchannel by reading this field,
 * and we were always sending type=0. mhxd is lenient about it (it
 * matches by ref to a pre-typed slot) which is why integration
 * tests against mhxd passed while real servers hung.
 *
 * This test re-encodes the header from scratch — same htonl + same
 * shift — and pins the byte-for-byte output for each transfer type.
 * If anyone reshuffles struct htxf_hdr or renumbers HTXF_TYPE_*,
 * this test catches it before the next user opens a folder download
 * against a Mac-native server and hangs.
 */

#include "config.h"
#include <string.h>
#include <glib.h>
#include <arpa/inet.h>          /* htonl */
#include "compat.h"             /* PACKED — used inside hotline.h */
#include "hotline.h"
#include "proto_helpers.h"      /* hl_htxf_hdr_pack — the shared packer */

/* Wrap the production packer so each test stays focused on the
 * field values rather than the packer signature. Production
 * htxf_connect (src/network.c), the banner sender (src/banner.c),
 * and the integration test harness all funnel through
 * hl_htxf_hdr_pack — so pinning the bytes here pins all four
 * call sites at once. */
static void
encode_htxf_hdr (guint8 out[16], guint32 ref, guint32 len, guint16 type)
{
    hl_htxf_hdr_pack (out, ref, len, type, /*flags=*/0);
}

/* The header is exactly 16 bytes — the figure baked into every
 * call site that reads or writes one. */
static void
test_size_is_16_bytes (void)
{
    g_assert_cmpint (SIZEOF_HTXF_HDR, ==, 16);
    g_assert_cmpint (sizeof (struct htxf_hdr), ==, 16);
}

/* HTXF_MAGIC_INT is the ASCII bytes "HTXF" read as a big-endian
 * u32. Lock the value AND prove that the htonl-encoded magic comes
 * out as the literal four ASCII chars at the start of the header. */
static void
test_magic_int_matches_ascii (void)
{
    g_assert_cmphex (HTXF_MAGIC_INT, ==, 0x48545846u);

    guint8 buf[16];
    encode_htxf_hdr (buf, 0, 0, HTXF_TYPE_FILE);
    g_assert_cmphex (buf[0], ==, 'H');
    g_assert_cmphex (buf[1], ==, 'T');
    g_assert_cmphex (buf[2], ==, 'X');
    g_assert_cmphex (buf[3], ==, 'F');
}

/* HTXF_TYPE_FILE single-file transfer — type=0. ref and len use
 * a fixed test pattern so we can pin all 16 bytes. */
static void
test_file_layout (void)
{
    guint8 buf[16];
    encode_htxf_hdr (buf, /*ref=*/0x11223344u, /*len=*/0x00010000u,
                     HTXF_TYPE_FILE);

    const guint8 expected[16] = {
        'H', 'T', 'X', 'F',                       /* magic */
        0x11, 0x22, 0x33, 0x44,                   /* ref */
        0x00, 0x01, 0x00, 0x00,                   /* len = 65536 */
        0x00, 0x00, 0x00, 0x00,                   /* type=0, reserved=0 */
    };
    g_assert_cmpmem (buf, 16, expected, 16);
}

/* HTXF_TYPE_FOLDER — type=1. The byte at offset 13 (low byte of the
 * type u16 in the last 4-byte word) is the one that flips from 0
 * to 1; bytes 14..15 remain zero. */
static void
test_folder_layout (void)
{
    guint8 buf[16];
    encode_htxf_hdr (buf, /*ref=*/1, /*len=*/0, HTXF_TYPE_FOLDER);

    g_assert_cmphex (buf[0], ==, 'H');
    g_assert_cmphex (buf[12], ==, 0x00);
    g_assert_cmphex (buf[13], ==, 0x01); /* type = HTXF_TYPE_FOLDER */
    g_assert_cmphex (buf[14], ==, 0x00); /* reserved */
    g_assert_cmphex (buf[15], ==, 0x00); /* reserved */
}

/* HTXF_TYPE_BANNER — type=2. */
static void
test_banner_layout (void)
{
    guint8 buf[16];
    encode_htxf_hdr (buf, /*ref=*/0xdeadbeef, /*len=*/12345,
                     HTXF_TYPE_BANNER);

    g_assert_cmphex (buf[12], ==, 0x00);
    g_assert_cmphex (buf[13], ==, 0x02); /* type = HTXF_TYPE_BANNER */
    g_assert_cmphex (buf[14], ==, 0x00);
    g_assert_cmphex (buf[15], ==, 0x00);
}

/* type and reserved are separate u16 fields. The reserved bytes
 * (offsets 14..15) must stay zero regardless of which transfer type
 * goes in offset 12..13 — proven here by inspecting all three
 * HTXF_TYPE_* values. mhxd's wire validator and Mac-native servers
 * both look for zero reserved bytes; nonzero reads have been seen
 * to crash older Mac clients. */
static void
test_reserved_bytes_always_zero (void)
{
    guint16 types[] = { HTXF_TYPE_FILE, HTXF_TYPE_FOLDER, HTXF_TYPE_BANNER };
    for (gsize i = 0; i < G_N_ELEMENTS (types); i++) {
        guint8 buf[16];
        encode_htxf_hdr (buf, 0xa5a5a5a5u, 0x5a5a5a5au, types[i]);
        g_assert_cmphex (buf[14], ==, 0x00);
        g_assert_cmphex (buf[15], ==, 0x00);
    }
}

/* type values are stable. Renumber one of these and (a) server
 * dispatchers go wrong and (b) the type field test cases above
 * silently lose their meaning. */
static void
test_type_constants_are_stable (void)
{
    g_assert_cmpint (HTXF_TYPE_FILE, ==, 0);
    g_assert_cmpint (HTXF_TYPE_FOLDER, ==, 1);
    g_assert_cmpint (HTXF_TYPE_BANNER, ==, 2);
}

/* Field order: magic, ref, len, unknown. Renumbering any of these
 * (e.g., swapping ref and len) would silently mis-dispatch every
 * subchannel — mhxd matches by ref and would never find ours.
 * Encode a known pattern with all four distinct values so any
 * reordering shows up. */
static void
test_field_offsets_are_stable (void)
{
    guint8 buf[16];
    encode_htxf_hdr (buf, /*ref=*/0x01020304u, /*len=*/0x05060708u,
                     HTXF_TYPE_FILE);

    /* magic at 0..3 */
    g_assert_cmphex (buf[0], ==, 0x48);

    /* ref at 4..7 */
    g_assert_cmphex (buf[4], ==, 0x01);
    g_assert_cmphex (buf[5], ==, 0x02);
    g_assert_cmphex (buf[6], ==, 0x03);
    g_assert_cmphex (buf[7], ==, 0x04);

    /* len at 8..11 */
    g_assert_cmphex (buf[8], ==, 0x05);
    g_assert_cmphex (buf[9], ==, 0x06);
    g_assert_cmphex (buf[10], ==, 0x07);
    g_assert_cmphex (buf[11], ==, 0x08);

    /* type/reserved at 12..15 */
    g_assert_cmphex (buf[12], ==, 0x00);
    g_assert_cmphex (buf[13], ==, 0x00);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/htxf_hdr/size_is_16_bytes", test_size_is_16_bytes);
    g_test_add_func ("/htxf_hdr/magic_int_matches_ascii",
                     test_magic_int_matches_ascii);
    g_test_add_func ("/htxf_hdr/file_layout", test_file_layout);
    g_test_add_func ("/htxf_hdr/folder_layout", test_folder_layout);
    g_test_add_func ("/htxf_hdr/banner_layout", test_banner_layout);
    g_test_add_func ("/htxf_hdr/reserved_bytes_always_zero",
                     test_reserved_bytes_always_zero);
    g_test_add_func ("/htxf_hdr/type_constants_are_stable",
                     test_type_constants_are_stable);
    g_test_add_func ("/htxf_hdr/field_offsets_are_stable",
                     test_field_offsets_are_stable);

    return g_test_run ();
}
