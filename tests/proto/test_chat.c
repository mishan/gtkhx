/*
 * tests/proto/test_chat.c — drive hx_chat_extract against canned
 * HTLS_HDR_CHAT wire bodies.
 *
 * The handler in rcv.c reads three chunks from a chat message:
 *   HTLS_DATA_CHAT       — the body bytes
 *   HTLS_DATA_CHAT_ID    — which chat (cid 0 = main, anything else
 *                          is a private chat)
 *   HTLS_DATA_UID        — the speaker's UID (1.5+ servers only;
 *                          older servers omit this and the body
 *                          starts with "\nFrom: ")
 *
 * The body then gets the same sanitisation pass we covered in
 * test_text_sanitisers, plus a leading-LF strip (the "\nUser: msg"
 * Hotline format would otherwise render as a blank line). Pango
 * UTF-8 sanitisation happens later in chat.c — not in scope here.
 *
 * Coverage notes:
 *   - Pin down the ID round-trip (CHAT_ID and UID are 4 / 2 bytes
 *     respectively in the wire format; dh_getint dispatches by len)
 *   - Body sanitisation: CR→LF, ANSI, leading-LF strip
 *   - Truncation at the 8 KiB stack-buffer boundary
 *   - Empty / missing chunks
 */

#include "config.h"
#include <string.h>
#include <glib.h>
#include "protocol.h"
#include "hotline.h"
#include "proto_helpers.h"
#include "wire_fixture.h"

/* ---------- Body bytes ---------- */

static void
test_chat_extracts_simple_body (void)
{
    struct htlc_conn htlc;
    const char *body = "hello, world";
    wire_fixture_init (&htlc, HTLS_HDR_CHAT, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_CHAT, strlen (body), body);

    struct hx_chat_msg msg;
    g_assert_true (hx_chat_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, &msg));
    g_assert_cmpstr (msg.text, ==, "hello, world");
    g_assert_cmpuint (msg.text_len, ==, strlen (body));
    g_assert_cmpuint (msg.cid, ==, 0);
    g_assert_cmpuint (msg.uid, ==, 0);

    wire_fixture_free (&htlc);
}

static void
test_chat_strips_leading_newline (void)
{
    /* Hotline servers commonly format chat as "\nUser: message".
	 * The handler skips the leading LF so the chat widget doesn't
	 * render a blank line above the message. */
    struct htlc_conn htlc;
    const char *body = "\nMisha: hello!";
    wire_fixture_init (&htlc, HTLS_HDR_CHAT, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_CHAT, strlen (body), body);

    struct hx_chat_msg msg;
    g_assert_true (hx_chat_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, &msg));
    /* Note the leading '\n' is gone. */
    g_assert_cmpstr (msg.text, ==, "Misha: hello!");
    g_assert_cmpuint (msg.text_len, ==, strlen (body) - 1);
    /* msg.text points one past msg.buf — into the same allocation. */
    g_assert_true (msg.text == msg.buf + 1);

    wire_fixture_free (&htlc);
}

static void
test_chat_does_not_strip_internal_newline (void)
{
    /* Leading-LF strip is exactly one byte, only at position 0.
	 * Internal newlines stay. */
    struct htlc_conn htlc;
    const char *body = "alpha\nbeta\ngamma";
    wire_fixture_init (&htlc, HTLS_HDR_CHAT, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_CHAT, strlen (body), body);

    struct hx_chat_msg msg;
    g_assert_true (hx_chat_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, &msg));
    g_assert_cmpstr (msg.text, ==, "alpha\nbeta\ngamma");
    g_assert_true (msg.text == msg.buf); /* no leading-LF strip */

    wire_fixture_free (&htlc);
}

static void
test_chat_converts_cr_to_lf (void)
{
    /* Hotline wire endings are '\r'. CR2LF runs before the
	 * leading-LF strip — so a body of "\rUser: hi" becomes
	 * "\nUser: hi" then strips to "User: hi". */
    struct htlc_conn htlc;
    const char *body = "\rUser: hi\rline two";
    wire_fixture_init (&htlc, HTLS_HDR_CHAT, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_CHAT, strlen (body), body);

    struct hx_chat_msg msg;
    g_assert_true (hx_chat_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, &msg));
    g_assert_cmpstr (msg.text, ==, "User: hi\nline two");

    wire_fixture_free (&htlc);
}

static void
test_chat_strips_ansi (void)
{
    struct htlc_conn htlc;
    const char body[] = "\x1b[31mred message\x1b[0m";
    const char expected[] = "[[31mred message[[0m";
    wire_fixture_init (&htlc, HTLS_HDR_CHAT, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_CHAT, sizeof (body) - 1, body);

    struct hx_chat_msg msg;
    g_assert_true (hx_chat_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, &msg));
    g_assert_cmpstr (msg.text, ==, expected);

    wire_fixture_free (&htlc);
}

/* ---------- CHAT_ID dispatch ---------- */

static void
test_chat_extracts_chat_id (void)
{
    struct htlc_conn htlc;
    const char *body = "hi";
    const guint32 cid_wire = g_htonl(0x12345678);
    wire_fixture_init (&htlc, HTLS_HDR_CHAT, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_CHAT_ID, sizeof (cid_wire),
                            &cid_wire);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_CHAT, strlen (body), body);

    struct hx_chat_msg msg;
    g_assert_true (hx_chat_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, &msg));
    g_assert_cmphex (msg.cid, ==, 0x12345678u);
    g_assert_cmpstr (msg.text, ==, "hi");

    wire_fixture_free (&htlc);
}

/* HTLS_DATA_UID is 2 bytes wire-format. dh_getint dispatches off
 * `_len`: 4 bytes uses HN32, anything else uses HN16. Verify the
 * 2-byte path. */
