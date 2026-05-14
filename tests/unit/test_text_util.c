/*
 * tests/unit/test_text_util.c — verify gtkhx_text_to_utf8 sanitises
 * 8-bit-text-from-the-wire into valid UTF-8 across the three branches
 * the function commits to:
 *
 *   1. Already-valid UTF-8 → returned verbatim (just g_strndup'd).
 *   2. Invalid UTF-8 that's valid Mac Roman → g_convert through
 *      MACINTOSH succeeds and the output round-trips to known
 *      codepoints.
 *   3. Junk that's neither valid UTF-8 nor a clean MACINTOSH source
 *      (only really happens for unterminated multibyte sequences and
 *      similar — see notes below): falls back to g_utf8_make_valid,
 *      which substitutes U+FFFD for the unrepresentable bytes.
 *
 * Plus the NULL / empty / out_len edge cases that callers rely on.
 */

#include "config.h"
#include <string.h>
#include <glib.h>
#include "text_util.h"

/* ---------- Branch 1: already valid UTF-8 ---------- */

static void
test_valid_utf8_passthrough_ascii (void)
{
    const char *input = "hello world";
    gsize len_in = strlen (input);
    gsize len_out = 0;

    char *out = gtkhx_text_to_utf8 (input, len_in, &len_out);
    g_assert_nonnull (out);
    g_assert_cmpstr (out, ==, "hello world");
    g_assert_cmpuint (len_out, ==, len_in);
    g_free (out);
}

static void
test_valid_utf8_passthrough_multibyte (void)
{
    /* "café" — 'é' is 0xc3 0xa9 in UTF-8, two bytes. Total 5 bytes
	 * for a 4-codepoint string. */
    const char input[] = "caf\xc3\xa9";
    gsize len_in = sizeof (input) - 1;
    gsize len_out = 0;

    char *out = gtkhx_text_to_utf8 (input, len_in, &len_out);
    g_assert_nonnull (out);
    g_assert_cmpstr (out, ==, "caf\xc3\xa9");
    g_assert_cmpuint (len_out, ==, len_in);
    g_assert_true (g_utf8_validate (out, -1, NULL));
    g_free (out);
}

/* Embedded NULs are valid UTF-8. The function should preserve them
 * verbatim (g_strndup-style copy) and report the correct byte length
 * so the caller can use len_out + the buffer rather than strlen. */
static void
test_valid_utf8_preserves_embedded_nul (void)
{
    const char input[] = { 'a', 'b', '\0', 'c', 'd' };
    gsize len_in = sizeof (input);
    gsize len_out = 0;

    char *out = gtkhx_text_to_utf8 (input, len_in, &len_out);
    g_assert_nonnull (out);
    g_assert_cmpuint (len_out, ==, len_in);
    g_assert_cmpint (memcmp (out, input, len_in), ==, 0);
    g_free (out);
}

/* ---------- Branch 2: Mac Roman → UTF-8 conversion ----------
 *
 * Mac Roman code points we care about (and their UTF-8 outputs):
 *   0x8e  →  é   →  0xc3 0xa9
 *   0xd5  →  '   →  0xe2 0x80 0x99 (RIGHT SINGLE QUOTATION MARK)
 *   0xd2  →  "   →  0xe2 0x80 0x9c (LEFT DOUBLE QUOTATION MARK)
 *   0xd3  →  "   →  0xe2 0x80 0x9d (RIGHT DOUBLE QUOTATION MARK)
 *
 * Bytes 0x80..0xff that do NOT begin a valid UTF-8 sequence are the
 * giveaway that the input isn't already UTF-8. */

static void
test_mac_roman_e_acute (void)
{
    /* "café" but the 'é' is a single Mac Roman 0x8e byte. */
    const char input[] = { 'c', 'a', 'f', (char)0x8e };
    gsize len_in = sizeof (input);
    gsize len_out = 0;

    g_assert_false (g_utf8_validate (input, len_in, NULL));

    char *out = gtkhx_text_to_utf8 (input, len_in, &len_out);
    g_assert_nonnull (out);
    g_assert_true (g_utf8_validate (out, -1, NULL));

    /* g_convert may NUL-terminate the output and exclude that NUL
	 * from bytes_written, which is what we report. Compare on the
	 * NUL-terminated string. */
    g_assert_cmpstr (out, ==, "caf\xc3\xa9");
    g_free (out);
}

static void
test_mac_roman_curly_quotes (void)
{
    /* Mac Roman: 0xd2 0xd3 = open + close double quote.
	 * UTF-8: U+201C U+201D = 0xe2 0x80 0x9c 0xe2 0x80 0x9d. */
    const char input[] = { (char)0xd2, 'h', 'i', (char)0xd3 };
    gsize len_in = sizeof (input);
    gsize len_out = 0;

    g_assert_false (g_utf8_validate (input, len_in, NULL));

    char *out = gtkhx_text_to_utf8 (input, len_in, &len_out);
    g_assert_nonnull (out);
    g_assert_true (g_utf8_validate (out, -1, NULL));
    g_assert_cmpstr (out, ==, "\xe2\x80\x9chi\xe2\x80\x9d");
    g_free (out);
}

