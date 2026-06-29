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

/* ---------- Pathological-length guard ----------
 *
 * Two High-severity issues these tests pin down:
 *
 * 1. g_utf8_validate takes a gssize. A gsize len above G_MAXSSIZE wraps
 *    negative on the cast and GLib treats the input as NUL-terminated —
 *    happily reading past the supplied buffer. We must catch the
 *    overflow before the validate call.
 *
 * 2. The slow path allocates `len * 3 + 1` bytes. Without an overflow
 *    check, large len wraps to a tiny allocation; with a "G_MAXSIZE - 1"
 *    saturation it tries to g_malloc near-SIZE_MAX. Either way: bad.
 *
 * The function's contract: return g_strdup("") on pathological input,
 * never reach g_utf8_validate or g_malloc with the bogus size.
 *
 * Construction trick: we pass a tiny on-stack buffer with a huge `len`.
 * If the guard fires correctly the function never reads through `bytes`,
 * so the read-OOB only happens if the bug is present. We can't
 * literally allocate G_MAXSIZE memory to "prove" the bug from this
 * angle; what we can verify is the contract — empty string out,
 * out_len = 0, no crash. */

static void
test_len_above_max_returns_empty (void)
{
    /* tiny stack buffer + huge len — the guard must short-circuit
	 * before g_utf8_validate would scan through `bytes`. */
    char input = 'a';
    gsize huge = GTKHX_TEXT_TO_UTF8_MAX_LEN + 1;
    gsize len_out = 42;

    char *out = gtkhx_text_to_utf8 (&input, huge, &len_out);
    g_assert_nonnull (out);
    g_assert_cmpstr (out, ==, "");
    g_assert_cmpuint (len_out, ==, 0);
    g_free (out);
}

static void
test_len_g_maxsize_returns_empty (void)
{
    /* Specifically the SIZE_MAX case Copilot flagged: previous code
	 * tried to "saturate" with G_MAXSIZE - 1 and then g_malloc that.
	 * The guard must reject this without any allocation attempt. */
    char input = 'a';
    gsize len_out = 99;

    char *out = gtkhx_text_to_utf8 (&input, G_MAXSIZE, &len_out);
    g_assert_nonnull (out);
    g_assert_cmpstr (out, ==, "");
    g_assert_cmpuint (len_out, ==, 0);
    g_free (out);
}

static void
test_len_at_max_does_not_call_validate_with_wraparound (void)
{
    /* G_MAXSSIZE + 1 is the precise gssize-wraparound threshold for
	 * the g_utf8_validate cast (the cast wraps negative at that
	 * value). The bound covers it. */
    char input = 'a';
    gsize len_out = 0;

    char *out = gtkhx_text_to_utf8 (&input, (gsize) G_MAXSSIZE + 1, &len_out);
    g_assert_nonnull (out);
    g_assert_cmpstr (out, ==, "");
    g_assert_cmpuint (len_out, ==, 0);
    g_free (out);
}

static void
test_len_above_decoded_isize_cap_returns_empty (void)
{
    /* Pinpoint the case Copilot called out: `len` could be below
	 * the gssize-wrap threshold but high enough that `len * 3` blows
	 * past isize::MAX. Without a tight bound the C side would
	 * g_malloc(len * 3 + 1) (an astronomical allocation that aborts
	 * the process) while the Rust shim rejects the cap. The tightened
	 * bound `(G_MAXSSIZE - 1) / 3` rejects any len in that gap,
	 * before either allocation or FFI hand-off.
	 *
	 * Pick a value just above the bound but below G_MAXSSIZE — i.e.
	 * inside the previously-uncovered gap. */
    char input = 'a';
    gsize bad_len = GTKHX_TEXT_TO_UTF8_MAX_LEN + 1;
    gsize len_out = 0;

    /* Sanity: this value is what the bug requires (above bound but
	 * still below G_MAXSSIZE, so the old g_utf8_validate cast wouldn't
	 * wrap and the old bound wouldn't catch it). */
    g_assert_cmpuint (bad_len, <=, (gsize) G_MAXSSIZE);
    /* And `bad_len * 3 + 1` would overflow gssize, which is what
	 * forces the allocation path past isize::MAX. */
    g_assert_cmpuint (bad_len, >, (gsize) G_MAXSSIZE / 3);

    char *out = gtkhx_text_to_utf8 (&input, bad_len, &len_out);
    g_assert_nonnull (out);
    g_assert_cmpstr (out, ==, "");
    g_assert_cmpuint (len_out, ==, 0);
    g_free (out);
}

