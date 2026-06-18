/*
 * tests/unit/test_inline_media_decode.c — Phase 9.B bounded
 * decoder.
 *
 * Two layers under test:
 *
 *   sniff — magic-byte detection over hand-crafted prefixes
 *           covering allowlisted (JPEG / PNG / GIF) and
 *           blocked (SVG / WebP / AVIF / HEIC / TIFF / ICO /
 *           BMP) formats, plus the unknown / short / empty
 *           edge cases. The sniff function is pure C with no
 *           GLib or GTK dependency — these tests verify the
 *           rejection-at-the-door semantics that defend the
 *           rest of the decode pipeline.
 *
 *   decode — full pipeline (sniff + byte cap + size-prepared
 *            gate + GdkPixbufLoader). Uses dynamically-generated
 *            test pixbufs serialised to PNG / GIF / JPEG, which
 *            keeps the test data small and avoids checking
 *            binary fixtures into the repo. Verifies:
 *              - well-formed PNG / GIF / JPEG decode to a
 *                non-NULL GdkTexture with the spec-canonical
 *                MIME.
 *              - byte cap rejects oversized input with
 *                error_code = 1 (PayloadTooLarge).
 *              - dimension cap rejects oversized image with
 *                error_code = 1.
 *              - SVG / WebP-magic / random bytes reject with
 *                error_code = 2 (UnsupportedFormat).
 *              - empty input rejects with error_code = 2.
 *
 * The decoder layer needs a GdkPixbuf context (which requires
 * `g_test_init` + the gdk pixbuf loaders registered; gtk_init
 * is not required for pixbuf decoding). The test wraps the
 * decode cases in `g_test_add_func` so a CI failure tells
 * which scenario regressed.
 */

#include <glib.h>
#include <gtk/gtk.h>
#include <stdint.h>
#include <string.h>

#include "inline_media_decode.h"

/* ---- Sniff layer ---- */

static void
test_sniff_jpeg (void)
{
    static const guint8 jpg[] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00};
    g_assert_cmpint (inline_media_sniff (jpg, sizeof (jpg)), ==,
                     INLINE_MEDIA_FORMAT_JPEG);
    g_assert_true (inline_media_format_is_allowed (INLINE_MEDIA_FORMAT_JPEG));
    g_assert_cmpstr (
        inline_media_format_to_mime (INLINE_MEDIA_FORMAT_JPEG), ==,
        "image/jpeg");
}

static void
test_sniff_png (void)
{
    static const guint8 png[]
        = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00};
    g_assert_cmpint (inline_media_sniff (png, sizeof (png)), ==,
                     INLINE_MEDIA_FORMAT_PNG);
    g_assert_cmpstr (
        inline_media_format_to_mime (INLINE_MEDIA_FORMAT_PNG), ==,
        "image/png");
}

static void
test_sniff_gif (void)
{
    static const guint8 gif87[] = {'G', 'I', 'F', '8', '7', 'a'};
    static const guint8 gif89[] = {'G', 'I', 'F', '8', '9', 'a'};
    g_assert_cmpint (inline_media_sniff (gif87, sizeof (gif87)), ==,
                     INLINE_MEDIA_FORMAT_GIF);
    g_assert_cmpint (inline_media_sniff (gif89, sizeof (gif89)), ==,
                     INLINE_MEDIA_FORMAT_GIF);
}

static void
test_sniff_webp_rejected (void)
{
    /* RIFF.... WEBP — 12 bytes minimum to detect. */
    static const guint8 webp[]
        = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'};
    g_assert_cmpint (inline_media_sniff (webp, sizeof (webp)), ==,
                     INLINE_MEDIA_FORMAT_WEBP);
    g_assert_false (inline_media_format_is_allowed (INLINE_MEDIA_FORMAT_WEBP));
}

static void
test_sniff_avif_rejected (void)
{
    /* 0..3 size, 4..7 "ftyp", 8..11 brand "avif". */
    static const guint8 avif[]
        = {0, 0, 0, 32, 'f', 't', 'y', 'p', 'a', 'v', 'i', 'f'};
    g_assert_cmpint (inline_media_sniff (avif, sizeof (avif)), ==,
                     INLINE_MEDIA_FORMAT_AVIF);
    g_assert_false (inline_media_format_is_allowed (INLINE_MEDIA_FORMAT_AVIF));
}

static void
test_sniff_heic_rejected (void)
{
    static const guint8 heic[]
        = {0, 0, 0, 32, 'f', 't', 'y', 'p', 'h', 'e', 'i', 'c'};
    g_assert_cmpint (inline_media_sniff (heic, sizeof (heic)), ==,
                     INLINE_MEDIA_FORMAT_HEIC);
    /* mif1 brand also lands on HEIC. */
    static const guint8 mif1[]
        = {0, 0, 0, 32, 'f', 't', 'y', 'p', 'm', 'i', 'f', '1'};
    g_assert_cmpint (inline_media_sniff (mif1, sizeof (mif1)), ==,
                     INLINE_MEDIA_FORMAT_HEIC);
    g_assert_false (inline_media_format_is_allowed (INLINE_MEDIA_FORMAT_HEIC));
}

