/*
 * tests/proto/test_chat.c — the chat line helpers the view runs over a
 * received line: hx_chat_split_nick_body, which finds the speaker's
 * nick in a formatted line, and hx_highlight_match, which decides
 * whether a line mentions one of the user's highlight words.
 */

#include "config.h"
#include <string.h>
#include <glib.h>
#include "proto_helpers.h"

/* ---------- hx_chat_split_nick_body ---------- */

static void
test_chat_split_typical_line (void)
{
    gsize n_off, n_len, b_off, b_len;
    const char *line = " misha:  hello world";
    g_assert_true (hx_chat_split_nick_body (line, strlen (line), &n_off, &n_len,
                                            &b_off, &b_len));
    g_assert_cmpuint (n_off, ==, 1);
    g_assert_cmpuint (n_len, ==, 5);
    g_assert_cmpuint (b_off, ==, 9);
    g_assert_cmpuint (b_len, ==, strlen ("hello world"));
    g_assert_true (memcmp (line + n_off, "misha", n_len) == 0);
    g_assert_true (memcmp (line + b_off, "hello world", b_len) == 0);
}

static void
test_chat_split_no_padding (void)
{
    gsize n_off, n_len, b_off, b_len;
    const char *line = "alice: hi";
    g_assert_true (hx_chat_split_nick_body (line, strlen (line), &n_off, &n_len,
                                            &b_off, &b_len));
    g_assert_cmpuint (n_off, ==, 0);
    g_assert_cmpuint (n_len, ==, 5);
    g_assert_cmpuint (b_off, ==, 7);
    g_assert_cmpuint (b_len, ==, 2);
}

static void
test_chat_split_name_with_spaces (void)
{
    gsize n_off, n_len, b_off, b_len;
    const char *line = "  Alice Cooper:  rock";
    g_assert_true (hx_chat_split_nick_body (line, strlen (line), &n_off, &n_len,
                                            &b_off, &b_len));
    g_assert_cmpuint (n_len, ==, strlen ("Alice Cooper"));
    g_assert_true (memcmp (line + n_off, "Alice Cooper", n_len) == 0);
}

static void
test_chat_split_extra_colons_in_body (void)
{
    gsize n_off, n_len, b_off, b_len;
    const char *line = " bob:  check http://example.com";
    g_assert_true (hx_chat_split_nick_body (line, strlen (line), &n_off, &n_len,
                                            &b_off, &b_len));
    g_assert_cmpuint (n_len, ==, 3);
    g_assert_true (memcmp (line + n_off, "bob", n_len) == 0);
    g_assert_true (memcmp (line + b_off, "check http://example.com", b_len)
                   == 0);
}

static void
test_chat_split_rejects_long_pre_colon (void)
{
    /* Pre-colon portion exceeds the 31-byte Hotline nick cap.
     * Lines like a sentence that happens to contain a colon
     * deep in the prose ("the part you were curious about:
     * here") shouldn't be misread as nick + body — the colon
     * is a regular punctuation mark, not the chat separator. */
    gsize n_off, n_len, b_off, b_len;
    const char *line = " the long thing I wanted to mention: here is the body";
    g_assert_false (hx_chat_split_nick_body (line, strlen (line), &n_off,
                                             &n_len, &b_off, &b_len));
}

static void
test_chat_split_rejects_no_colon (void)
{
    gsize n_off, n_len, b_off, b_len;
    const char *line = "  *** charlie waves";
    g_assert_false (hx_chat_split_nick_body (line, strlen (line), &n_off,
                                             &n_len, &b_off, &b_len));
}

static void
test_chat_split_rejects_all_whitespace (void)
{
    gsize n_off, n_len, b_off, b_len;
    const char *line = "      ";
    g_assert_false (hx_chat_split_nick_body (line, strlen (line), &n_off,
                                             &n_len, &b_off, &b_len));
}

static void
test_chat_split_rejects_empty_nick (void)
{
    gsize n_off, n_len, b_off, b_len;
    const char *line = " : oops";
    g_assert_false (hx_chat_split_nick_body (line, strlen (line), &n_off,
                                             &n_len, &b_off, &b_len));
}

static void
test_chat_split_empty_body (void)
{
    gsize n_off, n_len, b_off, b_len;
    const char *line = " misha:  ";
    g_assert_true (hx_chat_split_nick_body (line, strlen (line), &n_off, &n_len,
                                            &b_off, &b_len));
    g_assert_cmpuint (n_len, ==, 5);
    g_assert_cmpuint (b_len, ==, 0);
}

/* ---------- hx_highlight_match ---------- */

