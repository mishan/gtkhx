/*
 * tests/proto/test_banner.c — drive hx_banner_extract against canned
 * HTLS_HDR_BANNER wire bodies.
 *
 * The banner reply carries one mandatory and one optional chunk:
 *
 *   HTLS_DATA_BANNER_TYPE (0x0098) — exactly 4 bytes (4-char type
 *                                    code, space-right-padded). Codes
 *                                    seen in the wild: "URL ", "JPEG",
 *                                    "GIFf", "PICT". The protocol pins
 *                                    the length at 4 — anything else
 *                                    is malformed.
 *   HTLS_DATA_BANNER_URL  (0x0099) — only present for URL-mode
 *                                    banners. File-backed JPEG/GIF
 *                                    banners omit this and the client
 *                                    is expected to follow up with
 *                                    HTLC_HDR_DOWNLOAD_BANNER on the
 *                                    HTXF subchannel.
 *
 * Tests pin:
 *   - Mandatory 4-byte type code, NUL-terminated, space-padded for
 *     short codes — banner.c::toolbar_banner_load_type expects exactly
 *     this shape so it can compare against "URL ".
 *   - has_url flag is FALSE when the URL chunk is absent (file-mode
 *     banners).
 *   - Wrong-sized type chunk (anything ≠ 4 bytes) → FALSE return so
 *     the surrounding handler can ignore the broadcast.
 *   - URL truncation at the buffer cap (1024 bytes per the helper).
 */

#include "config.h"
#include <string.h>
#include <glib.h>
#include "protocol.h"
#include "hotline.h"
#include "proto_helpers.h"
#include "wire_fixture.h"

/* ---------- Happy paths ---------- */

static void
test_banner_url_mode (void)
{
    /* The full common case: 4-byte "URL " type plus an explicit
     * URL. banner.c picks libsoup up and fetches the image. */
    struct htlc_conn htlc;
    const char *url = "https://example.com/banner.jpg";

    wire_fixture_init (&htlc, HTLS_HDR_BANNER, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_BANNER_TYPE, 4, "URL ");
    wire_fixture_add_chunk (&htlc, HTLS_DATA_BANNER_URL, strlen (url), url);

    struct hx_banner_msg b;
    g_assert_true (hx_banner_extract (hx_test_in (&htlc)->buf,
                                      hx_test_in (&htlc)->pos, &b));
    g_assert_cmpstr (b.type, ==, "URL ");
    g_assert_true (b.has_url);
    g_assert_cmpstr (b.url, ==, url);
    g_assert_cmpuint (b.url_len, ==, strlen (url));

    wire_fixture_free (&htlc);
}

static void
test_banner_jpeg_mode (void)
{
    /* JPEG file-mode: type is present, URL is absent. The client
     * then follows up with HTLC_HDR_DOWNLOAD_BANNER on the HTXF
     * subchannel; this helper just reports the missing URL. */
    struct htlc_conn htlc;
    wire_fixture_init (&htlc, HTLS_HDR_BANNER, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_BANNER_TYPE, 4, "JPEG");

    struct hx_banner_msg b;
    g_assert_true (hx_banner_extract (hx_test_in (&htlc)->buf,
                                      hx_test_in (&htlc)->pos, &b));
    g_assert_cmpstr (b.type, ==, "JPEG");
    g_assert_false (b.has_url);
    g_assert_cmpstr (b.url, ==, "");
    g_assert_cmpuint (b.url_len, ==, 0);

    wire_fixture_free (&htlc);
}

static void
test_banner_gif_mode (void)
{
    /* GIFf is the other file-mode code mhxd emits — same shape as
     * JPEG, just a different glyph for the banner.c dispatch. */
    struct htlc_conn htlc;
    wire_fixture_init (&htlc, HTLS_HDR_BANNER, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_BANNER_TYPE, 4, "GIFf");

    struct hx_banner_msg b;
    g_assert_true (hx_banner_extract (hx_test_in (&htlc)->buf,
                                      hx_test_in (&htlc)->pos, &b));
    g_assert_cmpstr (b.type, ==, "GIFf");
    g_assert_false (b.has_url);

    wire_fixture_free (&htlc);
}

static void
test_banner_pict_mode (void)
{
    /* PICT is the legacy Mac classic 'PICT' image format. Original
     * Hotline Communications banners. We don't actually render these
     * (no Pict decoder), but the extractor must still report the
     * type so the surrounding handler can decide. */
    struct htlc_conn htlc;
    wire_fixture_init (&htlc, HTLS_HDR_BANNER, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_BANNER_TYPE, 4, "PICT");

    struct hx_banner_msg b;
    g_assert_true (hx_banner_extract (hx_test_in (&htlc)->buf,
                                      hx_test_in (&htlc)->pos, &b));
    g_assert_cmpstr (b.type, ==, "PICT");

    wire_fixture_free (&htlc);
}

