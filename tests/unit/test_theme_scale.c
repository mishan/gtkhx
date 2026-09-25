/*
 * test_theme_scale — unit coverage for GtkhxTheme: the per-area
 * UI-scale model AND the xtext UI-role palette. Pins the honest
 * "default theme" factors and colors, the GKeyFile loader's
 * round-trip + fallback semantics, clamping, missing-key handling,
 * and the change signal so a regression in any of them fails CI
 * rather than silently shipping a mis-themed UI.
 *
 * See docs/theming.md and docs/theming-file-format.md.
 */
#include "config.h"

#include <string.h>

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

/* GdkRGBA fields are `float`, not `double`, so a round-trip from a
 * double literal through the struct loses LSBs. Compare via packed
 * 8-bit bytes (which is the on-wire shape anyway) to dodge the
 * IEEE-precision rathole entirely. */
static void
assert_rgba_eq_bytes (GdkRGBA c, int r, int g, int b)
{
    int cr = (int)(c.red * 255.0 + 0.5);
    int cg = (int)(c.green * 255.0 + 0.5);
    int cb = (int)(c.blue * 255.0 + 0.5);
    g_assert_cmpint (cr, ==, r);
    g_assert_cmpint (cg, ==, g);
    g_assert_cmpint (cb, ==, b);
}

/* An unset fg/bg is "follow the system": fully transparent. */
static void
assert_rgba_is_system (GdkRGBA c)
{
    g_assert_cmpfloat (c.alpha, ==, 0.0);
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

    g_assert_cmpint (gtkhx_theme_get_percent (GTKHX_SCALE_TOOLBAR), ==, 100);
    g_assert_cmpfloat (gtkhx_theme_scale (GTKHX_SCALE_USERLIST_ICON), ==, 1.00);

    assert_rgba_is_system (gtkhx_theme_get_color (GTKHX_PAL_FG, FALSE));
    assert_rgba_is_system (gtkhx_theme_get_color (GTKHX_PAL_BG, TRUE));
    assert_rgba_eq_bytes (gtkhx_theme_get_color (GTKHX_PAL_MARK_BG, FALSE),
                          0x35, 0x84, 0xe4);

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
                     100);

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

    assert_rgba_eq_bytes (gtkhx_theme_get_color (GTKHX_PAL_FG, FALSE), 0x11,
                          0x22, 0x33);
    assert_rgba_eq_bytes (gtkhx_theme_get_color (GTKHX_PAL_BG, FALSE), 0xff,
                          0xee, 0xdd);
    assert_rgba_eq_bytes (gtkhx_theme_get_color (GTKHX_PAL_FG, TRUE), 0xab,
                          0xcd, 0xef);
    assert_rgba_eq_bytes (gtkhx_theme_get_color (GTKHX_PAL_BG, TRUE), 0x0a,
                          0x0b, 0x0c);

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
    g_key_file_set_string (kf, "palette.light", "bg", "#ABCD"); /* too short */

    gtkhx_theme_load_from_keyfile (kf);

    /* Both bad slots silently fall back to defaults. */
    assert_rgba_is_system (gtkhx_theme_get_color (GTKHX_PAL_FG, FALSE));
    assert_rgba_is_system (gtkhx_theme_get_color (GTKHX_PAL_BG, FALSE));

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
    g_key_file_set_string (kf1, "palette.light", "fg", "#112233");
    gtkhx_theme_load_from_keyfile (kf1);

    /* Confirm setup. */
    g_assert_cmpint (gtkhx_theme_get_percent (GTKHX_SCALE_TOOLBAR), ==, 150);

    /* Second load mentions only userlist_text — toolbar override
     * must vanish, fg must revert to the default. */
    g_key_file_set_integer (kf2, "scale", "userlist_text", 90);
    gtkhx_theme_load_from_keyfile (kf2);

    g_assert_cmpint (gtkhx_theme_get_percent (GTKHX_SCALE_TOOLBAR), ==, 100);
    g_assert_cmpint (gtkhx_theme_get_percent (GTKHX_SCALE_USERLIST_TEXT), ==,
                     90);
    assert_rgba_is_system (gtkhx_theme_get_color (GTKHX_PAL_FG, FALSE));

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
    handler
        = g_signal_connect (theme, "changed", G_CALLBACK (on_changed), NULL);
    changed_count = 0;

    gtkhx_theme_load_from_keyfile (kf);
    g_assert_cmpint (changed_count, ==, 1);

    gtkhx_theme_load_from_keyfile (kf);
    g_assert_cmpint (changed_count, ==, 2);

    g_signal_handler_disconnect (theme, handler);
    g_key_file_free (kf);
}

