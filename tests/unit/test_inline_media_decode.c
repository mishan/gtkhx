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

#include <gdk-pixbuf/gdk-pixbuf.h>
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

/* ---- Decoder layer ---- */

/* Build a small PNG/JPEG/GIF/BMP in-memory by encoding a
 * known-good pixbuf via gdk_pixbuf_save_to_buffer. Returns the
 * GBytes; caller unrefs. NULL on failure. */
static GBytes *
encode_test_image (const char *format, int w, int h)
{
    GdkPixbuf *pix
        = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, w, h);
    if (!pix) {
        return NULL;
    }
    /* Fill the pixbuf with a recognisable test pattern (red). */
    gdk_pixbuf_fill (pix, 0xFF0000FF);

    gchar *buf = NULL;
    gsize bufsz = 0;
    GError *err = NULL;
    gboolean ok = gdk_pixbuf_save_to_buffer (pix, &buf, &bufsz, format,
                                             &err, NULL);
    g_object_unref (pix);
    if (!ok || !buf) {
        g_clear_error (&err);
        return NULL;
    }
    return g_bytes_new_take (buf, bufsz);
}

static void
test_decode_accepts_well_formed_png (void)
{
    GBytes *bytes = encode_test_image ("png", 64, 48);
    g_assert_nonnull (bytes);

    HxInlineMediaCaps caps = {0};
    gsize len;
    const guint8 *raw = g_bytes_get_data (bytes, &len);
    HxInlineMediaDecoded r = inline_media_decode (raw, len, &caps);
    g_bytes_unref (bytes);

    g_assert_nonnull (r.texture);
    g_assert_cmpuint (r.error_code, ==, 0);
    g_assert_cmpstr (r.canonical_mime, ==, "image/png");
    g_assert_cmpint (r.sniffed_format, ==, INLINE_MEDIA_FORMAT_PNG);
    g_object_unref (r.texture);
}

static void
test_decode_accepts_well_formed_jpeg (void)
{
    GBytes *bytes = encode_test_image ("jpeg", 64, 48);
    /* JPEG support depends on the gdk-pixbuf loader being present;
	 * if save fails the format isn't available — skip rather
	 * than fail. */
    if (!bytes) {
        g_test_skip ("JPEG encoder unavailable");
        return;
    }

    HxInlineMediaCaps caps = {0};
    gsize len;
    const guint8 *raw = g_bytes_get_data (bytes, &len);
    HxInlineMediaDecoded r = inline_media_decode (raw, len, &caps);
    g_bytes_unref (bytes);

    g_assert_nonnull (r.texture);
    g_assert_cmpuint (r.error_code, ==, 0);
    g_assert_cmpstr (r.canonical_mime, ==, "image/jpeg");
    g_assert_cmpint (r.sniffed_format, ==, INLINE_MEDIA_FORMAT_JPEG);
    g_object_unref (r.texture);
}

static void
test_decode_accepts_well_formed_gif (void)
{
    /* gdk_pixbuf_save_to_buffer ("gif", ...) isn't available by
	 * default — GIF write-support depends on the libgif loader's
	 * encoder which isn't always built. Use a hand-crafted
	 * minimal valid 1×1 transparent GIF89a (43 bytes) so the
	 * test is self-contained and fails loudly per the
	 * no-silent-skips rule. The byte sequence is the canonical
	 * minimal GIF89a documented in the GIF89a spec appendix —
	 * header + screen descriptor + GCE + image descriptor + LZW
	 * + trailer. */
    static const guint8 minimal_gif_1x1[] = {
        0x47, 0x49, 0x46, 0x38, 0x39, 0x61, /* "GIF89a" */
        0x01, 0x00, 0x01, 0x00,             /* width=1 height=1 */
        0x80, 0x00, 0x00,                   /* global color table flag, bg, aspect */
        0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, /* 2-color global palette */
        0x21, 0xF9, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, /* GCE block */
        0x2C, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, /* image desc */
        0x02, 0x02, 0x44, 0x01, 0x00,       /* LZW-encoded pixel */
        0x3B,                                /* GIF trailer */
    };

    HxInlineMediaCaps caps = {0};
    HxInlineMediaDecoded r = inline_media_decode (
        minimal_gif_1x1, sizeof (minimal_gif_1x1), &caps);

    g_assert_nonnull (r.texture);
    g_assert_cmpuint (r.error_code, ==, 0);
    g_assert_cmpstr (r.canonical_mime, ==, "image/gif");
    g_assert_cmpint (r.sniffed_format, ==, INLINE_MEDIA_FORMAT_GIF);
    g_object_unref (r.texture);
}

static void
test_decode_byte_cap_rejects_oversized (void)
{
    GBytes *bytes = encode_test_image ("png", 16, 16);
    g_assert_nonnull (bytes);

    /* Cap is well below the encoded size — should reject. */
    HxInlineMediaCaps caps = {.max_bytes = 8};
    gsize len;
    const guint8 *raw = g_bytes_get_data (bytes, &len);
    HxInlineMediaDecoded r = inline_media_decode (raw, len, &caps);
    g_bytes_unref (bytes);

    g_assert_null (r.texture);
    g_assert_cmpuint (r.error_code, ==, 1); /* PayloadTooLarge */
}

