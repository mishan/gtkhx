/*
 * test_pict_embed — exercise hx_pict_extract_embedded against
 * synthetic PICT-like inputs.
 *
 * We don't need real QuickDraw opcodes to test the sniff path —
 * the helper is "skip the 512-byte header, scan for image format
 * magic". So each test case is just: padding + image magic +
 * payload, and we assert the returned bytes start at the right
 * offset and have the right length.
 */

#include <glib.h>
#include <string.h>

#include "pict_embed.h"

#define HEADER 512

/* Build a synthetic input: 512 zero bytes, then `prelude_len` of
 * arbitrary non-magic bytes (simulating PICT opcodes before the
 * embedded image), then `payload` of length `payload_len`. */
static GBytes *
make_pict (gsize prelude_len, const guint8 *payload, gsize payload_len)
{
    gsize total = HEADER + prelude_len + payload_len;
    guint8 *buf = g_malloc0 (total);
    /* Fill prelude with non-magic bytes (0x01 — won't collide with
     * any image signature first byte). */
    for (gsize i = 0; i < prelude_len; i++) {
        buf[HEADER + i] = 0x01;
    }
    memcpy (buf + HEADER + prelude_len, payload, payload_len);
    return g_bytes_new_take (buf, total);
}

static void
test_jpeg (void)
{
    static const guint8 jpeg_blob[]
        = { 0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0 };

    /* 24 bytes of fake PICT opcodes, then a JPEG SOI sequence. */
    GBytes *pict = make_pict (24, jpeg_blob, sizeof jpeg_blob);
    gsize size;
    const guint8 *data = g_bytes_get_data (pict, &size);
    GBytes *out = hx_pict_extract_embedded (data, size);

    g_assert_nonnull (out);

    gsize out_len;
    const guint8 *out_data = g_bytes_get_data (out, &out_len);

    /* Should start exactly at the JPEG SOI. */
    g_assert_cmpuint (out_data[0], ==, 0xFF);
    g_assert_cmpuint (out_data[1], ==, 0xD8);
    g_assert_cmpuint (out_data[2], ==, 0xFF);

    /* Length = (size - 512 prelude header - 24 fake opcode bytes). */
    g_assert_cmpuint (out_len, ==, sizeof jpeg_blob);

    g_bytes_unref (out);
    g_bytes_unref (pict);
}

static void
test_png (void)
{
    static const guint8 png_blob[]
        = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, /* …more */
            'I',  'H',  'D',  'R' };
    GBytes *pict = make_pict (8, png_blob, sizeof png_blob);
    gsize size;
    const guint8 *data = g_bytes_get_data (pict, &size);
    GBytes *out = hx_pict_extract_embedded (data, size);

    g_assert_nonnull (out);

    gsize out_len;
    const guint8 *out_data = g_bytes_get_data (out, &out_len);
    g_assert_cmpuint (out_data[0], ==, 0x89);
    g_assert_cmpuint (out_data[3], ==, 'G');
    g_assert_cmpuint (out_len, ==, sizeof png_blob);

    g_bytes_unref (out);
    g_bytes_unref (pict);
}

static void
test_gif89 (void)
{
    static const guint8 gif_blob[]
        = { 'G', 'I', 'F', '8', '9', 'a', 0x01, 0x00, 0x01, 0x00 };
    GBytes *pict = make_pict (16, gif_blob, sizeof gif_blob);
    gsize size;
    const guint8 *data = g_bytes_get_data (pict, &size);
    GBytes *out = hx_pict_extract_embedded (data, size);
    g_assert_nonnull (out);

    gsize out_len;
    const guint8 *out_data = g_bytes_get_data (out, &out_len);
    g_assert_cmpuint (out_data[0], ==, 'G');
    g_assert_cmpuint (out_data[3], ==, '8');
    g_assert_cmpuint (out_data[4], ==, '9');
    g_assert_cmpuint (out_len, ==, sizeof gif_blob);

    g_bytes_unref (out);
    g_bytes_unref (pict);
}

static void
test_tiff_little_endian (void)
{
    static const guint8 tiff_blob[]
        = { 'I', 'I', 0x2A, 0x00, 0x08, 0x00, 0x00, 0x00 };
    GBytes *pict = make_pict (8, tiff_blob, sizeof tiff_blob);
    gsize size;
    const guint8 *data = g_bytes_get_data (pict, &size);
    GBytes *out = hx_pict_extract_embedded (data, size);
    g_assert_nonnull (out);

    gsize out_len;
    const guint8 *out_data = g_bytes_get_data (out, &out_len);
    g_assert_cmpuint (out_data[0], ==, 'I');
    g_assert_cmpuint (out_data[2], ==, 0x2A);
    g_assert_cmpuint (out_len, ==, sizeof tiff_blob);

    g_bytes_unref (out);
    g_bytes_unref (pict);
}

