/*
 * tests/unit/test_hl_date.c — pin the Hotline 8-byte timestamp
 * decoder. The Capabilities.md spec defines two competing wire
 * formats:
 *
 *   Mac 1904 epoch  year == 1904, secs = total seconds since
 *                   1904-01-01 00:00:00 UTC. Vintage Mac servers,
 *                   mhxd, Mobius default.
 *   Modern          year == actual year (e.g. 2026), secs = seconds
 *                   since Jan 1 of that year, local time. Servers
 *                   that see DATA_CAPABILITIES from the client
 *                   switch to this format because the 1904 epoch's
 *                   u32 seconds field overflows in 2040.
 *
 * hl_date_decode auto-detects via the year field: year==1904 → Mac
 * epoch; otherwise modern. Tests pin both branches plus the
 * sentinel-zero / out-of-range refusal paths.
 *
 * Calendar-arithmetic note: the modern-format tests use mktime to
 * compute the expected value the same way the decoder does, so
 * the assertion isn't sensitive to the test host's local-time
 * zone. The 1904-format tests are absolute (UTC arithmetic on a
 * u32) so they don't need that dance.
 */

#include "config.h"
#include <string.h>
#include <time.h>
#include <glib.h>
#include "hl_date.h"

/* ---------- Mac 1904 epoch decode ---------- */

/* Helper: build the 8-byte wire layout from explicit fields, big-
 * endian. */
static void
pack (guint8 buf[8], guint16 year, guint16 msecs, guint32 seconds)
{
    buf[0] = (guint8) (year >> 8);
    buf[1] = (guint8) (year & 0xff);
    buf[2] = (guint8) (msecs >> 8);
    buf[3] = (guint8) (msecs & 0xff);
    buf[4] = (guint8) (seconds >> 24);
    buf[5] = (guint8) (seconds >> 16);
    buf[6] = (guint8) (seconds >> 8);
    buf[7] = (guint8) (seconds & 0xff);
}

/* year=1904, secs=0x7c25b080 (= 2082844800 = the 1904→1970 epoch
 * offset). Result must be Unix t=0 (1970-01-01 00:00:00 UTC). */
static void
test_mac_1904_epoch_unix_zero (void)
{
    guint8 buf[8];
    pack (buf, 1904, 0, 2082844800u);
    time_t t = 12345;
    g_assert_true (hl_date_decode (buf, &t));
    g_assert_cmpint ((long long)t, ==, 0);
}

/* year=1904, secs = 2082844800 + 86400 = 2082931200. That's
 * 1970-01-02 00:00:00 UTC, Unix t=86400. */
static void
test_mac_1904_epoch_one_day_after_unix_zero (void)
{
    guint8 buf[8];
    pack (buf, 1904, 0, 2082844800u + 86400u);
    time_t t = 0;
    g_assert_true (hl_date_decode (buf, &t));
    g_assert_cmpint ((long long)t, ==, 86400);
}

/* Real-world-ish: a 1904-epoch timestamp from 2026-05-15. Compute
 * what the wire seconds field would be and round-trip through the
 * decoder. */
static void
test_mac_1904_epoch_modern_date (void)
{
    /* Pick 2026-05-15 00:00:00 UTC. */
    struct tm utm = { 0 };
    utm.tm_year = 2026 - 1900;
    utm.tm_mon = 5 - 1;
    utm.tm_mday = 15;
    /* timegm is GNU; the test_text_util etc. tests use it so we
	 * can assume it's available. */
    time_t target = timegm (&utm);

    /* Encode as 1904 epoch seconds. */
    guint32 wire_secs = (guint32)target + 2082844800u;

    guint8 buf[8];
    pack (buf, 1904, 0, wire_secs);
    time_t t = 0;
    g_assert_true (hl_date_decode (buf, &t));
    g_assert_cmpint ((long long)t, ==, (long long)target);
}

/* ---------- Modern format decode ---------- */

/* Modern format: year = actual year, secs = seconds since Jan 1 of
 * that year in LOCAL time. To make this timezone-independent, we
 * compute the expected time_t locally using the same mktime path
 * the decoder uses. */
static void
test_modern_format_jan_1 (void)
{
    /* secs=0 against year=2026 = Jan 1, 2026 00:00:00 local time. */
    struct tm expected_tm = { 0 };
    expected_tm.tm_year = 2026 - 1900;
    expected_tm.tm_mon = 0;
    expected_tm.tm_mday = 1;
    expected_tm.tm_isdst = -1;
    time_t expected = mktime (&expected_tm);

    /* But secs == 0 is the sentinel for "no timestamp set" — both
	 * formats agree on it. The decoder refuses. */
    guint8 buf[8];
    pack (buf, 2026, 0, 0);
    time_t t = 12345;
    g_assert_false (hl_date_decode (buf, &t));

    /* Try secs=1 instead so we get out of the sentinel zone. */
    pack (buf, 2026, 0, 1);
    g_assert_true (hl_date_decode (buf, &t));
    g_assert_cmpint ((long long)t, ==, (long long)expected + 1);
}