/* ---------- Chunk order independence ----------
 *
 * Wire chunk order isn't load-bearing for this opcode; the parser
 * dispatches off chunk type. Make sure URL-before-TYPE also works.
 */
static void
test_banner_chunk_order_url_first (void)
{
    struct htlc_conn htlc;
    const char *url = "http://hotline.example/banner.png";
    wire_fixture_init (&htlc, HTLS_HDR_BANNER, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_BANNER_URL, strlen (url), url);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_BANNER_TYPE, 4, "URL ");

    struct hx_banner_msg b;
    g_assert_true (hx_banner_extract (hx_test_in (&htlc)->buf,
                                      hx_test_in (&htlc)->pos, &b));
    g_assert_cmpstr (b.type, ==, "URL ");
    g_assert_true (b.has_url);
    g_assert_cmpstr (b.url, ==, url);

    wire_fixture_free (&htlc);
}

/* ---------- Malformed type chunk ----------
 *
 * The protocol pins HTLS_DATA_BANNER_TYPE at exactly 4 bytes
 * (space-padded for shorter codes). A 3-byte or 5-byte payload is
 * a server bug; we refuse the whole message rather than risk
 * comparing partial bytes against the type-string codes downstream.
 */
static void
test_banner_short_type_rejected (void)
{
    struct htlc_conn htlc;
    wire_fixture_init (&htlc, HTLS_HDR_BANNER, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_BANNER_TYPE, 3, "URL");

    struct hx_banner_msg b;
    g_assert_false (hx_banner_extract (hx_test_in (&htlc)->buf,
                                       hx_test_in (&htlc)->pos, &b));

    wire_fixture_free (&htlc);
}

static void
test_banner_long_type_rejected (void)
{
    struct htlc_conn htlc;
    wire_fixture_init (&htlc, HTLS_HDR_BANNER, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_BANNER_TYPE, 5, "JPEG ");

    struct hx_banner_msg b;
    g_assert_false (hx_banner_extract (hx_test_in (&htlc)->buf,
                                       hx_test_in (&htlc)->pos, &b));

    wire_fixture_free (&htlc);
}

static void
test_banner_empty_type_rejected (void)
{
    struct htlc_conn htlc;
    wire_fixture_init (&htlc, HTLS_HDR_BANNER, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_BANNER_TYPE, 0, NULL);

    struct hx_banner_msg b;
    g_assert_false (hx_banner_extract (hx_test_in (&htlc)->buf,
                                       hx_test_in (&htlc)->pos, &b));

    wire_fixture_free (&htlc);
}

/* If the type chunk is malformed but a valid URL chunk also appears,
 * we still refuse — better a missed banner than a half-parsed one
 * that might dispatch on garbage type bytes. */
static void
test_banner_bad_type_with_valid_url_still_refused (void)
{
    struct htlc_conn htlc;
    const char *url = "https://example.com/x.jpg";
    wire_fixture_init (&htlc, HTLS_HDR_BANNER, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_BANNER_TYPE, 2, "OK");
    wire_fixture_add_chunk (&htlc, HTLS_DATA_BANNER_URL, strlen (url), url);

    struct hx_banner_msg b;
    g_assert_false (hx_banner_extract (hx_test_in (&htlc)->buf,
                                       hx_test_in (&htlc)->pos, &b));

    wire_fixture_free (&htlc);
}

/* ---------- Missing chunks ---------- */

static void
test_banner_missing_type_rejected (void)
{
    /* URL-only message with no TYPE chunk: extract reports FALSE.
     * Caller's surrounding handler then ignores the broadcast. */
    struct htlc_conn htlc;
    const char *url = "https://example.com/x.jpg";
    wire_fixture_init (&htlc, HTLS_HDR_BANNER, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_BANNER_URL, strlen (url), url);

    struct hx_banner_msg b;
    g_assert_false (hx_banner_extract (hx_test_in (&htlc)->buf,
                                       hx_test_in (&htlc)->pos, &b));

    wire_fixture_free (&htlc);
}

static void
test_banner_no_chunks_rejected (void)
{
    /* A header-only banner frame: no chunks at all. */
    struct htlc_conn htlc;
    wire_fixture_init (&htlc, HTLS_HDR_BANNER, 1, 0);

    struct hx_banner_msg b;
    g_assert_false (hx_banner_extract (hx_test_in (&htlc)->buf,
                                       hx_test_in (&htlc)->pos, &b));

    wire_fixture_free (&htlc);
}

/* ---------- URL truncation ----------
 *
 * hx_banner_msg.url is 1025 bytes (1024 + NUL). A wire URL bigger
 * than that gets clipped at the buffer cap. URLs that long aren't
 * realistic in practice, but a defensive bound matters: a buggy
 * server could otherwise blow our buffer.
 */
