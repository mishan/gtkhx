/*
 * tests/unit/test_text_sanitisers.c — verify the in-place byte-pile
 * transforms that everything between the wire and the widget runs
 * incoming text through.
 *
 * Three things under test:
 *
 *   CR2LF / LF2CR (compat.h macros)
 *     The Hotline wire format uses '\r' for line endings; we use
 *     '\n' internally. Every chunk parser in rcv.c calls CR2LF on
 *     the way in; commands.c / msg.c / news.c call LF2CR on the way
 *     out. Both are in-place and length-preserving — they just
 *     swap one byte for another.
 *
 *   strip_ansi (protocol.h static inline)
 *     "Strip" is a misnomer — the function actually maps low-control
 *     bytes to printable equivalents (it's the same trick xtext used
 *     to do for IRC ANSI: byte = (byte & 0x7f) | 0x40). The exact
 *     range is "byte > 13 && byte < 31 && byte != 15 && byte != 22",
 *     which is a hand-tuned blacklist that excludes TAB (9), LF (10),
 *     CR (13), SI (15), and SYN (22). The rest get mapped.
 *
 * Nothing in any of the three changes the buffer's length; they all
 * walk it byte-by-byte and rewrite in place. So the contracts are:
 *   - len_in == len_out (no realloc, no truncation)
 *   - bytes outside the targeted set are passed through unchanged
 *   - bytes inside the targeted set are transformed exactly as
 *     documented above
 */

#include "config.h"
#include <string.h>
#include <glib.h>
#include "compat.h"
#include "protocol.h" /* strip_ansi (static inline) */

/* ---------- CR2LF: '\r' → '\n' in place ---------- */

static void
test_cr2lf_simple (void)
{
    char buf[] = "hello\rworld\r";
    const gsize len = sizeof (buf) - 1;
    CR2LF (buf, len);
    g_assert_cmpstr (buf, ==, "hello\nworld\n");
}

static void
test_cr2lf_no_cr_unchanged (void)
{
    char buf[] = "no carriage returns here";
    const gsize len = sizeof (buf) - 1;
    char copy[64];
    memcpy (copy, buf, sizeof (buf));
    CR2LF (buf, len);
    g_assert_cmpmem (buf, sizeof (buf), copy, sizeof (buf));
}

static void
test_cr2lf_only_cr (void)
{
    char buf[] = "\r\r\r\r";
    const gsize len = sizeof (buf) - 1;
    CR2LF (buf, len);
    g_assert_cmpmem (buf, len, "\n\n\n\n", 4);
}

static void
test_cr2lf_does_not_touch_lf (void)
{
    /* LF should pass through unchanged — only CR gets mapped. */
    char buf[] = "a\nb\nc";
    const gsize len = sizeof (buf) - 1;
    char copy[16];
    memcpy (copy, buf, sizeof (buf));
    CR2LF (buf, len);
    g_assert_cmpmem (buf, sizeof (buf), copy, sizeof (buf));
}

static void
test_cr2lf_respects_len_not_nul (void)
{
    /* The macro walks until len, ignoring embedded NULs. CR after
     * an embedded NUL must still be converted. */
    char buf[] = { 'a', '\0', '\r', 'b' };
    CR2LF (buf, sizeof (buf));
    g_assert_cmphex (buf[0], ==, 'a');
    g_assert_cmphex (buf[1], ==, '\0');
    g_assert_cmphex (buf[2], ==, '\n');
    g_assert_cmphex (buf[3], ==, 'b');
}

static void
test_cr2lf_zero_length_no_op (void)
{
    char buf[] = "\r\r\r";
    char copy[8];
    memcpy (copy, buf, sizeof (buf));
    CR2LF (buf, 0);
    /* Buffer unchanged when len is zero. */
    g_assert_cmpmem (buf, sizeof (buf), copy, sizeof (buf));
}

/* ---------- LF2CR: '\n' → '\r' in place ---------- */