static void
test_highlight_match_simple (void)
{
    const char *words[] = { "misha", NULL };
    const char *body = "hey misha did you see this";
    g_assert_true (hx_highlight_match (body, strlen (body), words));
}

static void
test_highlight_match_case_insensitive (void)
{
    const char *words[] = { "Misha", NULL };
    const char *body = "MISHA wake up";
    g_assert_true (hx_highlight_match (body, strlen (body), words));
}

static void
test_highlight_match_word_boundary (void)
{
    /* "mishap" must NOT match "misha". */
    const char *words[] = { "misha", NULL };
    const char *body = "what a mishap that was";
    g_assert_false (hx_highlight_match (body, strlen (body), words));
}

static void
test_highlight_match_at_buffer_start (void)
{
    const char *words[] = { "misha", NULL };
    const char *body = "misha look here";
    g_assert_true (hx_highlight_match (body, strlen (body), words));
}

static void
test_highlight_match_at_buffer_end (void)
{
    const char *words[] = { "misha", NULL };
    const char *body = "hey misha";
    g_assert_true (hx_highlight_match (body, strlen (body), words));
}

static void
test_highlight_match_punctuation_boundary (void)
{
    /* Parens/commas are non-word bytes — should boundary-match. */
    const char *words[] = { "misha", NULL };
    const char *body = "(misha), did you?";
    g_assert_true (hx_highlight_match (body, strlen (body), words));
}

static void
test_highlight_match_multiple_words (void)
{
    const char *words[] = { "alice", "bob", "carol", NULL };
    const char *body = "carol just signed in";
    g_assert_true (hx_highlight_match (body, strlen (body), words));
}

static void
test_highlight_match_skips_empty_words (void)
{
    /* g_strsplit can leave empty entries from "a,,b" — make
     * sure those don't false-match anywhere. */
    const char *words[] = { "", "a", "", NULL };
    const char *body = "nothing here";
    g_assert_false (hx_highlight_match (body, strlen (body), words));
}

static void
test_highlight_match_no_words_returns_false (void)
{
    const char *words[] = { NULL };
    const char *body = "some text";
    g_assert_false (hx_highlight_match (body, strlen (body), words));
}

static void
test_highlight_match_empty_body (void)
{
    const char *words[] = { "misha", NULL };
    g_assert_false (hx_highlight_match ("", 0, words));
    g_assert_false (hx_highlight_match (NULL, 0, words));
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/proto/chat/split_typical_line",
                     test_chat_split_typical_line);
    g_test_add_func ("/proto/chat/split_no_padding",
                     test_chat_split_no_padding);
    g_test_add_func ("/proto/chat/split_name_with_spaces",
                     test_chat_split_name_with_spaces);
    g_test_add_func ("/proto/chat/split_extra_colons_in_body",
                     test_chat_split_extra_colons_in_body);
    g_test_add_func ("/proto/chat/split_rejects_long_pre_colon",
                     test_chat_split_rejects_long_pre_colon);
    g_test_add_func ("/proto/chat/split_rejects_no_colon",
                     test_chat_split_rejects_no_colon);
    g_test_add_func ("/proto/chat/split_rejects_all_whitespace",
                     test_chat_split_rejects_all_whitespace);
    g_test_add_func ("/proto/chat/split_rejects_empty_nick",
                     test_chat_split_rejects_empty_nick);
    g_test_add_func ("/proto/chat/split_empty_body",
                     test_chat_split_empty_body);

    g_test_add_func ("/proto/chat/highlight_match_simple",
                     test_highlight_match_simple);
    g_test_add_func ("/proto/chat/highlight_match_case_insensitive",
                     test_highlight_match_case_insensitive);
    g_test_add_func ("/proto/chat/highlight_match_word_boundary",
                     test_highlight_match_word_boundary);
    g_test_add_func ("/proto/chat/highlight_match_at_buffer_start",
                     test_highlight_match_at_buffer_start);
    g_test_add_func ("/proto/chat/highlight_match_at_buffer_end",
                     test_highlight_match_at_buffer_end);
    g_test_add_func ("/proto/chat/highlight_match_punctuation_boundary",
                     test_highlight_match_punctuation_boundary);
    g_test_add_func ("/proto/chat/highlight_match_multiple_words",
                     test_highlight_match_multiple_words);
    g_test_add_func ("/proto/chat/highlight_match_skips_empty_words",
                     test_highlight_match_skips_empty_words);
    g_test_add_func ("/proto/chat/highlight_match_no_words_returns_false",
                     test_highlight_match_no_words_returns_false);
    g_test_add_func ("/proto/chat/highlight_match_empty_body",
                     test_highlight_match_empty_body);

    return g_test_run ();
}
