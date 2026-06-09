/*
 * tests/unit/test_ui_scale.c — verify the gtkhx_ui_scale() inline
 * helper in gtkutil.h reads gtkhx_prefs.ui_scale_pct, clamps it to
 * the documented [UI_SCALE_PCT_MIN, UI_SCALE_PCT_MAX] range, and
 * returns the percentage as a double scale factor.
 *
 * gtkhx_ui_scale is the single chokepoint every consumer (users_view
 * pixel_scale, files_panel / news_browser icon sizing, gtkutil
 * pixmap-button construction, options.c font CSS) calls, so a wrong
 * clamp value here would corrupt rendering across the whole UI. The
 * inline lives in gtkutil.h so we don't need to link any of gtkhx —
 * just define the gtkhx_prefs storage and call it.
 */

#include "config.h"
#include <glib.h>

/* The inline reaches into gtkhx_prefs.ui_scale_pct; define the struct
 * storage here so the linker has the symbol. Other fields go
 * unreferenced and stay zero. prefs.h pulls in cfgkeys.h transitively
 * for the UI_SCALE_PCT_MIN / MAX / DEFAULT constants we reference. */
#include "prefs.h"
struct gtkhx_prefs gtkhx_prefs;

static void
test_default_pct_is_100 (void)
{
    gtkhx_prefs.ui_scale_pct = UI_SCALE_PCT_DEFAULT;
    g_assert_cmpfloat (gtkhx_ui_scale (), ==, 1.0);
}

static void
test_125_pct_returns_1_25 (void)
{
    gtkhx_prefs.ui_scale_pct = 125;
    g_assert_cmpfloat (gtkhx_ui_scale (), ==, 1.25);
}

static void
test_below_min_clamps_to_default (void)
{
    gtkhx_prefs.ui_scale_pct = UI_SCALE_PCT_MIN - 1;
    /* A corrupt or hand-edited gtkhxrc reading "10" for ui_scale
	 * would otherwise shrink icons to 0.1×. The clamp catches that
	 * and falls back to 100%, the documented safe default. */
    g_assert_cmpfloat (gtkhx_ui_scale (), ==, 1.0);
}

static void
test_above_max_clamps_to_default (void)
{
    gtkhx_prefs.ui_scale_pct = UI_SCALE_PCT_MAX + 1;
    /* Mirror of the below-min case — a runaway value (e.g. 500%)
	 * doesn't get fed into icon rebuilds that could allocate huge
	 * pixbufs. */
    g_assert_cmpfloat (gtkhx_ui_scale (), ==, 1.0);
}

static void
test_zero_clamps_to_default (void)
{
    gtkhx_prefs.ui_scale_pct = 0;
    g_assert_cmpfloat (gtkhx_ui_scale (), ==, 1.0);
}

static void
test_negative_clamps_to_default (void)
{
    gtkhx_prefs.ui_scale_pct = -100;
    g_assert_cmpfloat (gtkhx_ui_scale (), ==, 1.0);
}

static void
test_min_exact_is_honored (void)
{
    gtkhx_prefs.ui_scale_pct = UI_SCALE_PCT_MIN;
    g_assert_cmpfloat (gtkhx_ui_scale (), ==, UI_SCALE_PCT_MIN / 100.0);
}

static void
test_max_exact_is_honored (void)
{
    gtkhx_prefs.ui_scale_pct = UI_SCALE_PCT_MAX;
    g_assert_cmpfloat (gtkhx_ui_scale (), ==, UI_SCALE_PCT_MAX / 100.0);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/ui_scale/default_pct_is_100", test_default_pct_is_100);
    g_test_add_func ("/ui_scale/125_pct_returns_1_25", test_125_pct_returns_1_25);
    g_test_add_func ("/ui_scale/below_min_clamps_to_default",
                     test_below_min_clamps_to_default);
    g_test_add_func ("/ui_scale/above_max_clamps_to_default",
                     test_above_max_clamps_to_default);
    g_test_add_func ("/ui_scale/zero_clamps_to_default",
                     test_zero_clamps_to_default);
    g_test_add_func ("/ui_scale/negative_clamps_to_default",
                     test_negative_clamps_to_default);
    g_test_add_func ("/ui_scale/min_exact_is_honored",
                     test_min_exact_is_honored);
    g_test_add_func ("/ui_scale/max_exact_is_honored",
                     test_max_exact_is_honored);

    return g_test_run ();
}
