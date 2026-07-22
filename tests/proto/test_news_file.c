/*
 * tests/proto/test_news_file.c — drive hx_news_file_extract against
 * canned HTLS_HDR_TASK messages.
 *
 * The news file is the one-shot reply to HTLC_HDR_NEWS_GETFILE —
 * the server sends back the entire news file body in a single
 * HTLS_DATA_NEWS chunk inside an HTLS_HDR_TASK reply (the request's
 * trans ID is what tells us "this task reply is for our news
 * fetch"). The extractor sanitises (CR2LF +
 * strip_ansi) into the caller's buffer, NUL-terminates, and
 * truncates if the body's larger than what was provided.
 *
 * Order sensitivity: if the response carries multiple HTLS_DATA_NEWS
 * chunks (servers don't, but the protocol doesn't forbid it), the
 * first one wins. The test below pins this down.
 */

#include "config.h"
#include <string.h>
#include <netinet/in.h>
#include <glib.h>
#include "protocol.h"
#include "hotline.h"
#include "proto_helpers.h"
#include "wire_fixture.h"

static void
test_news_file_extracts_body (void)
{
    struct htlc_conn htlc;
    const char *body = "Welcome to the news.\r\rRule 1: be nice.";
    wire_fixture_init (&htlc, HTLS_HDR_TASK, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_NEWS, strlen (body), body);

    char out[256];
    gsize out_len = 0;
    g_assert_true (hx_news_file_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, out, sizeof (out), &out_len));
    g_assert_cmpstr (out, ==, "Welcome to the news.\n\nRule 1: be nice.");
    g_assert_cmpuint (out_len, ==, strlen (body));

    wire_fixture_free (&htlc);
}

static void
test_news_file_strips_ansi (void)
{
    struct htlc_conn htlc;
    const char body[] = "\x1b[31mALERT\x1b[0m\rmessage";
    wire_fixture_init (&htlc, HTLS_HDR_TASK, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_NEWS, sizeof (body) - 1, body);

    char out[128];
    g_assert_true (hx_news_file_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, out, sizeof (out), NULL));
    g_assert_cmpstr (out, ==, "[[31mALERT[[0m\nmessage");

    wire_fixture_free (&htlc);
}

static void
test_news_file_truncates_to_buffer (void)
{
    struct htlc_conn htlc;
    guint8 big[8000];
    memset (big, 'N', sizeof (big));
    wire_fixture_init (&htlc, HTLS_HDR_TASK, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_NEWS, sizeof (big), big);

    char out[128];
    gsize out_len = 0;
    g_assert_true (hx_news_file_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, out, sizeof (out), &out_len));
    g_assert_cmpuint (out_len, ==, sizeof (out) - 1);
    g_assert_cmpuint (strlen (out), ==, sizeof (out) - 1);
    g_assert_cmphex (out[sizeof (out) - 1], ==, '\0');

    wire_fixture_free (&htlc);
}

static void
test_news_file_no_news_chunk_returns_false (void)
{
    struct htlc_conn htlc;
    const guint16 uid_wire = htons (5);
    wire_fixture_init (&htlc, HTLS_HDR_TASK, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_UID, sizeof (uid_wire), &uid_wire);

    char out[64] = "untouched";
    g_assert_false (hx_news_file_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, out, sizeof (out), NULL));
    g_assert_cmpstr (out, ==, "untouched");

    wire_fixture_free (&htlc);
}

static void
test_news_file_empty_message_returns_false (void)
{
    struct htlc_conn htlc;
    wire_fixture_init (&htlc, HTLS_HDR_TASK, 1, 0);

    char out[64];
    g_assert_false (hx_news_file_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, out, sizeof (out), NULL));

    wire_fixture_free (&htlc);
}

/* ---------- First-NEWS-chunk-wins contract ---------- */

static void
test_news_file_first_news_chunk_wins (void)
{
    struct htlc_conn htlc;
    const char *first = "first body";
    const char *second = "second body";
    wire_fixture_init (&htlc, HTLS_HDR_TASK, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_NEWS, strlen (first), first);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_NEWS, strlen (second), second);

    char out[64];
    g_assert_true (hx_news_file_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, out, sizeof (out), NULL));
    g_assert_cmpstr (out, ==, "first body");

    wire_fixture_free (&htlc);
}

/* Skip-unrelated-chunks regression: the previous dh_start macro
 * would have hung on a non-NEWS chunk before the NEWS chunk. The
 * fix made `continue` correct. */
static void
test_news_file_skips_unrelated_chunks_before_news (void)
{
    struct htlc_conn htlc;
    const guint16 uid_wire = htons (5);
    const guint32 cid_wire = htonl (3);
    const char *body = "the news";
    wire_fixture_init (&htlc, HTLS_HDR_TASK, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_UID, sizeof (uid_wire), &uid_wire);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_CHAT_ID, sizeof (cid_wire),
                            &cid_wire);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_NEWS, strlen (body), body);

    char out[64];
    g_assert_true (hx_news_file_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, out, sizeof (out), NULL));
    g_assert_cmpstr (out, ==, "the news");

    wire_fixture_free (&htlc);
}

/* ---------- API edge cases ---------- */

static void
test_news_file_null_out_returns_false (void)
{
    struct htlc_conn htlc;
    const char *body = "anything";
    wire_fixture_init (&htlc, HTLS_HDR_TASK, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_NEWS, strlen (body), body);

    g_assert_false (hx_news_file_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, NULL, 64, NULL));

    wire_fixture_free (&htlc);
}

static void
test_news_file_zero_buffer_returns_false (void)
{
    struct htlc_conn htlc;
    const char *body = "anything";
    wire_fixture_init (&htlc, HTLS_HDR_TASK, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_NEWS, strlen (body), body);

    char out[64];
    g_assert_false (hx_news_file_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, out, 0, NULL));

    wire_fixture_free (&htlc);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/proto/news_file/extracts_body",
                     test_news_file_extracts_body);
    g_test_add_func ("/proto/news_file/strips_ansi",
                     test_news_file_strips_ansi);
    g_test_add_func ("/proto/news_file/truncates_to_buffer",
                     test_news_file_truncates_to_buffer);

    g_test_add_func ("/proto/news_file/no_news_chunk_returns_false",
                     test_news_file_no_news_chunk_returns_false);
    g_test_add_func ("/proto/news_file/empty_message_returns_false",
                     test_news_file_empty_message_returns_false);

    g_test_add_func ("/proto/news_file/first_news_chunk_wins",
                     test_news_file_first_news_chunk_wins);
    g_test_add_func ("/proto/news_file/skips_unrelated_chunks_before_news",
                     test_news_file_skips_unrelated_chunks_before_news);

    g_test_add_func ("/proto/news_file/null_out_returns_false",
                     test_news_file_null_out_returns_false);
    g_test_add_func ("/proto/news_file/zero_buffer_returns_false",
                     test_news_file_zero_buffer_returns_false);

    return g_test_run ();
}