static void
test_decode_dimension_cap_rejects_oversized (void)
{
    /* 64×64 PNG with a very low dimension cap should be rejected
	 * at the size-prepared gate. */
    GBytes *bytes = encode_test_image ("png", 64, 64);
    g_assert_nonnull (bytes);

    HxInlineMediaCaps caps = {.max_dimension = 16};
    gsize len;
    const guint8 *raw = g_bytes_get_data (bytes, &len);
    HxInlineMediaDecoded r = inline_media_decode (raw, len, &caps);
    g_bytes_unref (bytes);

    g_assert_null (r.texture);
    g_assert_cmpuint (r.error_code, ==, 1);
}

static void
test_decode_pixel_count_cap_rejects_oversized (void)
{
    /* 64×64 = 4096 pixels; cap at 100 pixels → reject. */
    GBytes *bytes = encode_test_image ("png", 64, 64);
    g_assert_nonnull (bytes);

    HxInlineMediaCaps caps = {
        .max_dimension = 4096,
        .max_pixels = 100,
    };
    gsize len;
    const guint8 *raw = g_bytes_get_data (bytes, &len);
    HxInlineMediaDecoded r = inline_media_decode (raw, len, &caps);
    g_bytes_unref (bytes);

    g_assert_null (r.texture);
    g_assert_cmpuint (r.error_code, ==, 1);
}

static void
test_decode_rejects_svg (void)
{
    const guint8 svg[] = "<?xml version=\"1.0\"?><svg></svg>";
    HxInlineMediaCaps caps = {0};
    HxInlineMediaDecoded r
        = inline_media_decode (svg, sizeof (svg) - 1, &caps);
    g_assert_null (r.texture);
    g_assert_cmpuint (r.error_code, ==, 2); /* UnsupportedFormat */
    g_assert_cmpint (r.sniffed_format, ==, INLINE_MEDIA_FORMAT_SVG);
}

static void
test_decode_rejects_webp (void)
{
    const guint8 webp[]
        = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'};
    HxInlineMediaCaps caps = {0};
    HxInlineMediaDecoded r
        = inline_media_decode (webp, sizeof (webp), &caps);
    g_assert_null (r.texture);
    g_assert_cmpuint (r.error_code, ==, 2);
    g_assert_cmpint (r.sniffed_format, ==, INLINE_MEDIA_FORMAT_WEBP);
}

static void
test_decode_rejects_random_bytes (void)
{
    const guint8 garbage[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01, 0x02};
    HxInlineMediaCaps caps = {0};
    HxInlineMediaDecoded r
        = inline_media_decode (garbage, sizeof (garbage), &caps);
    g_assert_null (r.texture);
    g_assert_cmpuint (r.error_code, ==, 2);
    g_assert_cmpint (r.sniffed_format, ==, INLINE_MEDIA_FORMAT_UNKNOWN);
}

static void
test_decode_rejects_empty (void)
{
    HxInlineMediaCaps caps = {0};
    HxInlineMediaDecoded r
        = inline_media_decode ((const guint8 *) "", 0, &caps);
    g_assert_null (r.texture);
    g_assert_cmpuint (r.error_code, ==, 2);
}

static void
test_decode_truncated_png_fails_cleanly (void)
{
    /* Take a real PNG and chop off the last few bytes — the
	 * loader should fail at close, mapped to UnsupportedFormat.
	 * Decoder must NOT crash or return a partial texture. */
    GBytes *bytes = encode_test_image ("png", 32, 32);
    g_assert_nonnull (bytes);
    gsize full_len;
    const guint8 *raw = g_bytes_get_data (bytes, &full_len);
    g_assert_cmpuint (full_len, >, 16);

    HxInlineMediaCaps caps = {0};
    HxInlineMediaDecoded r = inline_media_decode (raw, full_len - 16, &caps);
    g_bytes_unref (bytes);

    g_assert_null (r.texture);
    g_assert_cmpuint (r.error_code, ==, 2);
    g_assert_cmpint (r.sniffed_format, ==, INLINE_MEDIA_FORMAT_PNG);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    /* gdk_pixbuf alone is enough for sniff + decode; GTK isn't
	 * required (no widget surface). */

    /* Sniff cases — pure C, no global init needed but g_test
	 * still routes the assertions. */
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

    /* Decode cases — exercise the full pipeline. */
    g_test_add_func ("/inline_media_decode/decode/png",
                     test_decode_accepts_well_formed_png);
    g_test_add_func ("/inline_media_decode/decode/jpeg",
                     test_decode_accepts_well_formed_jpeg);
    g_test_add_func ("/inline_media_decode/decode/gif",
                     test_decode_accepts_well_formed_gif);
    g_test_add_func ("/inline_media_decode/decode/byte_cap",
                     test_decode_byte_cap_rejects_oversized);
    g_test_add_func ("/inline_media_decode/decode/dim_cap",
                     test_decode_dimension_cap_rejects_oversized);
    g_test_add_func ("/inline_media_decode/decode/pixel_cap",
                     test_decode_pixel_count_cap_rejects_oversized);
    g_test_add_func ("/inline_media_decode/decode/reject_svg",
                     test_decode_rejects_svg);
    g_test_add_func ("/inline_media_decode/decode/reject_webp",
                     test_decode_rejects_webp);
    g_test_add_func ("/inline_media_decode/decode/reject_garbage",
                     test_decode_rejects_random_bytes);
    g_test_add_func ("/inline_media_decode/decode/reject_empty",
                     test_decode_rejects_empty);
    g_test_add_func ("/inline_media_decode/decode/truncated_png",
                     test_decode_truncated_png_fails_cleanly);

    return g_test_run ();
}
