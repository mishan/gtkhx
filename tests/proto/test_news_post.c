/*
 * tests/proto/test_news_post.c — drive hx_news_post_walk against
 * canned HTLS_HDR_NEWS_POST bodies.
 *
 * The handler's contract: for every HTLS_DATA_NEWS chunk in the
 * message, sanitise the bytes (CR2LF + strip_ansi + NUL) and call
 * the user callback. Non-NEWS chunks are skipped silently. Returns
 * the count of NEWS chunks processed.
 *
 * Phase 5 cleanup context: this replaced an earlier handler that
 * maintained a file-scope news_buf+news_len accumulator the emit
 * code never read — a slow memory leak with a misleading shape.
 * See proto_helpers.h for the full backstory. The tests below pin
 * down the simpler "sanitise per chunk, emit per chunk" semantics
 * the cleanup committed to.
 */

#include "config.h"
#include <string.h>
#include <glib.h>
#include "protocol.h"
#include "hotline.h"
#include "proto_helpers.h"
#include "wire_fixture.h"

/* ---------- Recording callback ---------- */

struct call_record {
    gsize len;
    gchar *bytes; /* g_strndup'd copy, not the cb's pointer */
};

struct recorder {
    GArray *calls; /* of struct call_record */
};

static void
recorder_init (struct recorder *r)
{
    r->calls = g_array_new (FALSE, FALSE, sizeof (struct call_record));
}

static void
recorder_free (struct recorder *r)
{
    for (guint i = 0; i < r->calls->len; i++) {
        struct call_record *c
            = &g_array_index (r->calls, struct call_record, i);
        g_free (c->bytes);
    }
    g_array_free (r->calls, TRUE);
}

static void
record_cb (void *user, const char *bytes, gsize len)
{
    struct recorder *r = user;
    struct call_record c = {
        .len = len,
        .bytes = g_strndup (bytes, len),
    };
    g_array_append_val (r->calls, c);
}

/* ---------- Single chunk ---------- */

static void
test_news_post_single_chunk (void)
{
    struct htlc_conn htlc;
    const char *body = "hello, news";

    wire_fixture_init (&htlc, HTLS_HDR_NEWS_POST, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_NEWS, strlen (body), body);

    struct recorder r;
    recorder_init (&r);
    int n = hx_news_post_walk (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, record_cb, &r);
    g_assert_cmpint (n, ==, 1);
    g_assert_cmpuint (r.calls->len, ==, 1);

    struct call_record *c = &g_array_index (r.calls, struct call_record, 0);
    g_assert_cmpstr (c->bytes, ==, "hello, news");
    g_assert_cmpuint (c->len, ==, strlen (body));

    recorder_free (&r);
    wire_fixture_free (&htlc);
}

/* ---------- Multiple NEWS chunks in one message ---------- */

static void
test_news_post_multiple_chunks_emit_in_order (void)
{
    struct htlc_conn htlc;
    const char *first = "first post";
    const char *second = "second post";
    const char *third = "third post";

    wire_fixture_init (&htlc, HTLS_HDR_NEWS_POST, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_NEWS, strlen (first), first);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_NEWS, strlen (second), second);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_NEWS, strlen (third), third);

    struct recorder r;
    recorder_init (&r);
    int n = hx_news_post_walk (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, record_cb, &r);
    g_assert_cmpint (n, ==, 3);
    g_assert_cmpuint (r.calls->len, ==, 3);

    g_assert_cmpstr (g_array_index (r.calls, struct call_record, 0).bytes, ==,
                     "first post");
    g_assert_cmpstr (g_array_index (r.calls, struct call_record, 1).bytes, ==,
                     "second post");
    g_assert_cmpstr (g_array_index (r.calls, struct call_record, 2).bytes, ==,
                     "third post");

    recorder_free (&r);
    wire_fixture_free (&htlc);
}

/* ---------- Sanitisation: CR→LF and strip_ansi ---------- */

static void
test_news_post_converts_cr_to_lf (void)
{
    struct htlc_conn htlc;
    const char *body = "line one\rline two\rline three";
    wire_fixture_init (&htlc, HTLS_HDR_NEWS_POST, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_NEWS, strlen (body), body);

    struct recorder r;
    recorder_init (&r);
    hx_news_post_walk (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, record_cb, &r);
    g_assert_cmpstr (g_array_index (r.calls, struct call_record, 0).bytes, ==,
                     "line one\nline two\nline three");

    recorder_free (&r);
    wire_fixture_free (&htlc);
}

static void
test_news_post_strips_ansi (void)
{
    struct htlc_conn htlc;
    const char body[] = "\x1b[31malert\x1b[0m post";
    wire_fixture_init (&htlc, HTLS_HDR_NEWS_POST, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_NEWS, sizeof (body) - 1, body);

    struct recorder r;
    recorder_init (&r);
    hx_news_post_walk (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, record_cb, &r);
    g_assert_cmpstr (g_array_index (r.calls, struct call_record, 0).bytes, ==,
                     "[[31malert[[0m post");

    recorder_free (&r);
    wire_fixture_free (&htlc);
}

/* ---------- Non-NEWS chunks skipped ---------- */

static void
test_news_post_skips_non_news_chunks (void)
{
    struct htlc_conn htlc;
    const guint16 some_uid = g_htons(5);
    const char *body = "real post";

    wire_fixture_init (&htlc, HTLS_HDR_NEWS_POST, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_UID, sizeof (some_uid), &some_uid);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_NEWS, strlen (body), body);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_CHAT_ID, sizeof (some_uid),
                            &some_uid);

    struct recorder r;
    recorder_init (&r);
    int n = hx_news_post_walk (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, record_cb, &r);
    g_assert_cmpint (n, ==, 1); /* only the one NEWS chunk */
    g_assert_cmpuint (r.calls->len, ==, 1);
    g_assert_cmpstr (g_array_index (r.calls, struct call_record, 0).bytes, ==,
                     "real post");

    recorder_free (&r);
    wire_fixture_free (&htlc);
}