static void
test_lf2cr_simple (void)
{
    char buf[] = "hello\nworld\n";
    const gsize len = sizeof (buf) - 1;
    LF2CR (buf, len);
    g_assert_cmpstr (buf, ==, "hello\rworld\r");
}

static void
test_lf2cr_does_not_touch_cr (void)
{
    char buf[] = "a\rb\rc";
    const gsize len = sizeof (buf) - 1;
    char copy[16];
    memcpy (copy, buf, sizeof (buf));
    LF2CR (buf, len);
    g_assert_cmpmem (buf, sizeof (buf), copy, sizeof (buf));
}

static void
test_lf2cr_round_trip_via_cr2lf (void)
{
    /* Round-trip property: LF2CR followed by CR2LF returns the
     * original buffer. Tests that the two macros are exact
     * inverses byte-for-byte. */
    char buf[] = "line one\nline two\nline three";
    char orig[] = "line one\nline two\nline three";
    const gsize len = sizeof (buf) - 1;
    LF2CR (buf, len);
    CR2LF (buf, len);
    g_assert_cmpmem (buf, sizeof (buf), orig, sizeof (orig));
}

/* ---------- strip_ansi: low-control → printable ----------
 *
 * The transform: for byte b where 13 < b < 31 AND b != 15 AND b != 22,
 *   b' = (b & 127) | 64
 * which for the 14..30 range without 15/22 is equivalent to b + 64
 * (since bit 6 wasn't set and the values are well below 127). This
 * pushes them into the 'N'..'^' range — visible characters that no
 * longer trip a terminal escape.
 *
 * Bytes 0..13 (NUL through CR), 15 (SI), 22 (SYN), and 31+ pass
 * through unchanged.
 */

static void
test_strip_ansi_passes_through_printable_ascii (void)
{
    char buf[] = "Hello, world! 1234567890 !@#$%^&*()";
    char copy[64];
    memcpy (copy, buf, sizeof (buf));
    strip_ansi (buf, sizeof (buf) - 1);
    g_assert_cmpmem (buf, sizeof (buf), copy, sizeof (buf));
}

static void
test_strip_ansi_converts_esc (void)
{
    /* ESC (0x1b = 27) is in the targeted range. (27 & 127) | 64 = 91
     * = '['. Everyone's favorite "ESC[" sequence reduces to "[[". */
    char buf[] = "\x1b[31mred\x1b[0m";
    const gsize len = sizeof (buf) - 1;
    strip_ansi (buf, len);
    g_assert_cmpstr (buf, ==, "[[31mred[[0m");
}

static void
test_strip_ansi_passes_through_tab_lf_cr (void)
{
    /* TAB (9), LF (10), CR (13) are below 14 so the range check
     * skips them. They MUST survive — chat lines depend on this. */
    char buf[] = "tab\there\nnewline\rcarriage";
    const gsize len = sizeof (buf) - 1;
    char copy[64];
    memcpy (copy, buf, sizeof (buf));
    strip_ansi (buf, len);
    g_assert_cmpmem (buf, sizeof (buf), copy, sizeof (buf));
}

static void
test_strip_ansi_passes_through_si_and_syn (void)
{
    /* SI (15) and SYN (22) are explicitly excluded from the targeted
     * range — they're known mIRC color / formatting codes that the
     * codebase wants to keep. */
    /* String-concat to keep the hex escapes from greedily eating
     * the following ASCII chars (\x16d would be parsed as hex 0x16d,
     * out of range for char — gcc warns). */
    char buf[] = "\x0f"
                 "format"
                 "\x0f"
                 "chunk"
                 "\x16"
                 "data"
                 "\x16";
    const gsize len = sizeof (buf) - 1;
    char copy[32];
    memcpy (copy, buf, sizeof (buf));
    strip_ansi (buf, len);
    g_assert_cmpmem (buf, sizeof (buf), copy, sizeof (buf));
}