static void
test_len_one_byte_under_max_still_works_for_small_input (void)
{
    /* The guard is `len > MAX_LEN`, so len == MAX_LEN exactly should
	 * be accepted (in principle). We can't actually allocate that
	 * much, so we instead verify the OPPOSITE direction: a tiny
	 * len with a normal input continues to work — guards above
	 * the bound aren't accidentally rejecting reasonable input. */
    const char *input = "still works";
    gsize len_out = 0;

    char *out = gtkhx_text_to_utf8 (input, strlen (input), &len_out);
    g_assert_nonnull (out);
    g_assert_cmpstr (out, ==, "still works");
    g_assert_cmpuint (len_out, ==, strlen (input));
    g_free (out);
}

/* ---------- gtkhx_text_for_wire (Phase E2/E3) ---------- */

/* UTF-8 mode: pass-through. The input is already in the wire
 * encoding, so the function should just g_strndup it. */
static void
test_for_wire_utf8_mode_passthrough_ascii (void)
{
    const char *in = "hello";
    gsize len = 0;
    char *out = gtkhx_text_for_wire (in, strlen (in), /*utf8_mode=*/TRUE,
                                     /*is_body=*/FALSE, &len);
    g_assert_cmpstr (out, ==, "hello");
    g_assert_cmpuint (len, ==, 5);
    g_free (out);
}

/* UTF-8 mode: multibyte characters pass through verbatim. */
static void
test_for_wire_utf8_mode_passthrough_multibyte (void)
{
    /* "café" (5 UTF-8 bytes). */
    const char in[] = "caf\xc3\xa9";
    gsize len = 0;
    char *out = gtkhx_text_for_wire (in, 5, TRUE, FALSE, &len);
    g_assert_cmpuint (len, ==, 5);
    g_assert_cmpmem (out, len, in, 5);
    g_free (out);
}

/* UTF-8 mode: LF passes through even when is_body is TRUE — the spec
 * says UTF-8 clients receive LF as-is. */
static void
test_for_wire_utf8_mode_keeps_lf (void)
{
    const char in[] = "line1\nline2";
    gsize len = 0;
    char *out = gtkhx_text_for_wire (in, strlen (in), TRUE, /*is_body=*/TRUE,
                                     &len);
    g_assert_cmpstr (out, ==, "line1\nline2");
    /* No CR anywhere. */
    g_assert_null (memchr (out, '\r', len));
    g_free (out);
}

/* Legacy mode: ASCII passes through unchanged. */
static void
test_for_wire_legacy_ascii_passthrough (void)
{
    const char *in = "hello";
    gsize len = 0;
    char *out = gtkhx_text_for_wire (in, strlen (in), /*utf8_mode=*/FALSE,
                                     /*is_body=*/FALSE, &len);
    g_assert_cmpstr (out, ==, "hello");
    g_assert_cmpuint (len, ==, 5);
    g_free (out);
}

/* Legacy mode: UTF-8 multibyte transcodes to its Mac Roman code
 * point. "café" → 0x63 0x61 0x66 0x8e. */
static void
test_for_wire_legacy_e_acute_round_trips (void)
{
    const char in[] = "caf\xc3\xa9";
    const guint8 expected[] = { 'c', 'a', 'f', 0x8e };
    gsize len = 0;
    char *out = gtkhx_text_for_wire (in, 5, FALSE, FALSE, &len);
    g_assert_cmpuint (len, ==, 4);
    g_assert_cmpmem (out, len, expected, 4);
    g_free (out);
}