static void
test_sniff_svg_rejected (void)
{
    static const char xml[] = "<?xml version=\"1.0\"?><svg></svg>";
    g_assert_cmpint (
        inline_media_sniff ((const guint8 *) xml, sizeof (xml) - 1), ==,
        INLINE_MEDIA_FORMAT_SVG);
    static const char direct[] = "<svg xmlns=\"...\">";
    g_assert_cmpint (
        inline_media_sniff ((const guint8 *) direct, sizeof (direct) - 1),
        ==, INLINE_MEDIA_FORMAT_SVG);
    /* With leading whitespace + BOM. */
    static const guint8 bom_svg[]
        = {0xEF, 0xBB, 0xBF, ' ', '\n', '<', 's', 'v', 'g'};
    g_assert_cmpint (inline_media_sniff (bom_svg, sizeof (bom_svg)), ==,
                     INLINE_MEDIA_FORMAT_SVG);
    g_assert_false (inline_media_format_is_allowed (INLINE_MEDIA_FORMAT_SVG));
}

static void
test_sniff_tiff_rejected (void)
{
    static const guint8 little[] = {0x49, 0x49, 0x2A, 0x00};
    static const guint8 big[] = {0x4D, 0x4D, 0x00, 0x2A};
    g_assert_cmpint (inline_media_sniff (little, sizeof (little)), ==,
                     INLINE_MEDIA_FORMAT_TIFF);
    g_assert_cmpint (inline_media_sniff (big, sizeof (big)), ==,
                     INLINE_MEDIA_FORMAT_TIFF);
    g_assert_false (inline_media_format_is_allowed (INLINE_MEDIA_FORMAT_TIFF));
}

static void
test_sniff_bmp_rejected (void)
{
    static const guint8 bmp[] = {'B', 'M', 0, 0, 0, 0};
    g_assert_cmpint (inline_media_sniff (bmp, sizeof (bmp)), ==,
                     INLINE_MEDIA_FORMAT_BMP);
    g_assert_false (inline_media_format_is_allowed (INLINE_MEDIA_FORMAT_BMP));
}

static void
test_sniff_ico_rejected (void)
{
    static const guint8 ico[] = {0x00, 0x00, 0x01, 0x00};
    g_assert_cmpint (inline_media_sniff (ico, sizeof (ico)), ==,
                     INLINE_MEDIA_FORMAT_ICO);
    g_assert_false (inline_media_format_is_allowed (INLINE_MEDIA_FORMAT_ICO));
}

static void
test_sniff_unknown (void)
{
    static const guint8 garbage[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x00};
    g_assert_cmpint (inline_media_sniff (garbage, sizeof (garbage)), ==,
                     INLINE_MEDIA_FORMAT_UNKNOWN);
    g_assert_null (inline_media_format_to_mime (INLINE_MEDIA_FORMAT_UNKNOWN));
}

static void
test_sniff_empty (void)
{
    g_assert_cmpint (inline_media_sniff (NULL, 0), ==,
                     INLINE_MEDIA_FORMAT_UNKNOWN);
    g_assert_cmpint (inline_media_sniff ((const guint8 *) "", 0), ==,
                     INLINE_MEDIA_FORMAT_UNKNOWN);
}

static void
test_sniff_short_input_doesnt_overread (void)
{
    /* Single byte — not enough to match any signature. The
	 * point of the test is that the sniff returns UNKNOWN
	 * without reading past `len`. ASan would catch an actual
	 * OOB read; this asserts the documented behaviour. */
    static const guint8 single[] = {0xFF};
    g_assert_cmpint (inline_media_sniff (single, 1), ==,
                     INLINE_MEDIA_FORMAT_UNKNOWN);
    /* JPEG needs 3 bytes; PNG needs 8. Verify each prefix
	 * cut-off returns UNKNOWN. */
    static const guint8 png_prefix[]
        = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A};
    g_assert_cmpint (inline_media_sniff (png_prefix, sizeof (png_prefix)),
                     ==, INLINE_MEDIA_FORMAT_UNKNOWN);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    /* G.2 (glycin migration): the decode-layer test cases moved
	 * to the Rust crate (rust/crates/hx-image-decode). Sniff
	 * coverage stays here so the C-side FFI shim is exercised
	 * end-to-end via the same fixtures the legacy C impl had. */
    g_test_add_func ("/inline_media_decode/sniff/jpeg", test_sniff_jpeg);
    g_test_add_func ("/inline_media_decode/sniff/png", test_sniff_png);
    g_test_add_func ("/inline_media_decode/sniff/gif", test_sniff_gif);
    g_test_add_func ("/inline_media_decode/sniff/webp_rejected",
                     test_sniff_webp_rejected);
    g_test_add_func ("/inline_media_decode/sniff/avif_rejected",
                     test_sniff_avif_rejected);
    g_test_add_func ("/inline_media_decode/sniff/heic_rejected",
                     test_sniff_heic_rejected);
    g_test_add_func ("/inline_media_decode/sniff/svg_rejected",
                     test_sniff_svg_rejected);
    g_test_add_func ("/inline_media_decode/sniff/tiff_rejected",
                     test_sniff_tiff_rejected);
    g_test_add_func ("/inline_media_decode/sniff/bmp_rejected",
                     test_sniff_bmp_rejected);
    g_test_add_func ("/inline_media_decode/sniff/ico_rejected",
                     test_sniff_ico_rejected);
    g_test_add_func ("/inline_media_decode/sniff/unknown",
                     test_sniff_unknown);
    g_test_add_func ("/inline_media_decode/sniff/empty",
                     test_sniff_empty);
    g_test_add_func ("/inline_media_decode/sniff/short",
                     test_sniff_short_input_doesnt_overread);

    return g_test_run ();
}