/* ---------- Empty message ---------- */

static void
test_news_post_empty_message_returns_zero (void)
{
    struct htlc_conn htlc;
    wire_fixture_init (&htlc, HTLS_HDR_NEWS_POST, 1, 0);

    struct recorder r;
    recorder_init (&r);
    int n = hx_news_post_walk (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, record_cb, &r);
    g_assert_cmpint (n, ==, 0);
    g_assert_cmpuint (r.calls->len, ==, 0);

    recorder_free (&r);
    wire_fixture_free (&htlc);
}

/* ---------- Empty NEWS chunk ----------
 *
 * Pin down: a zero-length NEWS chunk is still a chunk, the cb
 * fires once with len=0 and an empty (NUL-terminated) string. The
 * earlier dh_start fix (changing >= to > on the loop guard) was
 * needed to make this reliable.
 */
static void
test_news_post_empty_news_chunk (void)
{
    struct htlc_conn htlc;
    wire_fixture_init (&htlc, HTLS_HDR_NEWS_POST, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_NEWS, 0, NULL);

    struct recorder r;
    recorder_init (&r);
    int n = hx_news_post_walk (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, record_cb, &r);
    g_assert_cmpint (n, ==, 1);
    g_assert_cmpuint (r.calls->len, ==, 1);
    g_assert_cmpuint (g_array_index (r.calls, struct call_record, 0).len, ==,
                      0);
    g_assert_cmpstr (g_array_index (r.calls, struct call_record, 0).bytes, ==,
                     "");

    recorder_free (&r);
    wire_fixture_free (&htlc);
}

/* ---------- NULL callback is allowed ---------- */

static void
test_news_post_null_cb_still_counts (void)
{
    struct htlc_conn htlc;
    const char *a = "one";
    const char *b = "two";
    wire_fixture_init (&htlc, HTLS_HDR_NEWS_POST, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_NEWS, strlen (a), a);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_NEWS, strlen (b), b);

    int n = hx_news_post_walk (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, NULL, NULL);
    g_assert_cmpint (n, ==, 2);

    wire_fixture_free (&htlc);
}

/* ---------- The cleanup's reason for being ----------
 *
 * The previous handler accumulated content across calls into a
 * global news_buf, in reverse chronological order. Across two
 * consecutive walks of the same message, the new walker must NOT
 * exhibit any cross-call accumulation effect — each call sees only
 * its own message's chunks.
 */
static void
test_news_post_no_cross_call_accumulation (void)
{
    /* First call: post "A". */
    struct htlc_conn htlc1;
    wire_fixture_init (&htlc1, HTLS_HDR_NEWS_POST, 1, 0);
    wire_fixture_add_chunk (&htlc1, HTLS_DATA_NEWS, 1, "A");

    struct recorder r1;
    recorder_init (&r1);
    hx_news_post_walk (hx_test_in(&htlc1)->buf, hx_test_in(&htlc1)->pos, record_cb, &r1);
    g_assert_cmpuint (r1.calls->len, ==, 1);
    g_assert_cmpuint (g_array_index (r1.calls, struct call_record, 0).len, ==,
                      1);

    wire_fixture_free (&htlc1);
    recorder_free (&r1);

    /* Second call: post "BB" (different length, different content).
	 * The walker MUST NOT carry over the "A" from the first call. */
    struct htlc_conn htlc2;
    wire_fixture_init (&htlc2, HTLS_HDR_NEWS_POST, 1, 0);
    wire_fixture_add_chunk (&htlc2, HTLS_DATA_NEWS, 2, "BB");

    struct recorder r2;
    recorder_init (&r2);
    hx_news_post_walk (hx_test_in(&htlc2)->buf, hx_test_in(&htlc2)->pos, record_cb, &r2);
    g_assert_cmpuint (r2.calls->len, ==, 1);
    g_assert_cmpuint (g_array_index (r2.calls, struct call_record, 0).len, ==,
                      2);
    g_assert_cmpstr (g_array_index (r2.calls, struct call_record, 0).bytes, ==,
                     "BB");

    wire_fixture_free (&htlc2);
    recorder_free (&r2);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/proto/news_post/single_chunk",
                     test_news_post_single_chunk);
    g_test_add_func ("/proto/news_post/multiple_chunks_emit_in_order",
                     test_news_post_multiple_chunks_emit_in_order);

    g_test_add_func ("/proto/news_post/converts_cr_to_lf",
                     test_news_post_converts_cr_to_lf);
    g_test_add_func ("/proto/news_post/strips_ansi",
                     test_news_post_strips_ansi);

    g_test_add_func ("/proto/news_post/skips_non_news_chunks",
                     test_news_post_skips_non_news_chunks);

    g_test_add_func ("/proto/news_post/empty_message_returns_zero",
                     test_news_post_empty_message_returns_zero);
    g_test_add_func ("/proto/news_post/empty_news_chunk",
                     test_news_post_empty_news_chunk);

    g_test_add_func ("/proto/news_post/null_cb_still_counts",
                     test_news_post_null_cb_still_counts);

    g_test_add_func ("/proto/news_post/no_cross_call_accumulation",
                     test_news_post_no_cross_call_accumulation);

    return g_test_run ();
}