/* User-list name colors: theme keys [users.light]/[users.dark]
 * round-trip via gtkhx_theme_get_user_color; the accessor returns
 * FALSE for slots the theme didn't set (caller falls back to its
 * hardcoded default). */
static void
test_load_user_colors (void)
{
    GKeyFile *kf = g_key_file_new ();
    GdkRGBA out;

    g_key_file_set_string (kf, "users.light", "active", "#112233");
    g_key_file_set_string (kf, "users.light", "admin", "#dc322f");
    g_key_file_set_string (kf, "users.dark", "idle", "#445566");
    g_key_file_set_string (kf, "users.dark", "admin_idle", "#aabbcc");

    gtkhx_theme_load_from_keyfile (kf);

    /* Light: active + admin set, idle + admin_idle not. */
    g_assert_true (
        gtkhx_theme_get_user_color (GTKHX_USER_COLOR_ACTIVE, FALSE, &out));
    g_assert_cmpint ((int)(out.red * 255.0 + 0.5), ==, 0x11);
    g_assert_cmpint ((int)(out.green * 255.0 + 0.5), ==, 0x22);
    g_assert_cmpint ((int)(out.blue * 255.0 + 0.5), ==, 0x33);
    g_assert_true (
        gtkhx_theme_get_user_color (GTKHX_USER_COLOR_ADMIN, FALSE, &out));
    g_assert_cmpint ((int)(out.red * 255.0 + 0.5), ==, 0xdc);
    g_assert_false (
        gtkhx_theme_get_user_color (GTKHX_USER_COLOR_IDLE, FALSE, &out));
    g_assert_false (
        gtkhx_theme_get_user_color (GTKHX_USER_COLOR_ADMIN_IDLE, FALSE, &out));

    /* Dark: idle + admin_idle set, active + admin not. */
    g_assert_false (
        gtkhx_theme_get_user_color (GTKHX_USER_COLOR_ACTIVE, TRUE, &out));
    g_assert_false (
        gtkhx_theme_get_user_color (GTKHX_USER_COLOR_ADMIN, TRUE, &out));
    g_assert_true (
        gtkhx_theme_get_user_color (GTKHX_USER_COLOR_IDLE, TRUE, &out));
    g_assert_cmpint ((int)(out.red * 255.0 + 0.5), ==, 0x44);
    g_assert_true (
        gtkhx_theme_get_user_color (GTKHX_USER_COLOR_ADMIN_IDLE, TRUE, &out));
    g_assert_cmpint ((int)(out.red * 255.0 + 0.5), ==, 0xaa);

    g_key_file_free (kf);
}

static int
byte_of (double channel)
{
    return (int)(channel * 255.0 + 0.5);
}

/* A theme that sets neither chrome keys nor the chat palette's fg/bg
 * leaves the chrome to the system theme: no role resolves and the
 * generated CSS is empty. That is what keeps the default theme stock. */
static void
test_chrome_untinted_without_opt_in (void)
{
    GKeyFile *kf = g_key_file_new ();
    GdkRGBA out;
    char *css;
    int r;

    g_key_file_set_string (kf, "palette.dark", "marker", "#cc0000");
    gtkhx_theme_load_from_keyfile (kf);

    for (r = 0; r < GTKHX_CHROME_N_ROLES; r++) {
        g_assert_false (gtkhx_theme_get_chrome_color (r, TRUE, &out));
        g_assert_false (gtkhx_theme_get_chrome_color (r, FALSE, &out));
    }
    css = gtkhx_theme_build_chrome_css (TRUE);
    g_assert_cmpstr (css, ==, "");
    g_free (css);

    g_key_file_free (kf);
}