static void
test_banner_url_truncated_at_buffer_cap (void)
{
    struct htlc_conn htlc;
    /* Wire chunk len is u16, so 2000 fits. Buffer cap is 1024. */
    guint8 long_url[2000];
    memset (long_url, 'a', sizeof (long_url));
    wire_fixture_init (&htlc, HTLS_HDR_BANNER, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_BANNER_TYPE, 4, "URL ");
    wire_fixture_add_chunk (&htlc, HTLS_DATA_BANNER_URL, sizeof (long_url),
                            long_url);

    struct hx_banner_msg b;
    g_assert_true (hx_banner_extract (hx_test_in (&htlc)->buf,
                                      hx_test_in (&htlc)->pos, &b));
    g_assert_true (b.has_url);
    g_assert_cmpuint (b.url_len, ==, 1024);
    g_assert_cmpuint (strlen (b.url), ==, 1024);

    wire_fixture_free (&htlc);
}

static void
test_banner_url_at_exact_buffer_cap (void)
{
    /* Boundary case: URL bytes == buffer cap (1024). Must round-trip
     * cleanly without losing the last byte to NUL placement. */
    struct htlc_conn htlc;
    guint8 url[1024];
    memset (url, 'b', sizeof (url));
    wire_fixture_init (&htlc, HTLS_HDR_BANNER, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_BANNER_TYPE, 4, "URL ");
    wire_fixture_add_chunk (&htlc, HTLS_DATA_BANNER_URL, sizeof (url), url);

    struct hx_banner_msg b;
    g_assert_true (hx_banner_extract (hx_test_in (&htlc)->buf,
                                      hx_test_in (&htlc)->pos, &b));
    g_assert_cmpuint (b.url_len, ==, 1024);
    for (gsize i = 0; i < 1024; i++) {
        g_assert_cmpint (b.url[i], ==, 'b');
    }
    g_assert_cmpint (b.url[1024], ==, '\0');

    wire_fixture_free (&htlc);
}

/* ---------- Empty URL chunk ---------- */

static void
test_banner_empty_url_still_flags_has_url (void)
{
    /* Zero-length URL chunk is weird but well-formed. has_url
     * should still be TRUE (the chunk was present); url_len is 0. */
    struct htlc_conn htlc;
    wire_fixture_init (&htlc, HTLS_HDR_BANNER, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_BANNER_TYPE, 4, "URL ");
    wire_fixture_add_chunk (&htlc, HTLS_DATA_BANNER_URL, 0, NULL);

    struct hx_banner_msg b;
    g_assert_true (hx_banner_extract (hx_test_in (&htlc)->buf,
                                      hx_test_in (&htlc)->pos, &b));
    g_assert_true (b.has_url);
    g_assert_cmpstr (b.url, ==, "");
    g_assert_cmpuint (b.url_len, ==, 0);

    wire_fixture_free (&htlc);
}

/* ---------- API edge cases ---------- */

static void
test_banner_null_out_returns_false (void)
{
    struct htlc_conn htlc;
    wire_fixture_init (&htlc, HTLS_HDR_BANNER, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_BANNER_TYPE, 4, "URL ");

    g_assert_false (hx_banner_extract (hx_test_in (&htlc)->buf,
                                       hx_test_in (&htlc)->pos, NULL));

    wire_fixture_free (&htlc);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/proto/banner/url_mode", test_banner_url_mode);
    g_test_add_func ("/proto/banner/jpeg_mode", test_banner_jpeg_mode);
    g_test_add_func ("/proto/banner/gif_mode", test_banner_gif_mode);
    g_test_add_func ("/proto/banner/pict_mode", test_banner_pict_mode);

    g_test_add_func ("/proto/banner/chunk_order_url_first",
                     test_banner_chunk_order_url_first);

    g_test_add_func ("/proto/banner/short_type_rejected",
                     test_banner_short_type_rejected);
    g_test_add_func ("/proto/banner/long_type_rejected",
                     test_banner_long_type_rejected);
    g_test_add_func ("/proto/banner/empty_type_rejected",
                     test_banner_empty_type_rejected);
    g_test_add_func ("/proto/banner/bad_type_with_valid_url_still_refused",
                     test_banner_bad_type_with_valid_url_still_refused);

    g_test_add_func ("/proto/banner/missing_type_rejected",
                     test_banner_missing_type_rejected);
    g_test_add_func ("/proto/banner/no_chunks_rejected",
                     test_banner_no_chunks_rejected);

    g_test_add_func ("/proto/banner/url_truncated_at_buffer_cap",
                     test_banner_url_truncated_at_buffer_cap);
    g_test_add_func ("/proto/banner/url_at_exact_buffer_cap",
                     test_banner_url_at_exact_buffer_cap);

    g_test_add_func ("/proto/banner/empty_url_still_flags_has_url",
                     test_banner_empty_url_still_flags_has_url);

    g_test_add_func ("/proto/banner/null_out_returns_false",
                     test_banner_null_out_returns_false);

    return g_test_run ();
}
