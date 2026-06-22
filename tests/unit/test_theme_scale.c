/*
 * test_theme_scale — unit coverage for GtkhxTheme: the per-area
 * UI-scale model AND the xtext UI-role palette. Pins the honest
 * "default theme" factors and colors, the GKeyFile loader's
 * round-trip + fallback semantics, clamping, missing-key handling,
 * and the change signal so a regression in any of them fails CI
 * rather than silently shipping a mis-themed UI.
 *
 * See docs/theming-scoping.md and docs/theming-file-format.md.
 */
#include "config.h"

#include <glib.h>

#include "hx.h"
#include "gtkhx_theme.h"

/* The singleton reads its overrides out of its own internal state
 * now (no more gtkhx_prefs.scale_*); we still need to provide a
 * gtkhx_prefs symbol because gtkhx_theme.c references theme_name
 * when resolving the active-theme path. The test never touches the
 * filesystem — it drives the loader via gtkhx_theme_load_from_keyfile
 * instead — so the field can stay NULL throughout. */
struct gtkhx_prefs gtkhx_prefs;

/* gtkhx_theme.c calls gtkhx_config_dir() from active_theme_path(),
 * which is reachable only via gtkhx_theme_load_active(). The unit
 * tests don't exercise that path (they drive the loader from
 * in-memory GKeyFiles), but the linker still needs the symbol.
 * Provide a stub rather than pulling gtkhx.c (and all of GTK) into
 * the test target. */
extern const char *gtkhx_config_dir (void);
const char *
gtkhx_config_dir (void)
{
    return "/tmp/gtkhx-test-not-used";
}

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

/* The default theme encodes the real factors GtkHx has always
 * applied — the whole point of the correction that 2× / 1.25× are
 * NOT 100%. */
static void
test_default_scale (void)
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

/* GdkRGBA fields are `float`, not `double`, so a round-trip from a
 * double literal through the struct loses LSBs. Compare via packed
 * 8-bit bytes (which is the on-wire shape anyway) to dodge the
 * IEEE-precision rathole entirely. */
static void
assert_rgba_eq_bytes (GdkRGBA c, int r, int g, int b)
{
    int cr = (int) (c.red   * 255.0 + 0.5);
    int cg = (int) (c.green * 255.0 + 0.5);
    int cb = (int) (c.blue  * 255.0 + 0.5);
    g_assert_cmpint (cr, ==, r);
    g_assert_cmpint (cg, ==, g);
    g_assert_cmpint (cb, ==, b);
}

/* Built-in default palette matches the historical chat.c constants
 * byte-for-byte — that's the contract that lets the theme-file
 * refactor land without changing how a fresh install looks. */
static void
test_default_palette (void)
{
    /* Light: FG = #1d1d1d on BG = #fafafa, the Adwaita-aligned set. */
    assert_rgba_eq_bytes (gtkhx_theme_get_default_color (GTKHX_PAL_FG, FALSE),
                          0x1d, 0x1d, 0x1d);
    assert_rgba_eq_bytes (gtkhx_theme_get_default_color (GTKHX_PAL_BG, FALSE),
                          0xfa, 0xfa, 0xfa);
    assert_rgba_eq_bytes (
        gtkhx_theme_get_default_color (GTKHX_PAL_MARK_FG, FALSE),
        0xff, 0xff, 0xff);
    assert_rgba_eq_bytes (
        gtkhx_theme_get_default_color (GTKHX_PAL_MARK_BG, FALSE),
        0x35, 0x84, 0xe4);
    assert_rgba_eq_bytes (
        gtkhx_theme_get_default_color (GTKHX_PAL_HISTORY_MUTED, FALSE),
        0x5e, 0x5e, 0x5e);

    /* Dark: FG = #cccccc, BG = #000000 — the original look. */
    assert_rgba_eq_bytes (gtkhx_theme_get_default_color (GTKHX_PAL_FG, TRUE),
                          0xcc, 0xcc, 0xcc);
    assert_rgba_eq_bytes (gtkhx_theme_get_default_color (GTKHX_PAL_BG, TRUE),
                          0x00, 0x00, 0x00);
    assert_rgba_eq_bytes (
        gtkhx_theme_get_default_color (GTKHX_PAL_MARK_BG, TRUE),
        0x20, 0x4a, 0x87);
    assert_rgba_eq_bytes (
        gtkhx_theme_get_default_color (GTKHX_PAL_HISTORY_MUTED, TRUE),
        0x9a, 0x9a, 0x9a);

    /* MARKER is theme-agnostic in the default — same red on both
     * variants. */
    assert_rgba_eq_bytes (
        gtkhx_theme_get_default_color (GTKHX_PAL_MARKER, FALSE),
        0xcc, 0x00, 0x00);
    assert_rgba_eq_bytes (
        gtkhx_theme_get_default_color (GTKHX_PAL_MARKER, TRUE),
        0xcc, 0x00, 0x00);
}