static void
test_tiff_big_endian (void)
{
    static const guint8 tiff_blob[]
        = { 'M', 'M', 0x00, 0x2A, 0x00, 0x00, 0x00, 0x08 };
    GBytes *pict = make_pict (8, tiff_blob, sizeof tiff_blob);
    gsize size;
    const guint8 *data = g_bytes_get_data (pict, &size);
    GBytes *out = hx_pict_extract_embedded (data, size);
    g_assert_nonnull (out);
    g_bytes_unref (out);
    g_bytes_unref (pict);
}

/* PICT-shaped file with no recognised image signature anywhere
 * should return NULL — caller falls back to "couldn't decode". */
static void
test_no_embedded (void)
{
    guint8 buf[1024];
    memset (buf, 0, HEADER);          /* zero header */
    memset (buf + HEADER, 0x01, 512); /* generic opcodes, no magic */

    GBytes *out = hx_pict_extract_embedded (buf, sizeof buf);
    g_assert_null (out);
}

/* Stripped-header variant: no 512-byte zero prefix, magic is right
 * at offset 0. Some tools emit PICTs without the padding. */
static void
test_stripped_header (void)
{
    static const guint8 jpeg_blob[] = { 0xFF, 0xD8, 0xFF, 0xE0, 0, 0 };
    GBytes *out = hx_pict_extract_embedded (jpeg_blob, sizeof jpeg_blob);
    g_assert_nonnull (out);

    gsize out_len;
    const guint8 *out_data = g_bytes_get_data (out, &out_len);
    g_assert_cmpuint (out_data[0], ==, 0xFF);
    g_assert_cmpuint (out_len, ==, sizeof jpeg_blob);

    g_bytes_unref (out);
}

/* Trivially short inputs should not crash and should return NULL.
 * The header-detect path needs at least 256 bytes; below that we
 * try the no-header path. With a 4-byte input (too short for any
 * signature), we expect NULL. */
static void
test_too_short (void)
{
    const guint8 buf[4] = { 0x00, 0x01, 0x02, 0x03 };
    GBytes *out = hx_pict_extract_embedded (buf, sizeof buf);
    g_assert_null (out);

    g_assert_null (hx_pict_extract_embedded (NULL, 0));
    g_assert_null (hx_pict_extract_embedded (NULL, 100));
}

/* If multiple image signatures appear (e.g. a TIFF marker inside
 * a JPEG payload), the earliest one wins. */
static void
test_earliest_wins (void)
{
    /* PNG signature at offset 16 from the start of the post-header
     * region, then a fake TIFF marker further along. The PNG one
     * should be returned. */
    guint8 buf[HEADER + 512];
    memset (buf, 0, HEADER);
    memset (buf + HEADER, 0x01, 512);
    /* PNG at HEADER+16 */
    static const guint8 png[]
        = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
    memcpy (buf + HEADER + 16, png, sizeof png);
    /* TIFF-LE later (offset 200). */
    static const guint8 tiff[] = { 'I', 'I', 0x2A, 0x00 };
    memcpy (buf + HEADER + 200, tiff, sizeof tiff);

    GBytes *out = hx_pict_extract_embedded (buf, sizeof buf);
    g_assert_nonnull (out);
    gsize out_len;
    const guint8 *out_data = g_bytes_get_data (out, &out_len);
    g_assert_cmpuint (out_data[0], ==, 0x89);
    g_assert_cmpuint (out_data[3], ==, 'G');
    g_bytes_unref (out);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/pict_embed/jpeg", test_jpeg);
    g_test_add_func ("/pict_embed/png", test_png);
    g_test_add_func ("/pict_embed/gif89", test_gif89);
    g_test_add_func ("/pict_embed/tiff_little_endian", test_tiff_little_endian);
    g_test_add_func ("/pict_embed/tiff_big_endian", test_tiff_big_endian);
    g_test_add_func ("/pict_embed/no_embedded", test_no_embedded);
    g_test_add_func ("/pict_embed/stripped_header", test_stripped_header);
    g_test_add_func ("/pict_embed/too_short", test_too_short);
    g_test_add_func ("/pict_embed/earliest_wins", test_earliest_wins);

    return g_test_run ();
}