/* Chat palette fg/bg alone tint the chrome: window and view take bg,
 * text takes fg, and the layered surfaces step from bg toward fg, so
 * a theme written before [chrome.*] existed still gets a coherent
 * window. The accent has no derivation. */
static void
test_chrome_derives_from_palette (void)
{
    GKeyFile *kf = g_key_file_new ();
    GdkRGBA out;

    g_key_file_set_string (kf, "palette.dark", "bg", "#000000");
    g_key_file_set_string (kf, "palette.dark", "fg", "#ffffff");
    gtkhx_theme_load_from_keyfile (kf);

    g_assert_true (
        gtkhx_theme_get_chrome_color (GTKHX_CHROME_WINDOW, TRUE, &out));
    g_assert_cmpint (byte_of (out.red), ==, 0x00);
    g_assert_true (
        gtkhx_theme_get_chrome_color (GTKHX_CHROME_VIEW, TRUE, &out));
    g_assert_cmpint (byte_of (out.red), ==, 0x00);
    g_assert_true (gtkhx_theme_get_chrome_color (GTKHX_CHROME_FG, TRUE, &out));
    g_assert_cmpint (byte_of (out.red), ==, 0xff);
    g_assert_true (
        gtkhx_theme_get_chrome_color (GTKHX_CHROME_HEADERBAR_FG, TRUE, &out));
    g_assert_cmpint (byte_of (out.red), ==, 0xff);

    /* Layered surfaces sit a small step off the window, in order. */
    {
        GdkRGBA card, headerbar, popover, sidebar;

        g_assert_true (
            gtkhx_theme_get_chrome_color (GTKHX_CHROME_CARD, TRUE, &card));
        g_assert_true (gtkhx_theme_get_chrome_color (GTKHX_CHROME_HEADERBAR,
                                                     TRUE, &headerbar));
        g_assert_true (gtkhx_theme_get_chrome_color (GTKHX_CHROME_POPOVER, TRUE,
                                                     &popover));
        g_assert_true (gtkhx_theme_get_chrome_color (GTKHX_CHROME_SIDEBAR, TRUE,
                                                     &sidebar));
        g_assert_cmpint (byte_of (card.red), >, 0);
        g_assert_cmpint (byte_of (card.red), <, byte_of (headerbar.red));
        g_assert_cmpint (byte_of (headerbar.red), <, byte_of (popover.red));
        g_assert_cmpint (byte_of (popover.red), <, 0x40);
        g_assert_cmpint (byte_of (sidebar.red), ==, byte_of (headerbar.red));
    }

    g_assert_false (
        gtkhx_theme_get_chrome_color (GTKHX_CHROME_ACCENT, TRUE, &out));
    /* Opting in on dark says nothing about light. */
    g_assert_false (
        gtkhx_theme_get_chrome_color (GTKHX_CHROME_WINDOW, FALSE, &out));

    g_key_file_free (kf);
}

/* Explicit [chrome.*] keys win over derivation, and a derived role
 * follows the explicit role it derives from (view follows window). */
