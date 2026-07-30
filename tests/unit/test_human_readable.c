/*
 * tests/unit/test_human_readable.c — pin the byte-count formatter
 * used in the tasks window, file browser size column, transfer
 * progress labels, and elsewhere.
 *
 * human_size is hard-coded to base-1024 with binary suffixes
 * (k, M, G, T, …). The wrapper goes through human_readable, which
 * is verbatim fileutils-4.0/lib/human.c (modulo intmax_t →
 * guint32). The full function has knobs we don't exercise from
 * GtkHx in production; this test pins the human_size convention
 * — what the user actually sees — and exercises a couple of the
 * underlying human_readable shapes so a future "tidy this up"
 * pass that touches the rounding arithmetic gets caught fast.
 *
 * Why bother? The algorithm uses the .5-tenths-rounding scheme
 * with the rounding state machine in the comments; it's correct
 * but unobvious. The test pins the *current* outputs (1024 → 1.0k,
 * 1023 → 1023, 8500 → 8.3k, …) so any future arithmetic tidy-up
 * has to confront its display impact before merging.
 *
 * IMPORTANT: human_readable / human_size return a pointer INTO
 * the supplied buffer, not necessarily to its base — they fill
 * from the right end. The test always assigns the return value
 * to a variable before reading.
 */

#include "config.h"
#include <string.h>
#include <glib.h>
#include "human_readable.h"

/* All sizes under 1024 stay as raw decimal — that's what makes
 * the "0", "1", "100", "1023" outputs natural. */
static void
test_under_threshold_is_raw_decimal (void)
{
    char buf[LONGEST_HUMAN_READABLE + 1];

    g_assert_cmpstr (human_size (buf, 0), ==, "0");
    g_assert_cmpstr (human_size (buf, 1), ==, "1");
    g_assert_cmpstr (human_size (buf, 100), ==, "100");
    g_assert_cmpstr (human_size (buf, 500), ==, "500");
    g_assert_cmpstr (human_size (buf, 1023), ==, "1023");
}

/* 1024 is exactly the boundary — the first byte that switches to
 * suffix form. Pin the "1.0k" rendering. */
static void
test_1024_renders_as_1_0k (void)
{
    char buf[LONGEST_HUMAN_READABLE + 1];
    g_assert_cmpstr (human_size (buf, 1024), ==, "1.0k");
}

/* 1.5 KiB and 8.3 KiB — the two fractional-display cases that
 * exercise the .5-rounding state machine on the way into the
 * suffix form. 8500 → 8.3k is the example from the original
 * fileutils comment. */
static void
test_kib_fractional_render (void)
{
    char buf[LONGEST_HUMAN_READABLE + 1];

    g_assert_cmpstr (human_size (buf, 1500), ==, "1.5k");
    g_assert_cmpstr (human_size (buf, 8500), ==, "8.3k");
}

/* 64 KiB - 1 byte and 64 KiB pin the high end of the k-suffix
 * regime. 65535 already rounds up to 64.0k. */
static void
test_kib_high_end (void)
{
    char buf[LONGEST_HUMAN_READABLE + 1];
    g_assert_cmpstr (human_size (buf, 65535), ==, "64.0k");
}

/* The M boundary — same shape as the k boundary, one power up. */
static void
test_mib_boundary (void)
{
    char buf[LONGEST_HUMAN_READABLE + 1];
    g_assert_cmpstr (human_size (buf, 1048576u), ==, "1.0M");
    g_assert_cmpstr (human_size (buf, 1048576u + 524288u), ==, "1.5M");
    g_assert_cmpstr (human_size (buf, 100u * 1024u * 1024u), ==, "100.0M");
}

/* The G boundary — same again, two powers up. The 4 GiB case is
 * the largest non-overflow value a guint32 can carry; pin it so
 * a future widening to guint64 lands cleanly. */