static void
test_strip_ansi_boundary_bytes (void)
{
    /* Boundaries: 13 keeps, 14 maps, 21 maps, 22 keeps, 23 maps,
     * 30 maps, 31 keeps. Spell out the expected output byte-for-
     * byte so a future tweak to the range check fails loudly. */
    char buf[] = { 13, 14, 21, 22, 23, 30, 31 };
    const gsize len = sizeof (buf);
    const char expected[] = {
        13,              /* keeps */
        (14 & 127) | 64, /* 78 = 'N' */
        (21 & 127) | 64, /* 85 = 'U' */
        22,              /* keeps (excluded) */
        (23 & 127) | 64, /* 87 = 'W' */
        (30 & 127) | 64, /* 94 = '^' */
        31,              /* keeps */
    };
    strip_ansi (buf, len);
    g_assert_cmpmem (buf, len, expected, sizeof (expected));
}

static void
test_strip_ansi_zero_length_no_op (void)
{
    char buf[] = "\x1b\x1b\x1b";
    char copy[8];
    memcpy (copy, buf, sizeof (buf));
    strip_ansi (buf, 0);
    g_assert_cmpmem (buf, sizeof (buf), copy, sizeof (buf));
}

static void
test_strip_ansi_idempotent (void)
{
    /* The transform output range is 64..94 ('@'..'^'), which is
     * outside the targeted 14..30 range, so applying strip_ansi
     * twice is the same as applying it once. */
    char a[] = "\x1b[31mred\x1b[0m";
    char b[] = "\x1b[31mred\x1b[0m";
    const gsize len = sizeof (a) - 1;
    strip_ansi (a, len);
    strip_ansi (b, len);
    strip_ansi (b, len); /* second pass */
    g_assert_cmpmem (a, len, b, len);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/text_sanitisers/cr2lf_simple", test_cr2lf_simple);
    g_test_add_func ("/text_sanitisers/cr2lf_no_cr_unchanged",
                     test_cr2lf_no_cr_unchanged);
    g_test_add_func ("/text_sanitisers/cr2lf_only_cr", test_cr2lf_only_cr);
    g_test_add_func ("/text_sanitisers/cr2lf_does_not_touch_lf",
                     test_cr2lf_does_not_touch_lf);
    g_test_add_func ("/text_sanitisers/cr2lf_respects_len_not_nul",
                     test_cr2lf_respects_len_not_nul);
    g_test_add_func ("/text_sanitisers/cr2lf_zero_length_no_op",
                     test_cr2lf_zero_length_no_op);

    g_test_add_func ("/text_sanitisers/lf2cr_simple", test_lf2cr_simple);
    g_test_add_func ("/text_sanitisers/lf2cr_does_not_touch_cr",
                     test_lf2cr_does_not_touch_cr);
    g_test_add_func ("/text_sanitisers/lf2cr_round_trip_via_cr2lf",
                     test_lf2cr_round_trip_via_cr2lf);

    g_test_add_func (
        "/text_sanitisers/strip_ansi_passes_through_printable_ascii",
        test_strip_ansi_passes_through_printable_ascii);
    g_test_add_func ("/text_sanitisers/strip_ansi_converts_esc",
                     test_strip_ansi_converts_esc);
    g_test_add_func ("/text_sanitisers/strip_ansi_passes_through_tab_lf_cr",
                     test_strip_ansi_passes_through_tab_lf_cr);
    g_test_add_func ("/text_sanitisers/strip_ansi_passes_through_si_and_syn",
                     test_strip_ansi_passes_through_si_and_syn);
    g_test_add_func ("/text_sanitisers/strip_ansi_boundary_bytes",
                     test_strip_ansi_boundary_bytes);
    g_test_add_func ("/text_sanitisers/strip_ansi_zero_length_no_op",
                     test_strip_ansi_zero_length_no_op);
    g_test_add_func ("/text_sanitisers/strip_ansi_idempotent",
                     test_strip_ansi_idempotent);

    return g_test_run ();
}