static void
test_chrome_explicit_keys_win (void)
{
    GKeyFile *kf = g_key_file_new ();
    GdkRGBA out;

    g_key_file_set_string (kf, "palette.light", "bg", "#ffffff");
    g_key_file_set_string (kf, "chrome.light", "window", "#fdf6e3");
    g_key_file_set_string (kf, "chrome.light", "headerbar", "#eee8d5");
    g_key_file_set_string (kf, "chrome.light", "accent", "#268bd2");
    g_key_file_set_string (kf, "chrome.light", "card", "garbage");
    g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                           "gtkhx_theme: bad color*");
    gtkhx_theme_load_from_keyfile (kf);
    g_test_assert_expected_messages ();

    g_assert_true (
        gtkhx_theme_get_chrome_color (GTKHX_CHROME_WINDOW, FALSE, &out));
    g_assert_cmpint (byte_of (out.red), ==, 0xfd);
    g_assert_true (
        gtkhx_theme_get_chrome_color (GTKHX_CHROME_VIEW, FALSE, &out));
    g_assert_cmpint (byte_of (out.red), ==, 0xfd);
    g_assert_true (
        gtkhx_theme_get_chrome_color (GTKHX_CHROME_SIDEBAR, FALSE, &out));
    g_assert_cmpint (byte_of (out.red), ==, 0xee);
    g_assert_true (
        gtkhx_theme_get_chrome_color (GTKHX_CHROME_ACCENT, FALSE, &out));
    g_assert_cmpint (byte_of (out.blue), ==, 0xd2);
    /* A bad value falls back to derivation, not to "unset". */
    g_assert_true (
        gtkhx_theme_get_chrome_color (GTKHX_CHROME_CARD, FALSE, &out));
    g_assert_cmpint (byte_of (out.red), <, 0xfd);

    g_key_file_free (kf);
}

/* The CSS sets each color both as a libadwaita variable and as the
 * named color the variables are seeded from, and picks readable text
 * for the accent. */
static void
test_chrome_css (void)
{
    GKeyFile *kf = g_key_file_new ();
    char *css;

    g_key_file_set_string (kf, "chrome.dark", "window", "#002b36");
    g_key_file_set_string (kf, "chrome.dark", "accent", "#268bd2");
    g_key_file_set_string (kf, "chrome.light", "accent", "#b5e0ff");
    gtkhx_theme_load_from_keyfile (kf);

    css = gtkhx_theme_build_chrome_css (TRUE);
    g_assert_nonnull (strstr (css, "@define-color window_bg_color #002b36;"));
    g_assert_nonnull (strstr (css, ":root {"));
    g_assert_nonnull (strstr (css, "--window-bg-color: #002b36;"));
    g_assert_nonnull (strstr (css, "--view-bg-color: #002b36;"));
    g_assert_nonnull (strstr (css, "--accent-bg-color: #268bd2;"));
    g_assert_nonnull (strstr (css, "--accent-fg-color: #ffffff;"));
    /* No fg anywhere in the theme, so none is forced. */
    g_assert_null (strstr (css, "--window-fg-color"));
    g_free (css);

    /* Light accent only: dark text on it, and nothing else emitted. */
    css = gtkhx_theme_build_chrome_css (FALSE);
    g_assert_nonnull (strstr (css, "--accent-fg-color: #000000;"));
    g_assert_null (strstr (css, "--window-bg-color"));
    g_free (css);

    g_key_file_free (kf);
}

/* An action color restyles suggested-action buttons as an outline, and
 * hover / focus / press turn the accent. No action color, no rule: the
 * stock filled button stays. */
static void
test_chrome_action_css (void)
{
    GKeyFile *kf = g_key_file_new ();
    char *css;

    g_key_file_set_string (kf, "chrome.dark", "window", "#0f0d14");
    g_key_file_set_string (kf, "chrome.dark", "accent", "#ff2d95");
    g_key_file_set_string (kf, "chrome.dark", "action", "#b48cff");
    g_key_file_set_string (kf, "chrome.light", "action", "#b48cff");
    gtkhx_theme_load_from_keyfile (kf);

    css = gtkhx_theme_build_chrome_css (TRUE);
    g_assert_nonnull (strstr (css, "splitbutton.suggested-action"));
    g_assert_nonnull (strstr (css, "alpha(#b48cff, 0.06)"));
    g_assert_nonnull (
        strstr (css, "box-shadow: inset 0 0 0 1px alpha(#b48cff, 0.35);"));
    g_assert_nonnull (strstr (css, "box-shadow: inset 0 0 0 1px #ff2d95;"));
    g_assert_nonnull (strstr (css, "button.suggested-action:focus-visible"));
    g_assert_nonnull (strstr (css, "alpha(#ff2d95, 0.08)"));
    g_free (css);

    /* An action with nothing else still styles the button, against the
     * system accent. */
    css = gtkhx_theme_build_chrome_css (FALSE);
    g_assert_null (strstr (css, ":root"));
    g_assert_nonnull (strstr (css, "alpha(@accent_bg_color, 0.08)"));
    g_free (css);

    g_key_file_free (kf);
    kf = g_key_file_new ();
    g_key_file_set_string (kf, "chrome.dark", "accent", "#ff2d95");
    gtkhx_theme_load_from_keyfile (kf);
    css = gtkhx_theme_build_chrome_css (TRUE);
    g_assert_null (strstr (css, "suggested-action"));
    g_free (css);

    g_key_file_free (kf);
}