/* Loading an empty GKeyFile clears all overrides → every accessor
 * returns the built-in default. This is also the contract for the
 * "active theme file missing keys" case: omit a key, inherit the
 * default. */
static void
test_load_empty_keyfile_uses_defaults (void)
{
    GKeyFile *kf = g_key_file_new ();

    gtkhx_theme_load_from_keyfile (kf);

    g_assert_cmpint (gtkhx_theme_get_percent (GTKHX_SCALE_TOOLBAR), ==, 200);
    g_assert_cmpfloat (gtkhx_theme_scale (GTKHX_SCALE_USERLIST_ICON), ==, 1.25);

    assert_rgba_eq_bytes (gtkhx_theme_get_color (GTKHX_PAL_FG, FALSE),
                          0x1d, 0x1d, 0x1d);

    g_key_file_free (kf);
}

/* A theme that sets [scale] overrides drives gtkhx_theme_scale. */
static void
test_load_scale_overrides (void)
{
    GKeyFile *kf = g_key_file_new ();

    g_key_file_set_integer (kf, "scale", "toolbar", 150);
    g_key_file_set_integer (kf, "scale", "userlist_icon", 100);

    gtkhx_theme_load_from_keyfile (kf);

    g_assert_cmpint (gtkhx_theme_get_percent (GTKHX_SCALE_TOOLBAR), ==, 150);
    g_assert_cmpfloat (gtkhx_theme_scale (GTKHX_SCALE_TOOLBAR), ==, 1.5);
    g_assert_cmpint (gtkhx_theme_get_percent (GTKHX_SCALE_USERLIST_ICON), ==,
                     100);
    /* Unspecified keys keep their default. */
    g_assert_cmpint (gtkhx_theme_get_percent (GTKHX_SCALE_WINDOW_BUTTONS), ==,
                     200);

    g_key_file_free (kf);
}

/* Scale values outside [50, 300] are clamped at load time so the
 * stored override is the in-range boundary, not a silently-broken
 * float. */
static void
test_load_scale_clamps (void)
{
    GKeyFile *kf = g_key_file_new ();

    g_key_file_set_integer (kf, "scale", "toolbar", 5000);
    g_key_file_set_integer (kf, "scale", "userlist_text", 10);

    gtkhx_theme_load_from_keyfile (kf);

    g_assert_cmpint (gtkhx_theme_get_percent (GTKHX_SCALE_TOOLBAR), ==,
                     GTKHX_SCALE_MAX);
    g_assert_cmpint (gtkhx_theme_get_percent (GTKHX_SCALE_USERLIST_TEXT), ==,
                     GTKHX_SCALE_MIN);

    g_key_file_free (kf);
}

/* Palette round-trip: hex strings in [palette.light] / [palette.dark]
 * become GdkRGBA via gtkhx_theme_get_color. */
static void
test_load_palette (void)
{
    GKeyFile *kf = g_key_file_new ();

    g_key_file_set_string (kf, "palette.light", "fg", "#112233");
    g_key_file_set_string (kf, "palette.light", "bg", "#ffeedd");
    g_key_file_set_string (kf, "palette.dark", "fg", "#abcdef");
    /* Tolerate upper / lower / mixed case and a "#" prefix without
     * surprise. */
    g_key_file_set_string (kf, "palette.dark", "bg", "#0A0B0C");

    gtkhx_theme_load_from_keyfile (kf);

    assert_rgba_eq_bytes (gtkhx_theme_get_color (GTKHX_PAL_FG, FALSE),
                          0x11, 0x22, 0x33);
    assert_rgba_eq_bytes (gtkhx_theme_get_color (GTKHX_PAL_BG, FALSE),
                          0xff, 0xee, 0xdd);
    assert_rgba_eq_bytes (gtkhx_theme_get_color (GTKHX_PAL_FG, TRUE),
                          0xab, 0xcd, 0xef);
    assert_rgba_eq_bytes (gtkhx_theme_get_color (GTKHX_PAL_BG, TRUE),
                          0x0a, 0x0b, 0x0c);

    /* MARK_FG was not in the file → inherits the built-in default
     * (#ffffff on light). */
    assert_rgba_eq_bytes (gtkhx_theme_get_color (GTKHX_PAL_MARK_FG, FALSE),
                          0xff, 0xff, 0xff);

    g_key_file_free (kf);
}