/* Legacy mode: curly quotes — U+201C / U+201D — transcode to Mac
 * Roman 0xd2 / 0xd3 (the canonical inverse of the inbound test
 * above). */
static void
test_for_wire_legacy_curly_quotes (void)
{
    const char in[] = "\xe2\x80\x9chi\xe2\x80\x9d";
    const guint8 expected[] = { 0xd2, 'h', 'i', 0xd3 };
    gsize len = 0;
    char *out = gtkhx_text_for_wire (in, strlen (in), FALSE, FALSE, &len);
    g_assert_cmpuint (len, ==, 4);
    g_assert_cmpmem (out, len, expected, 4);
    g_free (out);
}

/* Legacy mode (phase E2): emoji are rewritten to their ASCII
 * `:shortcode:` form *before* Mac Roman conversion, so they ride the wire
 * as readable text instead of the '?' substitute. U+1F60A SMILING FACE
 * WITH SMILING EYES → ":blush:". */
static void
test_for_wire_legacy_emoji_to_shortcode (void)
{
    const char in[] = "ok\xf0\x9f\x98\x8a";  /* "ok😊" */
    gsize len = 0;
    char *out = gtkhx_text_for_wire (in, strlen (in), FALSE, FALSE, &len);
    g_assert_cmpmem (out, len, "ok:blush:", 9);
    /* No '?' substitute and no high bytes survived. */
    g_assert_null (memchr (out, '?', len));
    for (gsize i = 0; i < len; i++) {
        g_assert_true ((guint8) out[i] < 0x80);
    }
    g_free (out);
}

/* Legacy mode: a codepoint with no shortcode AND no Mac Roman mapping
 * still falls back to '?'. U+0950 DEVANAGARI OM is neither an emoji (so
 * the rewrite leaves it alone) nor in Mac Roman (so g_convert substitutes
 * it) — confirms the rewrite didn't displace the '?' fallback. */
static void
test_for_wire_legacy_non_emoji_unmappable_still_substitutes (void)
{
    const char in[] = "om\xe0\xa5\x90";  /* "om" + U+0950 */
    gsize len = 0;
    char *out = gtkhx_text_for_wire (in, strlen (in), FALSE, FALSE, &len);
    g_assert_cmpint (out[0], ==, 'o');
    g_assert_cmpint (out[1], ==, 'm');
    g_assert_cmpint (out[2], ==, '?');
    g_free (out);
}

/* UTF-8 mode must NOT rewrite emoji — the wire carries the real
 * codepoint when the server speaks UTF-8. */
static void
test_for_wire_utf8_mode_keeps_emoji (void)
{
    const char in[] = "ok\xf0\x9f\x98\x8a";  /* "ok😊" */
    gsize len = 0;
    char *out = gtkhx_text_for_wire (in, strlen (in), /*utf8_mode=*/TRUE, FALSE,
                                     &len);
    g_assert_cmpmem (out, len, in, strlen (in));
    g_free (out);
}

/* Legacy mode + is_body: emoji rewrite composes with LF→CR — the
 * shortcode is inserted, then LFs around it become CRs. */
static void
test_for_wire_legacy_emoji_with_body_crlf (void)
{
    const char in[] = "hi \xf0\x9f\x8e\x89\nbye";  /* "hi 🎉\nbye" */
    gsize len = 0;
    char *out = gtkhx_text_for_wire (in, strlen (in), FALSE, /*is_body=*/TRUE,
                                     &len);
    g_assert_cmpmem (out, len, "hi :tada:\rbye", 13);
    g_free (out);
}

/* Legacy mode + is_body: LF → CR normalisation for classic Mac line
 * endings. */