/* accent_fg replaces the contrast pick for text on the accent, and
 * accent_text sets libadwaita's accent-as-text color outright instead
 * of letting it shift the accent's lightness. */
static void
test_chrome_accent_keys (void)
{
    GKeyFile *kf = g_key_file_new ();
    char *css;

    g_key_file_set_string (kf, "chrome.dark", "accent", "#ff2d95");
    g_key_file_set_string (kf, "chrome.dark", "accent_fg", "#0f0d14");
    g_key_file_set_string (kf, "chrome.dark", "accent_text", "#ff2d95");
    g_key_file_set_string (kf, "chrome.light", "accent", "#ff2d95");
    gtkhx_theme_load_from_keyfile (kf);

    css = gtkhx_theme_build_chrome_css (TRUE);
    g_assert_nonnull (strstr (css, "--accent-fg-color: #0f0d14;"));
    g_assert_nonnull (strstr (css, "--accent-color: #ff2d95;"));
    g_assert_nonnull (strstr (css, "@define-color accent_color #ff2d95;"));
    g_free (css);

    /* Unset: text on the accent is picked for contrast, and the
     * accent-as-text color is left to libadwaita. */
    css = gtkhx_theme_build_chrome_css (FALSE);
    g_assert_nonnull (strstr (css, "--accent-fg-color: #ffffff;"));
    g_assert_null (strstr (css, "--accent-color:"));
    g_free (css);

    g_key_file_free (kf);
}

/* The chat palette stands in for the window colors only as a pair: a
 * background alone would tint the window and leave the system's text
 * color on it. */
static void
test_chrome_palette_needs_pair (void)
{
    GKeyFile *kf = g_key_file_new ();
    GdkRGBA out;
    char *css;

    g_key_file_set_string (kf, "palette.light", "bg", "#000000");
    g_key_file_set_string (kf, "palette.dark", "fg", "#ffffff");
    gtkhx_theme_load_from_keyfile (kf);

    for (int r = 0; r < GTKHX_CHROME_N_ROLES; r++) {
        g_assert_false (gtkhx_theme_get_chrome_color (r, FALSE, &out));
        g_assert_false (gtkhx_theme_get_chrome_color (r, TRUE, &out));
    }
    css = gtkhx_theme_build_chrome_css (FALSE);
    g_assert_cmpstr (css, ==, "");
    g_free (css);

    g_key_file_free (kf);
}

/* Timestamps and nicks derive from the roles they read as — secondary
 * text and body text — until a theme sets them; the gutter roles have
 * defaults of their own. */
static void
test_chat_roles_derive (void)
{
    GKeyFile *kf = g_key_file_new ();

    g_key_file_set_string (kf, "palette.dark", "fg", "#839496");
    g_key_file_set_string (kf, "palette.dark", "history_muted", "#586e75");
    g_key_file_set_string (kf, "palette.dark", "self_nick", "#93a1a1");
    gtkhx_theme_load_from_keyfile (kf);

    assert_rgba_eq_bytes (gtkhx_theme_get_color (GTKHX_PAL_TIMESTAMP, TRUE),
                          0x58, 0x6e, 0x75);
    assert_rgba_eq_bytes (gtkhx_theme_get_color (GTKHX_PAL_NICK, TRUE), 0x83,
                          0x94, 0x96);
    assert_rgba_eq_bytes (gtkhx_theme_get_color (GTKHX_PAL_SELF_NICK, TRUE),
                          0x93, 0xa1, 0xa1);
    g_assert_false (
        gtkhx_theme_palette_role_is_set (GTKHX_PAL_TIMESTAMP, TRUE));
    g_assert_cmpfloat (gtkhx_theme_get_color (GTKHX_PAL_SYSTEM, TRUE).alpha, ==,
                       1.0);

    /* The divider has always been drawn in the text color. */
    assert_rgba_eq_bytes (gtkhx_theme_get_color (GTKHX_PAL_RULE, TRUE), 0x83,
                          0x94, 0x96);

    /* Light set nothing: nicks follow fg all the way to "system". */
    assert_rgba_is_system (gtkhx_theme_get_color (GTKHX_PAL_NICK, FALSE));

    g_key_file_free (kf);
}