/* Malformed hex values fall back to the built-in default for that
 * slot — the rest of the file still loads. We expect a g_warning
 * for each bad value; the test environment treats them as
 * non-fatal so we can assert on the resulting state. */
static void
test_load_palette_bad_hex_falls_back (void)
{
    GKeyFile *kf = g_key_file_new ();

    g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                           "gtkhx_theme: bad color*");
    g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                           "gtkhx_theme: bad color*");

    g_key_file_set_string (kf, "palette.light", "fg", "not a color");
    g_key_file_set_string (kf, "palette.light", "bg", "#ABCD");   /* too short */

    gtkhx_theme_load_from_keyfile (kf);

    /* Both bad slots silently fall back to defaults. */
    assert_rgba_eq_bytes (gtkhx_theme_get_color (GTKHX_PAL_FG, FALSE),
                          0x1d, 0x1d, 0x1d);
    assert_rgba_eq_bytes (gtkhx_theme_get_color (GTKHX_PAL_BG, FALSE),
                          0xfa, 0xfa, 0xfa);

    g_test_assert_expected_messages ();
    g_key_file_free (kf);
}

/* load_from_keyfile is a replacement, not a merge. A second load
 * resets every override that the new file doesn't mention. */
static void
test_load_replaces_not_merges (void)
{
    GKeyFile *kf1 = g_key_file_new ();
    GKeyFile *kf2 = g_key_file_new ();

    g_key_file_set_integer (kf1, "scale", "toolbar", 150);
    g_key_file_set_string  (kf1, "palette.light", "fg", "#112233");
    gtkhx_theme_load_from_keyfile (kf1);

    /* Confirm setup. */
    g_assert_cmpint (gtkhx_theme_get_percent (GTKHX_SCALE_TOOLBAR), ==, 150);

    /* Second load mentions only userlist_text — toolbar override
     * must vanish, fg must revert to the default. */
    g_key_file_set_integer (kf2, "scale", "userlist_text", 90);
    gtkhx_theme_load_from_keyfile (kf2);

    g_assert_cmpint (gtkhx_theme_get_percent (GTKHX_SCALE_TOOLBAR), ==, 200);
    g_assert_cmpint (gtkhx_theme_get_percent (GTKHX_SCALE_USERLIST_TEXT), ==,
                     90);
    assert_rgba_eq_bytes (gtkhx_theme_get_color (GTKHX_PAL_FG, FALSE),
                          0x1d, 0x1d, 0x1d);

    g_key_file_free (kf1);
    g_key_file_free (kf2);
}

static int changed_count;

static void
on_changed (GtkhxTheme *theme, gpointer data)
{
    (void)theme;
    (void)data;
    changed_count++;
}

/* Loading any keyfile (even a trivial one) fires the "changed"
 * signal exactly once so subscribers refresh in lockstep. */
static void
test_load_emits_changed (void)
{
    GtkhxTheme *theme = gtkhx_theme_get_default ();
    GKeyFile *kf = g_key_file_new ();
    gulong handler;

    g_assert_nonnull (theme);
    handler = g_signal_connect (theme, "changed", G_CALLBACK (on_changed),
                                NULL);
    changed_count = 0;

    gtkhx_theme_load_from_keyfile (kf);
    g_assert_cmpint (changed_count, ==, 1);

    gtkhx_theme_load_from_keyfile (kf);
    g_assert_cmpint (changed_count, ==, 2);

    g_signal_handler_disconnect (theme, handler);
    g_key_file_free (kf);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/theme/clamp", test_clamp);
    g_test_add_func ("/theme/default-scale", test_default_scale);
    g_test_add_func ("/theme/default-palette", test_default_palette);
    g_test_add_func ("/theme/load-empty-uses-defaults",
                     test_load_empty_keyfile_uses_defaults);
    g_test_add_func ("/theme/load-scale-overrides",
                     test_load_scale_overrides);
    g_test_add_func ("/theme/load-scale-clamps", test_load_scale_clamps);
    g_test_add_func ("/theme/load-palette", test_load_palette);
    g_test_add_func ("/theme/load-palette-bad-hex-falls-back",
                     test_load_palette_bad_hex_falls_back);
    g_test_add_func ("/theme/load-replaces-not-merges",
                     test_load_replaces_not_merges);
    g_test_add_func ("/theme/load-emits-changed", test_load_emits_changed);
    return g_test_run ();
}