/* Modern format mid-year: secs=86400 means Jan 2 of `year` 00:00. */
static void
test_modern_format_one_day_into_year (void)
{
    struct tm expected_tm = { 0 };
    expected_tm.tm_year = 2026 - 1900;
    expected_tm.tm_mon = 0;
    expected_tm.tm_mday = 2;
    expected_tm.tm_isdst = -1;
    time_t expected = mktime (&expected_tm);

    guint8 buf[8];
    pack (buf, 2026, 0, 86400);
    time_t t = 0;
    g_assert_true (hl_date_decode (buf, &t));
    g_assert_cmpint ((long long)t, ==, (long long)expected);
}

/* Random earlier-modern-era year — 1990, ~30 days in. */
static void
test_modern_format_1990 (void)
{
    struct tm expected_tm = { 0 };
    expected_tm.tm_year = 1990 - 1900;
    expected_tm.tm_mon = 0;
    expected_tm.tm_mday = 1;
    expected_tm.tm_isdst = -1;
    time_t base = mktime (&expected_tm);

    guint32 offset_secs = 30 * 86400 + 3600;
    guint8 buf[8];
    pack (buf, 1990, 0, offset_secs);
    time_t t = 0;
    g_assert_true (hl_date_decode (buf, &t));
    g_assert_cmpint ((long long)t, ==, (long long)base + offset_secs);
}

/* ---------- Sentinel / refusal cases ---------- */

/* All-zero buffer → seconds==0, both formats agree on the "no
 * timestamp" sentinel. Decoder refuses. */
static void
test_all_zero_input_refuses (void)
{
    guint8 buf[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    time_t t = 42;
    g_assert_false (hl_date_decode (buf, &t));
}

/* secs=0 with a sane year → still refused (no-timestamp sentinel). */
static void
test_zero_seconds_with_year_refuses (void)
{
    guint8 buf[8];
    pack (buf, 1904, 0, 0);
    time_t t = 42;
    g_assert_false (hl_date_decode (buf, &t));
    pack (buf, 2026, 0, 0);
    g_assert_false (hl_date_decode (buf, &t));
}

/* Year outside the plausible Hotline-era range → refuse rather
 * than do nonsense year arithmetic. The decoder caps at
 * [1970, 2200]; 1969 and 2201 are over the line. */
static void
test_year_out_of_range_refuses (void)
{
    guint8 buf[8];
    time_t t = 42;
    pack (buf, 1969, 0, 1);
    g_assert_false (hl_date_decode (buf, &t));
    pack (buf, 2201, 0, 1);
    g_assert_false (hl_date_decode (buf, &t));
    pack (buf, 0, 0, 1);
    g_assert_false (hl_date_decode (buf, &t));
}

/* NULL buf or NULL out → refuse cleanly, no crash. */
static void
test_null_inputs_refuse_cleanly (void)
{
    guint8 buf[8];
    pack (buf, 2026, 0, 1);
    g_assert_false (hl_date_decode (NULL, NULL));
    time_t t = 0;
    g_assert_false (hl_date_decode (NULL, &t));
    g_assert_false (hl_date_decode (buf, NULL));
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/hl_date/mac_1904_epoch_unix_zero",
                     test_mac_1904_epoch_unix_zero);
    g_test_add_func ("/hl_date/mac_1904_epoch_one_day_after_unix_zero",
                     test_mac_1904_epoch_one_day_after_unix_zero);
    g_test_add_func ("/hl_date/mac_1904_epoch_modern_date",
                     test_mac_1904_epoch_modern_date);

    g_test_add_func ("/hl_date/modern_format_jan_1", test_modern_format_jan_1);
    g_test_add_func ("/hl_date/modern_format_one_day_into_year",
                     test_modern_format_one_day_into_year);
    g_test_add_func ("/hl_date/modern_format_1990", test_modern_format_1990);

    g_test_add_func ("/hl_date/all_zero_input_refuses",
                     test_all_zero_input_refuses);
    g_test_add_func ("/hl_date/zero_seconds_with_year_refuses",
                     test_zero_seconds_with_year_refuses);
    g_test_add_func ("/hl_date/year_out_of_range_refuses",
                     test_year_out_of_range_refuses);
    g_test_add_func ("/hl_date/null_inputs_refuse_cleanly",
                     test_null_inputs_refuse_cleanly);

    return g_test_run ();
}