/* nick_colors takes commas, semicolons or spaces between entries,
 * skips a bad entry without losing the rest, and caps the list. */
static void
test_nick_colors (void)
{
    GKeyFile *kf = g_key_file_new ();
    GdkRGBA out[GTKHX_NICK_COLORS_MAX];

    g_key_file_set_value (kf, "palette.dark", "nick_colors",
                          "#b58900, #cb4b16;#d33682 nope #6c71c4");
    g_key_file_set_value (kf, "palette.light", "nick_colors",
                          "#000001 #000002 #000003 #000004 #000005 #000006 "
                          "#000007 #000008 #000009");
    /* Light loads before dark. */
    g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                           "gtkhx_theme: *more than*");
    g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                           "gtkhx_theme: bad color*nope*");
    gtkhx_theme_load_from_keyfile (kf);
    g_test_assert_expected_messages ();

    g_assert_cmpint (gtkhx_theme_get_nick_colors (TRUE, out), ==, 4);
    assert_rgba_eq_bytes (out[0], 0xb5, 0x89, 0x00);
    assert_rgba_eq_bytes (out[3], 0x6c, 0x71, 0xc4);
    g_assert_cmpint (gtkhx_theme_get_nick_colors (FALSE, out), ==,
                     GTKHX_NICK_COLORS_MAX);
    assert_rgba_eq_bytes (out[GTKHX_NICK_COLORS_MAX - 1], 0x00, 0x00, 0x08);

    /* A reload without the key clears the list. */
    g_key_file_free (kf);
    kf = g_key_file_new ();
    gtkhx_theme_load_from_keyfile (kf);
    g_assert_cmpint (gtkhx_theme_get_nick_colors (TRUE, out), ==, 0);

    g_key_file_free (kf);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/theme/clamp", test_clamp);
    g_test_add_func ("/theme/load-empty-uses-defaults",
                     test_load_empty_keyfile_uses_defaults);
    g_test_add_func ("/theme/load-scale-overrides", test_load_scale_overrides);
    g_test_add_func ("/theme/load-scale-clamps", test_load_scale_clamps);
    g_test_add_func ("/theme/load-palette", test_load_palette);
    g_test_add_func ("/theme/load-palette-bad-hex-falls-back",
                     test_load_palette_bad_hex_falls_back);
    g_test_add_func ("/theme/load-replaces-not-merges",
                     test_load_replaces_not_merges);
    g_test_add_func ("/theme/load-emits-changed", test_load_emits_changed);
    g_test_add_func ("/theme/load-user-colors", test_load_user_colors);
    g_test_add_func ("/theme/chrome-untinted-without-opt-in",
                     test_chrome_untinted_without_opt_in);
    g_test_add_func ("/theme/chrome-derives-from-palette",
                     test_chrome_derives_from_palette);
    g_test_add_func ("/theme/chrome-explicit-keys-win",
                     test_chrome_explicit_keys_win);
    g_test_add_func ("/theme/chrome-css", test_chrome_css);
    g_test_add_func ("/theme/chrome-action-css", test_chrome_action_css);
    g_test_add_func ("/theme/chrome-accent-keys", test_chrome_accent_keys);
    g_test_add_func ("/theme/chrome-palette-needs-pair",
                     test_chrome_palette_needs_pair);
    g_test_add_func ("/theme/chat-roles-derive", test_chat_roles_derive);
    g_test_add_func ("/theme/nick-colors", test_nick_colors);
    return g_test_run ();
}