/* ---------- Branch 3: g_utf8_make_valid fallback ----------
 *
 * MACINTOSH iconv is permissive — every 0x00..0xff byte maps to
 * something — so the make_valid branch is hard to hit with real
 * Mac-Roman-shaped input. The reliable trigger is data that g_convert
 * itself rejects mid-stream, e.g. an explicit "no" via embedded
 * shift-state errors, but those don't exist in MACINTOSH either.
 *
 * Practically the make_valid fallback exists as a safety net. The
 * codepath we can exercise here is "the conversion succeeds and the
 * output is valid UTF-8 either way" — the important invariant is
 * that gtkhx_text_to_utf8 NEVER returns NULL and the result ALWAYS
 * validates as UTF-8. The next test enforces both.
 */

static void
test_high_byte_garbage_still_returns_valid_utf8 (void)
{
    /* Random 0xff/0xfe pile — invalid UTF-8 (no leading-byte
	 * pattern). MACINTOSH will happily map these to whatever it
	 * thinks they are; either way the contract is "valid UTF-8 out,
	 * non-NULL, len_out matches strlen". */
    const char input[] = { (char)0xff, (char)0xfe, (char)0xff,
                           (char)0xfe, (char)0xc0, (char)0xc1 };
    gsize len_in = sizeof (input);
    gsize len_out = 0;

    g_assert_false (g_utf8_validate (input, len_in, NULL));

    char *out = gtkhx_text_to_utf8 (input, len_in, &len_out);
    g_assert_nonnull (out);
    g_assert_true (g_utf8_validate (out, -1, NULL));
    g_assert_cmpuint (len_out, ==, strlen (out));
    g_free (out);
}

/* ---------- Edge cases on the API ---------- */

static void
test_null_input_returns_empty (void)
{
    gsize len_out = 42; /* should be cleared */

    char *out = gtkhx_text_to_utf8 (NULL, 0, &len_out);
    g_assert_nonnull (out);
    g_assert_cmpstr (out, ==, "");
    g_assert_cmpuint (len_out, ==, 0);
    g_free (out);
}

static void
test_null_input_null_out_len (void)
{
    /* NULL out_len is allowed and must not crash. */
    char *out = gtkhx_text_to_utf8 (NULL, 0, NULL);
    g_assert_nonnull (out);
    g_assert_cmpstr (out, ==, "");
    g_free (out);
}

static void
test_empty_input_zero_length (void)
{
    const char *input = "";
    gsize len_out = 99;

    char *out = gtkhx_text_to_utf8 (input, 0, &len_out);
    g_assert_nonnull (out);
    g_assert_cmpuint (len_out, ==, 0);
    g_assert_cmpint (out[0], ==, '\0');
    g_free (out);
}

static void
test_out_len_optional_on_valid_input (void)
{
    const char *input = "valid";
    char *out = gtkhx_text_to_utf8 (input, strlen (input), NULL);
    g_assert_nonnull (out);
    g_assert_cmpstr (out, ==, "valid");
    g_free (out);
}

static void
test_out_len_optional_on_mac_roman (void)
{
    /* Same Mac Roman 0x8e → é but caller passes NULL for out_len.
	 * Function must not deref NULL while reporting bytes_written. */
    const char input[] = { 'a', (char)0x8e };
    char *out = gtkhx_text_to_utf8 (input, sizeof (input), NULL);
    g_assert_nonnull (out);
    g_assert_true (g_utf8_validate (out, -1, NULL));
    g_assert_cmpstr (out, ==, "a\xc3\xa9");
    g_free (out);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/text_util/valid_utf8_passthrough_ascii",
                     test_valid_utf8_passthrough_ascii);
    g_test_add_func ("/text_util/valid_utf8_passthrough_multibyte",
                     test_valid_utf8_passthrough_multibyte);
    g_test_add_func ("/text_util/valid_utf8_preserves_embedded_nul",
                     test_valid_utf8_preserves_embedded_nul);
    g_test_add_func ("/text_util/mac_roman_e_acute", test_mac_roman_e_acute);
    g_test_add_func ("/text_util/mac_roman_curly_quotes",
                     test_mac_roman_curly_quotes);
    g_test_add_func ("/text_util/high_byte_garbage_still_returns_valid_utf8",
                     test_high_byte_garbage_still_returns_valid_utf8);
    g_test_add_func ("/text_util/null_input_returns_empty",
                     test_null_input_returns_empty);
    g_test_add_func ("/text_util/null_input_null_out_len",
                     test_null_input_null_out_len);
    g_test_add_func ("/text_util/empty_input_zero_length",
                     test_empty_input_zero_length);
    g_test_add_func ("/text_util/out_len_optional_on_valid_input",
                     test_out_len_optional_on_valid_input);
    g_test_add_func ("/text_util/out_len_optional_on_mac_roman",
                     test_out_len_optional_on_mac_roman);

    return g_test_run ();
}
