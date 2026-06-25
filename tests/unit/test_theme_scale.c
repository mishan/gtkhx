/*
 * test_theme_scale — unit coverage for the per-area UI scale model in
 * gtkhx_theme.c. Pins the honest "default theme" factors, the
 * unset→default fallback, override clamping, and the change signal so
 * a regression in any of them fails CI rather than silently shipping a
 * mis-scaled UI. See docs/theming-scoping.md.
 */
#include "config.h"

#include <glib.h>

#include "hx.h"
#include "gtkhx_theme.h"

/* The theme reads its override values out of gtkhx_prefs, which the
 * app defines in options.c. The unit test owns the global instead so
 * we can drive the scale_* fields directly without linking the
 * GTK-heavy options.c. */
struct gtkhx_prefs gtkhx_prefs;

static void
test_clamp (void)
{
    g_assert_cmpint (gtkhx_theme_clamp_percent (100), ==, 100);
    g_assert_cmpint (gtkhx_theme_clamp_percent (GTKHX_SCALE_MIN), ==,
                     GTKHX_SCALE_MIN);
    g_assert_cmpint (gtkhx_theme_clamp_percent (GTKHX_SCALE_MAX), ==,
                     GTKHX_SCALE_MAX);
    g_assert_cmpint (gtkhx_theme_clamp_percent (GTKHX_SCALE_MIN - 1), ==,
                     GTKHX_SCALE_MIN);
    g_assert_cmpint (gtkhx_theme_clamp_percent (GTKHX_SCALE_MAX + 1), ==,
                     GTKHX_SCALE_MAX);
    g_assert_cmpint (gtkhx_theme_clamp_percent (-5), ==, GTKHX_SCALE_MIN);
}

/* The default theme encodes the real factors GtkHx has always applied
 * — the whole point of the correction that the 2x / 1.25x are NOT 100%. */
static void
test_default_theme (void)
{
    g_assert_cmpint (gtkhx_theme_get_default_percent (GTKHX_SCALE_TOOLBAR), ==,
                     200);
    g_assert_cmpint (
        gtkhx_theme_get_default_percent (GTKHX_SCALE_WINDOW_BUTTONS), ==, 200);
    g_assert_cmpint (
        gtkhx_theme_get_default_percent (GTKHX_SCALE_USERLIST_ICON), ==, 125);
    g_assert_cmpint (
        gtkhx_theme_get_default_percent (GTKHX_SCALE_USERLIST_TEXT), ==, 125);
}

/* 0 (fresh prefs / pre-feature gtkhxrc) means "no override" → the
 * effective value is the default theme's, so an upgrade reproduces
 * today's look. */
static void
test_unset_falls_back_to_default (void)
{
    gtkhx_prefs.scale_toolbar = 0;
    gtkhx_prefs.scale_userlist_icon = 0;
    gtkhx_prefs.scale_userlist_text = 0;
    gtkhx_prefs.scale_window_buttons = 0;

    g_assert_cmpint (gtkhx_theme_get_percent (GTKHX_SCALE_TOOLBAR), ==, 200);
    g_assert_cmpfloat (gtkhx_theme_scale (GTKHX_SCALE_TOOLBAR), ==, 2.0);
    g_assert_cmpint (gtkhx_theme_get_percent (GTKHX_SCALE_USERLIST_ICON), ==,
                     125);
    g_assert_cmpfloat (gtkhx_theme_scale (GTKHX_SCALE_USERLIST_ICON), ==, 1.25);

    /* A negative leftover from a corrupt file is treated as unset too. */
    gtkhx_prefs.scale_toolbar = -3;
    g_assert_cmpint (gtkhx_theme_get_percent (GTKHX_SCALE_TOOLBAR), ==, 200);
}

static void
test_override (void)
{
    gtkhx_prefs.scale_toolbar = 150;
    g_assert_cmpint (gtkhx_theme_get_percent (GTKHX_SCALE_TOOLBAR), ==, 150);
    g_assert_cmpfloat (gtkhx_theme_scale (GTKHX_SCALE_TOOLBAR), ==, 1.5);

    /* An out-of-range override is clamped on read. */
    gtkhx_prefs.scale_toolbar = 5000;
    g_assert_cmpint (gtkhx_theme_get_percent (GTKHX_SCALE_TOOLBAR), ==,
                     GTKHX_SCALE_MAX);
}

static int changed_count;

static void
on_changed (GtkhxTheme *theme, gpointer data)
{
    (void)theme;
    (void)data;
    changed_count++;
}

static void
test_set_percent_writes_and_emits (void)
{
    GtkhxTheme *theme = gtkhx_theme_get_default ();

    g_assert_nonnull (theme);
    g_signal_connect (theme, "changed", G_CALLBACK (on_changed), NULL);

    gtkhx_prefs.scale_window_buttons = 0;
    changed_count = 0;

    /* A real change writes the override and emits once. */
    gtkhx_theme_set_percent (GTKHX_SCALE_WINDOW_BUTTONS, 175);
    g_assert_cmpint (gtkhx_prefs.scale_window_buttons, ==, 175);
    g_assert_cmpint (changed_count, ==, 1);

    /* Setting the same clamped value again is a no-op (no emission). */
    gtkhx_theme_set_percent (GTKHX_SCALE_WINDOW_BUTTONS, 175);
    g_assert_cmpint (changed_count, ==, 1);

    /* Out-of-range set is clamped before storing, and that counts as a
     * change from 175. */
    gtkhx_theme_set_percent (GTKHX_SCALE_WINDOW_BUTTONS, 10);
    g_assert_cmpint (gtkhx_prefs.scale_window_buttons, ==, GTKHX_SCALE_MIN);
    g_assert_cmpint (changed_count, ==, 2);

    /* pct <= 0 clears the override (stores the 0 sentinel) so the area
     * reverts to the default theme, and emits the change. */
    gtkhx_theme_set_percent (GTKHX_SCALE_WINDOW_BUTTONS, 0);
    g_assert_cmpint (gtkhx_prefs.scale_window_buttons, ==, 0);
    g_assert_cmpint (changed_count, ==, 3);
    g_assert_cmpint (gtkhx_theme_get_percent (GTKHX_SCALE_WINDOW_BUTTONS), ==,
                     200);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/theme/clamp", test_clamp);
    g_test_add_func ("/theme/default", test_default_theme);
    g_test_add_func ("/theme/unset-default", test_unset_falls_back_to_default);
    g_test_add_func ("/theme/override", test_override);
    g_test_add_func ("/theme/set-emit", test_set_percent_writes_and_emits);
    return g_test_run ();
}