static void
test_gib_boundary (void)
{
    char buf[LONGEST_HUMAN_READABLE + 1];
    g_assert_cmpstr (human_size (buf, 1024u * 1024u * 1024u), ==, "1.0G");
    g_assert_cmpstr (human_size (buf, 2u * 1024u * 1024u * 1024u), ==, "2.0G");
    g_assert_cmpstr (human_size (buf, 4294967295u), ==, "4.0G");
}

/* The output buffer is filled from the right; the *return value*,
 * not the buffer base, is the string the caller should print. Some
 * historical call sites got this wrong and printed garbage. Pin
 * the contract so any refactor that returns the wrong end of the
 * buffer breaks loudly. */
static void
test_returns_pointer_into_buf (void)
{
    char buf[LONGEST_HUMAN_READABLE + 1];

    char *r = human_size (buf, 12345);
    /* Must point somewhere inside the buffer. */
    g_assert_true (r >= buf);
    g_assert_true (r <= buf + LONGEST_HUMAN_READABLE);
    /* And must produce the right string. */
    g_assert_cmpstr (r, ==, "12.1k");
}

/* human_readable with output_block_size > 0 (positive) takes the
 * "raw decimal in those units" path. Exercise it with the
 * canonical from=1, to=1024 case: a byte count converted to KiB
 * stays as an integer. */
static void
test_human_readable_positive_output_block (void)
{
    char buf[LONGEST_HUMAN_READABLE + 1];

    /* 5120 bytes / 1024 = 5 (raw, no suffix). */
    char *r = human_readable (5120, buf, 1, 1024);
    g_assert_cmpstr (r, ==, "5");
}

/* human_readable with from_block_size > 1 (the call site reads
 * pre-aggregated counts in larger units). 100 in units of 1024
 * = 102400 raw bytes; with -1024 abbreviation, that's "100.0k". */
static void
test_human_readable_nontrivial_from_block (void)
{
    char buf[LONGEST_HUMAN_READABLE + 1];
    char *r = human_readable (100, buf, /*from=*/1024, /*output=*/-1024);
    g_assert_cmpstr (r, ==, "100.0k");
}

/* The suffix table is the contract for which letter each power
 * gets. Pin them — if anyone reorders the table the M/G/T
 * boundaries quietly start labeling things wrong. */
static void
test_suffix_letters_are_stable (void)
{
    g_assert_cmphex (human_suffixes[0], ==, 0); /* unused */
    g_assert_cmphex (human_suffixes[1], ==, 'k');
    g_assert_cmphex (human_suffixes[2], ==, 'M');
    g_assert_cmphex (human_suffixes[3], ==, 'G');
    g_assert_cmphex (human_suffixes[4], ==, 'T');
    g_assert_cmphex (human_suffixes[5], ==, 'P');
    g_assert_cmphex (human_suffixes[6], ==, 'E');
    g_assert_cmphex (human_suffixes[7], ==, 'Z');
    g_assert_cmphex (human_suffixes[8], ==, 'Y');
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/human_readable/under_threshold_is_raw_decimal",
                     test_under_threshold_is_raw_decimal);
    g_test_add_func ("/human_readable/1024_renders_as_1_0k",
                     test_1024_renders_as_1_0k);
    g_test_add_func ("/human_readable/kib_fractional_render",
                     test_kib_fractional_render);
    g_test_add_func ("/human_readable/kib_high_end", test_kib_high_end);
    g_test_add_func ("/human_readable/mib_boundary", test_mib_boundary);
    g_test_add_func ("/human_readable/gib_boundary", test_gib_boundary);
    g_test_add_func ("/human_readable/returns_pointer_into_buf",
                     test_returns_pointer_into_buf);
    g_test_add_func ("/human_readable/positive_output_block",
                     test_human_readable_positive_output_block);
    g_test_add_func ("/human_readable/nontrivial_from_block",
                     test_human_readable_nontrivial_from_block);
    g_test_add_func ("/human_readable/suffix_letters_are_stable",
                     test_suffix_letters_are_stable);

    return g_test_run ();
}