static void
test_chat_extracts_uid (void)
{
    struct htlc_conn htlc;
    const char *body = "msg";
    const guint16 uid_wire = g_htons(0xabcd);
    wire_fixture_init (&htlc, HTLS_HDR_CHAT, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_UID, sizeof (uid_wire), &uid_wire);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_CHAT, strlen (body), body);

    struct hx_chat_msg msg;
    g_assert_true (hx_chat_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, &msg));
    g_assert_cmphex (msg.uid, ==, 0xabcdu);

    wire_fixture_free (&htlc);
}

/* ---------- Truncation at buffer size ---------- */

static void
test_chat_truncates_oversized_body (void)
{
    /* The handler caps the copy at sizeof(buf) - 1 = 8192 bytes.
	 * Send 9000 bytes of 'A', expect 8192 'A's plus NUL. */
    struct htlc_conn htlc;
    guint8 big[9000];
    memset (big, 'A', sizeof (big));
    /* DATA_CHAT chunk len is uint16 — values must fit in 65535,
	 * which 9000 does. */
    wire_fixture_init (&htlc, HTLS_HDR_CHAT, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_CHAT, sizeof (big), big);

    struct hx_chat_msg msg;
    g_assert_true (hx_chat_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, &msg));
    g_assert_cmpuint (strlen (msg.text), ==, 8192);
    for (gsize i = 0; i < 8192; i++) {
        g_assert_cmphex (msg.text[i], ==, 'A');
    }
    g_assert_cmphex (msg.buf[8192], ==, '\0');

    wire_fixture_free (&htlc);
}

/* ---------- Empty / missing chunks ---------- */

static void
test_chat_empty_body_is_valid (void)
{
    /* Zero-length DATA_CHAT chunk: text is "", text_len == 0.
	 * Some servers send these as keepalives.
	 *
	 * Note: wire_fixture_add_chunk's dh->data field is the FAM tail
	 * of the hl_data_hdr struct, but with zero len there's nothing
	 * to memcpy and the helper handles NULL data. */
    struct htlc_conn htlc;
    wire_fixture_init (&htlc, HTLS_HDR_CHAT, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_CHAT, 0, NULL);

    struct hx_chat_msg msg;
    g_assert_true (hx_chat_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, &msg));
    g_assert_cmpstr (msg.text, ==, "");
    g_assert_cmpuint (msg.text_len, ==, 0);

    wire_fixture_free (&htlc);
}

static void
test_chat_missing_body_is_valid (void)
{
    /* No DATA_CHAT chunk at all (bizarre but defensively handled).
	 * text == "", cid / uid pulled from whatever IDs were sent. */
    struct htlc_conn htlc;
    const guint32 cid_wire = g_htonl(5);
    wire_fixture_init (&htlc, HTLS_HDR_CHAT, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_CHAT_ID, sizeof (cid_wire),
                            &cid_wire);

    struct hx_chat_msg msg;
    g_assert_true (hx_chat_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, &msg));
    g_assert_cmpstr (msg.text, ==, "");
    g_assert_cmphex (msg.cid, ==, 5);

    wire_fixture_free (&htlc);
}

/* ---------- API edge cases ---------- */

static void
test_chat_null_out_returns_false (void)
{
    struct htlc_conn htlc;
    const char *body = "hi";
    wire_fixture_init (&htlc, HTLS_HDR_CHAT, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_CHAT, strlen (body), body);

    g_assert_false (hx_chat_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, NULL));

    wire_fixture_free (&htlc);
}

/* ---------- All three chunks in one message ---------- */

static void
test_chat_all_three_chunks_combined (void)
{
    struct htlc_conn htlc;
    const guint32 cid_wire = g_htonl(7);
    const guint16 uid_wire = g_htons(42);
    const char *body = "\nMisha: hello team";
    wire_fixture_init (&htlc, HTLS_HDR_CHAT, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_CHAT_ID, sizeof (cid_wire),
                            &cid_wire);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_UID, sizeof (uid_wire), &uid_wire);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_CHAT, strlen (body), body);

    struct hx_chat_msg msg;
    g_assert_true (hx_chat_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, &msg));
    g_assert_cmphex (msg.cid, ==, 7);
    g_assert_cmphex (msg.uid, ==, 42);
    g_assert_cmpstr (msg.text, ==, "Misha: hello team");

    wire_fixture_free (&htlc);
}

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

    g_test_add_func ("/proto/chat/extracts_simple_body",
                     test_chat_extracts_simple_body);
    g_test_add_func ("/proto/chat/strips_leading_newline",
                     test_chat_strips_leading_newline);
    g_test_add_func ("/proto/chat/does_not_strip_internal_newline",
                     test_chat_does_not_strip_internal_newline);
    g_test_add_func ("/proto/chat/converts_cr_to_lf",
                     test_chat_converts_cr_to_lf);
    g_test_add_func ("/proto/chat/strips_ansi", test_chat_strips_ansi);

    g_test_add_func ("/proto/chat/extracts_chat_id",
                     test_chat_extracts_chat_id);
    g_test_add_func ("/proto/chat/extracts_uid", test_chat_extracts_uid);

    g_test_add_func ("/proto/chat/truncates_oversized_body",
                     test_chat_truncates_oversized_body);

    g_test_add_func ("/proto/chat/empty_body_is_valid",
                     test_chat_empty_body_is_valid);
    g_test_add_func ("/proto/chat/missing_body_is_valid",
                     test_chat_missing_body_is_valid);

    g_test_add_func ("/proto/chat/null_out_returns_false",
                     test_chat_null_out_returns_false);

    g_test_add_func ("/proto/chat/all_three_chunks_combined",
                     test_chat_all_three_chunks_combined);

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