static void
test_for_wire_legacy_body_lf_to_cr (void)
{
    const char in[] = "line1\nline2\nline3";
    gsize len = 0;
    char *out = gtkhx_text_for_wire (in, strlen (in), FALSE, /*is_body=*/TRUE,
                                     &len);
    g_assert_cmpuint (len, ==, strlen (in));
    /* No LFs. */
    g_assert_null (memchr (out, '\n', len));
    /* Two CRs. */
    int crs = 0;
    for (gsize i = 0; i < len; i++) {
        if (out[i] == '\r') {
            crs++;
        }
    }
    g_assert_cmpint (crs, ==, 2);
    g_free (out);
}

/* Legacy mode + !is_body: LF stays untouched (nicks / subjects
 * shouldn't have line endings but if they do, don't rewrite). */
static void
test_for_wire_legacy_name_keeps_lf (void)
{
    const char in[] = "weird\nname";
    gsize len = 0;
    char *out = gtkhx_text_for_wire (in, strlen (in), FALSE, /*is_body=*/FALSE,
                                     &len);
    g_assert_nonnull (memchr (out, '\n', len));
    g_assert_null (memchr (out, '\r', len));
    g_free (out);
}

/* NULL input is allowed for caller convenience. */
static void
test_for_wire_null_input (void)
{
    gsize len = 99;
    char *out = gtkhx_text_for_wire (NULL, 0, TRUE, TRUE, &len);
    g_assert_nonnull (out);
    g_assert_cmpuint (len, ==, 0);
    g_assert_cmpint (out[0], ==, '\0');
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
    g_test_add_func ("/text_util/len_above_max_returns_empty",
                     test_len_above_max_returns_empty);
    g_test_add_func ("/text_util/len_g_maxsize_returns_empty",
                     test_len_g_maxsize_returns_empty);
    g_test_add_func ("/text_util/len_at_max_does_not_call_validate_with_wraparound",
                     test_len_at_max_does_not_call_validate_with_wraparound);
    g_test_add_func ("/text_util/len_above_decoded_isize_cap_returns_empty",
                     test_len_above_decoded_isize_cap_returns_empty);
    g_test_add_func ("/text_util/len_one_byte_under_max_still_works_for_small_input",
                     test_len_one_byte_under_max_still_works_for_small_input);

    g_test_add_func ("/text_util/for_wire/utf8_mode_passthrough_ascii",
                     test_for_wire_utf8_mode_passthrough_ascii);
    g_test_add_func ("/text_util/for_wire/utf8_mode_passthrough_multibyte",
                     test_for_wire_utf8_mode_passthrough_multibyte);
    g_test_add_func ("/text_util/for_wire/utf8_mode_keeps_lf",
                     test_for_wire_utf8_mode_keeps_lf);
    g_test_add_func ("/text_util/for_wire/legacy_ascii_passthrough",
                     test_for_wire_legacy_ascii_passthrough);
    g_test_add_func ("/text_util/for_wire/legacy_e_acute_round_trips",
                     test_for_wire_legacy_e_acute_round_trips);
    g_test_add_func ("/text_util/for_wire/legacy_curly_quotes",
                     test_for_wire_legacy_curly_quotes);
    g_test_add_func ("/text_util/for_wire/legacy_emoji_to_shortcode",
                     test_for_wire_legacy_emoji_to_shortcode);
    g_test_add_func ("/text_util/for_wire/legacy_non_emoji_unmappable_still_substitutes",
                     test_for_wire_legacy_non_emoji_unmappable_still_substitutes);
    g_test_add_func ("/text_util/for_wire/utf8_mode_keeps_emoji",
                     test_for_wire_utf8_mode_keeps_emoji);
    g_test_add_func ("/text_util/for_wire/legacy_emoji_with_body_crlf",
                     test_for_wire_legacy_emoji_with_body_crlf);
    g_test_add_func ("/text_util/for_wire/legacy_body_lf_to_cr",
                     test_for_wire_legacy_body_lf_to_cr);
    g_test_add_func ("/text_util/for_wire/legacy_name_keeps_lf",
                     test_for_wire_legacy_name_keeps_lf);
    g_test_add_func ("/text_util/for_wire/null_input",
                     test_for_wire_null_input);

    return g_test_run ();
}
