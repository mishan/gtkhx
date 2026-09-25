/*
 * gtkhx_theme.c — implementation of the themable-state singleton.
 * See gtkhx_theme.h, docs/theming.md, and
 * docs/theming-file-format.md.
 *
 * Storage model: the singleton owns its loaded state. Theme files
 * (GKeyFile at $CONFIG/themes/<name>.ini, fallback to the built-in
 * default GResource) feed it; gtkhx_prefs / gtkhxrc does NOT carry
 * any per-axis theming fields. The only theming bit in gtkhxrc is
 * the THEMENAME string that picks which file to load.
 *
 * Sentinel values inside the singleton:
 *   scale_pct[area] == 0   → "active theme didn't override" → use
 *                            the built-in default for that area.
 *   palette_rgb[role][v] < 0 → same, for that (role, variant).
 *
 * Keeping the sentinels in the loaded state (rather than eagerly
 * resolving to default at load time) means the file-format reference
 * "this key may be omitted" round-trips cleanly through a future
 * Settings editor that wants to distinguish "explicitly set to the
 * default value" from "not set, inherits the default."
 */
#include "config.h"

#include <string.h>

#include "gtkhx_theme.h"
#include "cfgkeys.h"
#include "hx.h"

/* gtkhx_config_dir() — directory for the user's theme files. Declared
 * in gtkhx.h; pulled in via hx.h. */
#include "gtkhx.h"

#define GTKHX_DEFAULT_THEME_RESOURCE "/com/nasledov/gtkhx/themes/default.ini"

struct _GtkhxTheme {
    GObject parent_instance;

    /* Loaded scale overrides. 0 = "not set in the active theme" → fall
     * back to default_theme_pct[]. */
    int scale_pct[GTKHX_SCALE_N_AREAS];

    /* Loaded palette overrides, packed as 0x00RRGGBB. -1 = "not set"
     * → fall back to default_palette_{light,dark}[]. Two variants per
     * role (index 0 = light, index 1 = dark). */
    int palette_rgb[GTKHX_PAL_N_ROLES][2];

    /* Loaded user-list name color overrides, same packed-int shape as
     * palette_rgb. -1 = "not set" → caller (users.c::user_color_gdk)
     * keeps its historical default. */
    int user_color_rgb[GTKHX_USER_COLOR_N][2];

    /* Loaded [chrome.*] overrides, same packed-int shape. -1 = "not
     * set" → derived at read time (see gtkhx_theme_get_chrome_color). */
    int chrome_rgb[GTKHX_CHROME_N_ROLES][2];

    /* [gtkhx-theme] icons = classic: the pixel-art chrome icons rather
     * than the symbolic ones. */
    gboolean classic_icons;

    /* The [palette.*] nick_colors lists, packed the same way. */
    int nick_rgb[GTKHX_NICK_COLORS_MAX][2];
    int n_nick[2];
};

G_DEFINE_FINAL_TYPE (GtkhxTheme, gtkhx_theme, G_TYPE_OBJECT)

enum { SIGNAL_CHANGED, N_SIGNALS };

static guint signals[N_SIGNALS];

/* The built-in "default theme" scale table. These are the *real*
 * factors GtkHx has always applied, expressed honestly against the
 * unscaled source art (the 16×16 button pixmaps; the user-list icon's
 * natural size and the base font). The toolbar / window buttons have
 * always drawn at 2× their source art, and the standalone Users
 * window at 1.25×. */
static const int default_theme_pct[GTKHX_SCALE_N_AREAS] = {
    100, /* GTKHX_SCALE_TOOLBAR */
    100, /* GTKHX_SCALE_WINDOW_BUTTONS */
    100, /* GTKHX_SCALE_USERLIST_ICON */
    100, /* GTKHX_SCALE_USERLIST_TEXT */
    100, /* GTKHX_SCALE_TASKS_ROW_ICON */
};

/* Built-in default palette. mIRC slots 0..31 stay in chat.c::colors[]
 * because they're protocol-shaped, not theme-shaped (servers send
 * specific indices; users don't get to remap "red").
 *
 * FG and BG are transparent: "follow the system" (see gtkhx_theme.h).
 * TIMESTAMP, NICK, SELF_NICK and RULE are placeholders — an unset one takes
 * the role it derives from (see derived_role) rather than these.
 *
 * The gutter: brackets are punctuation, so they share the neutral gray
 * of secondary text — quiet, but at better than 6:1 against Adwaita's
 * light and dark views, because a bracket you can't see stops the name
 * reading as a nick. The [hx] tag is Adwaita's accent blue: it is the
 * client speaking. Mentions are red. The old full-intensity mIRC values
 * (#0000ff brackets on black) were close to invisible. */
#define RGB8(r, g, b) { (r) / 255.0, (g) / 255.0, (b) / 255.0, 1.0 }
#define SYSTEM_COLOR { 0.0, 0.0, 0.0, 0.0 }

static const GdkRGBA default_palette_light[GTKHX_PAL_N_ROLES] = {
    [GTKHX_PAL_FG] = SYSTEM_COLOR,
    [GTKHX_PAL_BG] = SYSTEM_COLOR,
    [GTKHX_PAL_MARK_FG] = RGB8 (0xff, 0xff, 0xff), /* selection contrast */
    [GTKHX_PAL_MARK_BG] = RGB8 (0x35, 0x84, 0xe4), /* Adwaita accent */
    [GTKHX_PAL_MARKER] = RGB8 (0xcc, 0x00, 0x00),  /* red marker line */
    [GTKHX_PAL_HISTORY_MUTED]
    = RGB8 (0x5e, 0x5e, 0x5e), /* ~6:1 on a light view */
    [GTKHX_PAL_TIMESTAMP] = SYSTEM_COLOR,
    [GTKHX_PAL_NICK] = SYSTEM_COLOR,
    [GTKHX_PAL_SELF_NICK] = SYSTEM_COLOR,
    [GTKHX_PAL_NICK_BRACKET] = RGB8 (0x5e, 0x5e, 0x5e), /* 6.5:1 */
    [GTKHX_PAL_SELF_BRACKET] = RGB8 (0x5e, 0x5e, 0x5e),
    [GTKHX_PAL_SYSTEM] = RGB8 (0x1c, 0x71, 0xd8), /* blue 4 */
    [GTKHX_PAL_SYSTEM_BRACKET] = RGB8 (0x5e, 0x5e, 0x5e),
    [GTKHX_PAL_HIGHLIGHT] = RGB8 (0xc0, 0x1c, 0x28), /* red 4 */
    [GTKHX_PAL_RULE] = SYSTEM_COLOR,
};

static const GdkRGBA default_palette_dark[GTKHX_PAL_N_ROLES] = {
    [GTKHX_PAL_FG] = SYSTEM_COLOR,
    [GTKHX_PAL_BG] = SYSTEM_COLOR,
    [GTKHX_PAL_MARK_FG] = RGB8 (0xee, 0xee, 0xee),
    [GTKHX_PAL_MARK_BG] = RGB8 (0x20, 0x4a, 0x87), /* Tango blue, original */
    [GTKHX_PAL_MARKER] = RGB8 (0xcc, 0x00, 0x00),
    [GTKHX_PAL_HISTORY_MUTED]
    = RGB8 (0x9a, 0x9a, 0x9a), /* ~6:1 on a dark view */
    [GTKHX_PAL_TIMESTAMP] = SYSTEM_COLOR,
    [GTKHX_PAL_NICK] = SYSTEM_COLOR,
    [GTKHX_PAL_SELF_NICK] = SYSTEM_COLOR,
    [GTKHX_PAL_NICK_BRACKET] = RGB8 (0x9a, 0x9a, 0x9a), /* 6:1 */
    [GTKHX_PAL_SELF_BRACKET] = RGB8 (0x9a, 0x9a, 0x9a),
    [GTKHX_PAL_SYSTEM] = RGB8 (0x78, 0xae, 0xed), /* blue 2 */
    [GTKHX_PAL_SYSTEM_BRACKET] = RGB8 (0x9a, 0x9a, 0x9a),
    [GTKHX_PAL_HIGHLIGHT] = RGB8 (0xf6, 0x61, 0x51), /* red 1 */
    [GTKHX_PAL_RULE] = SYSTEM_COLOR,
};

#undef SYSTEM_COLOR
#undef RGB8

static void
gtkhx_theme_class_init (GtkhxThemeClass *klass)
{
    /* Zero-argument notification. Subscribers re-read whatever
     * scales / colors they care about and refresh. A single coalesced
     * signal keeps fan-out cheap — every consumer already re-reads
     * only its own slots. */
    signals[SIGNAL_CHANGED]
        = g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                        0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
gtkhx_theme_init (GtkhxTheme *self)
{
    int a;
    int r;

    /* "Not overridden" sentinels — get_percent / get_color fall back
     * to the built-in defaults when these are unset. */
    for (a = 0; a < GTKHX_SCALE_N_AREAS; a++) {
        self->scale_pct[a] = 0;
    }
    for (r = 0; r < GTKHX_PAL_N_ROLES; r++) {
        self->palette_rgb[r][0] = -1;
        self->palette_rgb[r][1] = -1;
    }
    for (r = 0; r < GTKHX_USER_COLOR_N; r++) {
        self->user_color_rgb[r][0] = -1;
        self->user_color_rgb[r][1] = -1;
    }
    for (r = 0; r < GTKHX_CHROME_N_ROLES; r++) {
        self->chrome_rgb[r][0] = -1;
        self->chrome_rgb[r][1] = -1;
    }
    self->n_nick[0] = 0;
    self->n_nick[1] = 0;
}

GtkhxTheme *
gtkhx_theme_get_default (void)
{
    static GtkhxTheme *singleton = NULL;

    if (singleton == NULL) {
        singleton = g_object_new (GTKHX_TYPE_THEME, NULL);
    }
    return singleton;
}

int
gtkhx_theme_clamp_percent (int pct)
{
    if (pct < GTKHX_SCALE_MIN) {
        return GTKHX_SCALE_MIN;
    }
    if (pct > GTKHX_SCALE_MAX) {
        return GTKHX_SCALE_MAX;
    }
    return pct;
}

int
gtkhx_theme_get_default_percent (GtkhxScaleArea area)
{
    if (area < 0 || area >= GTKHX_SCALE_N_AREAS) {
        return 100;
    }
    return default_theme_pct[area];
}

int
gtkhx_theme_get_percent (GtkhxScaleArea area)
{
    GtkhxTheme *self = gtkhx_theme_get_default ();

    if (area < 0 || area >= GTKHX_SCALE_N_AREAS) {
        return 100;
    }
    /* 0 means "active theme didn't override" → use the default. A
     * stored negative is treated as unset too (corrupt-file
     * defensiveness — the loader clamps real values into range). */
    if (self->scale_pct[area] <= 0) {
        return gtkhx_theme_get_default_percent (area);
    }
    return gtkhx_theme_clamp_percent (self->scale_pct[area]);
}

double
gtkhx_theme_scale (GtkhxScaleArea area)
{
    return gtkhx_theme_get_percent (area) / 100.0;
}

/* ---- Palette ---------------------------------------------------------- */

GdkRGBA
gtkhx_theme_get_default_color (GtkhxPaletteRole role, gboolean dark)
{
    GdkRGBA fallback = { 0, 0, 0, 1.0 };

    if (role < 0 || role >= GTKHX_PAL_N_ROLES) {
        return fallback;
    }
    return dark ? default_palette_dark[role] : default_palette_light[role];
}

/* The role an unset role takes its color from, or the role itself when
 * it has a built-in default of its own. Timestamps read as secondary
 * text; nicks read as body text unless the theme colors them; the
 * divider has always been drawn in the text color. */
static GtkhxPaletteRole
derived_role (GtkhxPaletteRole role)
{
    switch (role) {
    case GTKHX_PAL_TIMESTAMP:
        return GTKHX_PAL_HISTORY_MUTED;
    case GTKHX_PAL_NICK:
    case GTKHX_PAL_SELF_NICK:
    case GTKHX_PAL_RULE:
        return GTKHX_PAL_FG;
    default:
        return role;
    }
}

GdkRGBA
gtkhx_theme_get_color (GtkhxPaletteRole role, gboolean dark)
{
    GtkhxTheme *self = gtkhx_theme_get_default ();
    int packed;
    GdkRGBA rgba;

    if (role < 0 || role >= GTKHX_PAL_N_ROLES) {
        rgba = (GdkRGBA){ 0, 0, 0, 1.0 };
        return rgba;
    }
    packed = self->palette_rgb[role][dark ? 1 : 0];
    if (packed < 0) {
        GtkhxPaletteRole parent = derived_role (role);
        if (parent != role) {
            return gtkhx_theme_get_color (parent, dark);
        }
        return gtkhx_theme_get_default_color (role, dark);
    }
    rgba.red = ((packed >> 16) & 0xff) / 255.0;
    rgba.green = ((packed >> 8) & 0xff) / 255.0;
    rgba.blue = ((packed) & 0xff) / 255.0;
    rgba.alpha = 1.0;
    return rgba;
}

gboolean
gtkhx_theme_palette_role_is_set (GtkhxPaletteRole role, gboolean dark)
{
    GtkhxTheme *self = gtkhx_theme_get_default ();
    if (role < 0 || role >= GTKHX_PAL_N_ROLES) {
        return FALSE;
    }
    return self->palette_rgb[role][dark ? 1 : 0] >= 0;
}

int
gtkhx_theme_get_nick_colors (gboolean dark, GdkRGBA out[GTKHX_NICK_COLORS_MAX])
{
    GtkhxTheme *self = gtkhx_theme_get_default ();
    int v = dark ? 1 : 0;
    int i;

    for (i = 0; i < self->n_nick[v]; i++) {
        int packed = self->nick_rgb[i][v];
        out[i] = (GdkRGBA){ ((packed >> 16) & 0xff) / 255.0,
                            ((packed >> 8) & 0xff) / 255.0,
                            (packed & 0xff) / 255.0, 1.0 };
    }
    return self->n_nick[v];
}

gboolean
gtkhx_theme_get_user_color (GtkhxUserColor slot, gboolean dark, GdkRGBA *out)
{
    GtkhxTheme *self = gtkhx_theme_get_default ();
    int packed;
    if (slot < 0 || slot >= GTKHX_USER_COLOR_N || !out) {
        return FALSE;
    }
    packed = self->user_color_rgb[slot][dark ? 1 : 0];
    if (packed < 0) {
        return FALSE;
    }
    out->red = ((packed >> 16) & 0xff) / 255.0;
    out->green = ((packed >> 8) & 0xff) / 255.0;
    out->blue = ((packed) & 0xff) / 255.0;
    out->alpha = 1.0;
    return TRUE;
}

static GdkRGBA
unpack_rgb (int packed)
{
    return (GdkRGBA){ ((packed >> 16) & 0xff) / 255.0,
                      ((packed >> 8) & 0xff) / 255.0, (packed & 0xff) / 255.0,
                      1.0 };
}

static GdkRGBA
mix_rgb (const GdkRGBA *a, const GdkRGBA *b, double t)
{
    return (GdkRGBA){ a->red + (b->red - a->red) * t,
                      a->green + (b->green - a->green) * t,
                      a->blue + (b->blue - a->blue) * t, 1.0 };
}

/* How far each derived surface sits from the window background, as a
 * fraction of the way toward the foreground. Small steps: the surfaces
 * should read as layers of one color, not as different colors. */
#define CHROME_STEP_CARD 0.05
#define CHROME_STEP_HEADERBAR 0.07
#define CHROME_STEP_POPOVER 0.09

/* chrome_rgb value for a role a theme set to "system" (see
 * load_chrome_group); -1 remains "not set". */
#define CHROME_SYSTEM (-2)

/* The chat palette stands in for the window colors only as a pair. A
 * theme that sets only a chat background would otherwise paint the
 * window with it while its text stayed the system's — dark text on a
 * dark window, or the reverse. */
static gboolean
palette_pair_set (GtkhxTheme *self, int v)
{
    return self->palette_rgb[GTKHX_PAL_BG][v] >= 0
           && self->palette_rgb[GTKHX_PAL_FG][v] >= 0;
}

gboolean
gtkhx_theme_get_chrome_color (GtkhxChromeRole role, gboolean dark, GdkRGBA *out)
{
    GtkhxTheme *self = gtkhx_theme_get_default ();
    int v = dark ? 1 : 0;
    GdkRGBA window;
    GdkRGBA fg;
    double step;

    if (role < 0 || role >= GTKHX_CHROME_N_ROLES || !out) {
        return FALSE;
    }
    if (self->chrome_rgb[role][v] >= 0) {
        *out = unpack_rgb (self->chrome_rgb[role][v]);
        return TRUE;
    }
    if (self->chrome_rgb[role][v] == CHROME_SYSTEM) {
        return FALSE;
    }

    switch (role) {
    case GTKHX_CHROME_WINDOW:
        if (!palette_pair_set (self, v)) {
            return FALSE;
        }
        *out = unpack_rgb (self->palette_rgb[GTKHX_PAL_BG][v]);
        return TRUE;
    case GTKHX_CHROME_FG:
        if (!palette_pair_set (self, v)) {
            return FALSE;
        }
        *out = unpack_rgb (self->palette_rgb[GTKHX_PAL_FG][v]);
        return TRUE;
    case GTKHX_CHROME_VIEW:
        return gtkhx_theme_get_chrome_color (GTKHX_CHROME_WINDOW, dark, out);
    case GTKHX_CHROME_SIDEBAR:
        return gtkhx_theme_get_chrome_color (GTKHX_CHROME_HEADERBAR, dark, out);
    case GTKHX_CHROME_HEADERBAR_FG:
        return gtkhx_theme_get_chrome_color (GTKHX_CHROME_FG, dark, out);
    case GTKHX_CHROME_ACCENT:
    case GTKHX_CHROME_ACCENT_FG:
    case GTKHX_CHROME_ACCENT_TEXT:
    case GTKHX_CHROME_ACTION:
        /* No derivation: these are choices, not shades. (An unset
         * accent_fg is picked for contrast where the CSS is built, and
         * an unset accent_text is left to libadwaita, which derives it
         * from the accent.) */
        return FALSE;
    case GTKHX_CHROME_CARD:
        step = CHROME_STEP_CARD;
        break;
    case GTKHX_CHROME_HEADERBAR:
        step = CHROME_STEP_HEADERBAR;
        break;
    case GTKHX_CHROME_POPOVER:
        step = CHROME_STEP_POPOVER;
        break;
    case GTKHX_CHROME_N_ROLES:
    default:
        return FALSE;
    }

    /* The layered surfaces need a window color to step from. The
     * direction comes from fg when the theme has one, else from the
     * variant: lighter on dark, darker on light. */
    if (!gtkhx_theme_get_chrome_color (GTKHX_CHROME_WINDOW, dark, &window)) {
        return FALSE;
    }
    if (!gtkhx_theme_get_chrome_color (GTKHX_CHROME_FG, dark, &fg)) {
        fg = dark ? (GdkRGBA){ 1, 1, 1, 1 } : (GdkRGBA){ 0, 0, 0, 1 };
    }
    *out = mix_rgb (&window, &fg, step);
    return TRUE;
}

/* Set one libadwaita named color both ways it can be read: as the
 * `--name-with-dashes` CSS variable stock widgets use, and as the
 * `@name_with_underscores` color that libadwaita's variables are
 * seeded from and that GtkHx's own chrome.css still references. */
static void
append_css_color (GString *vars, GString *defines, const char *name,
                  const GdkRGBA *c)
{
    g_autofree char *hex = g_strdup_printf (
        "#%02x%02x%02x", (int)(c->red * 255.0 + 0.5),
        (int)(c->green * 255.0 + 0.5), (int)(c->blue * 255.0 + 0.5));
    g_autofree char *var = g_strdelimit (g_strdup (name), "_", '-');

    g_string_append_printf (vars, "  --%s: %s;\n", var, hex);
    g_string_append_printf (defines, "@define-color %s %s;\n", name, hex);
}

/* Perceived brightness (Rec. 601 weights on the gamma-encoded
 * channels) — good enough to pick black or white text on the accent. */
static double
rgb_brightness (const GdkRGBA *c)
{
    return 0.299 * c->red + 0.587 * c->green + 0.114 * c->blue;
}

/* Suggested-action buttons (Connect, Save, Post, …) restyled as an
 * outline in the theme's action color over a faint wash of it, for a
 * theme whose design says an action is a link, not a filled pill.
 * Hover, keyboard focus and press are one state — they turn the accent
 * color over a faint accent wash — so the button behaves like every
 * other link. Empty when the
 * theme sets no action color: Adwaita's filled button stays. Caller
 * frees. */
static char *
action_css (gboolean dark)
{
    GdkRGBA action;
    GdkRGBA accent;
    g_autofree char *action_hex = NULL;
    g_autofree char *accent_hex = NULL;

    if (!gtkhx_theme_get_chrome_color (GTKHX_CHROME_ACTION, dark, &action)) {
        return g_strdup ("");
    }
    action_hex = g_strdup_printf (
        "#%02x%02x%02x", (int)(action.red * 255.0 + 0.5),
        (int)(action.green * 255.0 + 0.5), (int)(action.blue * 255.0 + 0.5));
    if (gtkhx_theme_get_chrome_color (GTKHX_CHROME_ACCENT, dark, &accent)) {
        accent_hex
            = g_strdup_printf ("#%02x%02x%02x", (int)(accent.red * 255.0 + 0.5),
                               (int)(accent.green * 255.0 + 0.5),
                               (int)(accent.blue * 255.0 + 0.5));
    } else {
        accent_hex = g_strdup ("@accent_bg_color");
    }

    return g_strdup_printf (
        "button.suggested-action, button.suggested-action:checked,\n"
        "splitbutton.suggested-action, menubutton.suggested-action {\n"
        "  background-color: alpha(%s, 0.06);\n"
        "  background-image: none;\n"
        "  color: %s;\n"
        "  box-shadow: inset 0 0 0 1px alpha(%s, 0.35);\n"
        "}\n"
        "button.suggested-action:hover, "
        "button.suggested-action:focus-visible,\n"
        "button.suggested-action:active,\n"
        "splitbutton.suggested-action:hover,\n"
        "splitbutton.suggested-action:focus-within,\n"
        "menubutton.suggested-action:hover,\n"
        "menubutton.suggested-action:focus-within {\n"
        "  background-color: alpha(%s, 0.08);\n"
        "  background-image: none;\n"
        "  color: %s;\n"
        "  box-shadow: inset 0 0 0 1px %s;\n"
        "}\n"
        /* The halves of a split / menu button tint themselves on hover
         * too; left alone, that stacks on the wash above. */
        "splitbutton.suggested-action > button,\n"
        "splitbutton.suggested-action > menubutton > button,\n"
        "menubutton.suggested-action > button {\n"
        "  background-color: transparent;\n"
        "  background-image: none;\n"
        "}\n",
        action_hex, action_hex, action_hex, accent_hex, accent_hex, accent_hex);
}

char *
gtkhx_theme_build_chrome_css (gboolean dark)
{
    g_autoptr (GString) vars = g_string_new (NULL);
    g_autoptr (GString) defines = g_string_new (NULL);
    GdkRGBA c;

#define SET(name) append_css_color (vars, defines, name, &c)
    /* Each libadwaita color a role feeds. Backdrop colors (the
     * unfocused-window variants) follow the matching surface so an
     * unfocused window doesn't fall back to stock gray. */
    if (gtkhx_theme_get_chrome_color (GTKHX_CHROME_WINDOW, dark, &c)) {
        SET ("window_bg_color");
        SET ("headerbar_backdrop_color");
    }
    if (gtkhx_theme_get_chrome_color (GTKHX_CHROME_VIEW, dark, &c)) {
        SET ("view_bg_color");
    }
    if (gtkhx_theme_get_chrome_color (GTKHX_CHROME_HEADERBAR, dark, &c)) {
        SET ("headerbar_bg_color");
    }
    if (gtkhx_theme_get_chrome_color (GTKHX_CHROME_SIDEBAR, dark, &c)) {
        SET ("sidebar_bg_color");
        SET ("sidebar_backdrop_color");
        SET ("secondary_sidebar_bg_color");
        SET ("secondary_sidebar_backdrop_color");
    }
    if (gtkhx_theme_get_chrome_color (GTKHX_CHROME_CARD, dark, &c)) {
        SET ("card_bg_color");
        SET ("thumbnail_bg_color");
    }
    if (gtkhx_theme_get_chrome_color (GTKHX_CHROME_POPOVER, dark, &c)) {
        SET ("popover_bg_color");
        SET ("dialog_bg_color");
    }
    if (gtkhx_theme_get_chrome_color (GTKHX_CHROME_FG, dark, &c)) {
        SET ("window_fg_color");
        SET ("view_fg_color");
        SET ("sidebar_fg_color");
        SET ("secondary_sidebar_fg_color");
        SET ("card_fg_color");
        SET ("popover_fg_color");
        SET ("dialog_fg_color");
        SET ("thumbnail_fg_color");
    }
    if (gtkhx_theme_get_chrome_color (GTKHX_CHROME_HEADERBAR_FG, dark, &c)) {
        SET ("headerbar_fg_color");
    }
    if (gtkhx_theme_get_chrome_color (GTKHX_CHROME_ACCENT, dark, &c)) {
        SET ("accent_bg_color");
        if (!gtkhx_theme_get_chrome_color (GTKHX_CHROME_ACCENT_FG, dark, &c)) {
            c = rgb_brightness (&c) > 0.6 ? (GdkRGBA){ 0, 0, 0, 1 }
                                          : (GdkRGBA){ 1, 1, 1, 1 };
        }
        SET ("accent_fg_color");
    }
    /* libadwaita derives the accent-as-text color from the accent,
     * shifting its lightness for contrast. A theme that has already
     * picked a readable text shade sets it outright. */
    if (gtkhx_theme_get_chrome_color (GTKHX_CHROME_ACCENT_TEXT, dark, &c)) {
        SET ("accent_color");
    }
#undef SET

    g_autofree char *action = action_css (dark);
    if (vars->len == 0) {
        return g_steal_pointer (&action);
    }
    return g_strdup_printf ("%s:root {\n%s}\n%s", defines->str, vars->str,
                            action);
}

gboolean
gtkhx_theme_classic_icons (void)
{
    return gtkhx_theme_get_default ()->classic_icons;
}

const char *
gtkhx_theme_active_name (void)
{
    const char *name = (gtkhx_prefs.theme_name && *gtkhx_prefs.theme_name)
                           ? gtkhx_prefs.theme_name
                           : "default";
    /* Same defensive rejection the loader applies (see
     * safe_active_theme_name): a name with a path separator could
     * escape the themes directory. Without this check the icon
     * resolver would try to load
     *   $CONFIG/themes/<bad-name>/icons/<logical>.png
     * with the path separator embedded, fail every lookup, and
     * never serve the user the default-theme bundled icons either.
     * Treating bad names as "default" matches the loader's behavior
     * end-to-end. */
    if (strchr (name, '/') || strchr (name, '\\')) {
        return "default";
    }
    return name;
}

/* ---- Hex parser ------------------------------------------------------- */

/* Parse "#rrggbb" / "#RRGGBB" into 0x00RRGGBB. Returns -1 (the
 * "unset" sentinel) on malformed input — the caller will fall back
 * to the built-in default for that slot rather than failing the
 * whole load. Tolerates surrounding whitespace and an optional
 * leading "#". */
static int
parse_hex_color (const char *s)
{
    int v = 0;
    int i;
    const char *p;

    if (!s) {
        return -1;
    }
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    if (*s == '#') {
        s++;
    }
    p = s;
    /* Exactly 6 hex digits. We don't accept the short "#abc" form —
     * theme files are machine-edited often enough that a typo here
     * should fail loudly (fall back to default) rather than silently
     * pick a weird color. */
    for (i = 0; i < 6; i++) {
        char c = p[i];
        int nib;
        /* Stop at the terminator before indexing further: a string
         * shorter than 6 hex digits ("#abc", "", …) cleanly falls back
         * to the default instead of walking past the NUL. */
        if (c == '\0') {
            return -1;
        }
        if (c >= '0' && c <= '9') {
            nib = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            nib = 10 + (c - 'a');
        } else if (c >= 'A' && c <= 'F') {
            nib = 10 + (c - 'A');
        } else {
            return -1;
        }
        v = (v << 4) | nib;
    }
    /* Tail must be empty or whitespace only. */
    {
        const char *t = p + 6;
        while (*t == ' ' || *t == '\t' || *t == '\r' || *t == '\n') {
            t++;
        }
        if (*t) {
            return -1;
        }
    }
    return v & 0xffffff;
}

/* ---- Loader ----------------------------------------------------------- */

/* Mapping table for the [scale] keys. Indexing is by GtkhxScaleArea
 * so the loader can iterate. */
static const char *const scale_key_name[GTKHX_SCALE_N_AREAS] = {
    [GTKHX_SCALE_TOOLBAR] = "toolbar",
    [GTKHX_SCALE_WINDOW_BUTTONS] = "window_buttons",
    [GTKHX_SCALE_USERLIST_ICON] = "userlist_icon",
    [GTKHX_SCALE_USERLIST_TEXT] = "userlist_text",
    [GTKHX_SCALE_TASKS_ROW_ICON] = "tasks_row_icon",
};

/* Mapping for the [palette.light] / [palette.dark] keys. */
static const char *const palette_key_name[GTKHX_PAL_N_ROLES] = {
    [GTKHX_PAL_FG] = "fg",
    [GTKHX_PAL_BG] = "bg",
    [GTKHX_PAL_MARK_FG] = "mark_fg",
    [GTKHX_PAL_MARK_BG] = "mark_bg",
    [GTKHX_PAL_MARKER] = "marker",
    [GTKHX_PAL_HISTORY_MUTED] = "history_muted",
    [GTKHX_PAL_TIMESTAMP] = "timestamp",
    [GTKHX_PAL_NICK] = "nick",
    [GTKHX_PAL_SELF_NICK] = "self_nick",
    [GTKHX_PAL_NICK_BRACKET] = "nick_bracket",
    [GTKHX_PAL_SELF_BRACKET] = "self_bracket",
    [GTKHX_PAL_SYSTEM] = "system",
    [GTKHX_PAL_SYSTEM_BRACKET] = "system_bracket",
    [GTKHX_PAL_HIGHLIGHT] = "highlight",
    [GTKHX_PAL_RULE] = "rule",
};

#define NICK_COLORS_KEY "nick_colors"

#define META_GROUP "gtkhx-theme"
#define SCALE_GROUP "scale"
#define PALETTE_LIGHT_GROUP "palette.light"
#define PALETTE_DARK_GROUP "palette.dark"
#define USERS_LIGHT_GROUP "users.light"
#define USERS_DARK_GROUP "users.dark"
#define CHROME_LIGHT_GROUP "chrome.light"
#define CHROME_DARK_GROUP "chrome.dark"

static const char *const chrome_key_name[GTKHX_CHROME_N_ROLES] = {
    [GTKHX_CHROME_WINDOW] = "window",
    [GTKHX_CHROME_VIEW] = "view",
    [GTKHX_CHROME_HEADERBAR] = "headerbar",
    [GTKHX_CHROME_SIDEBAR] = "sidebar",
    [GTKHX_CHROME_CARD] = "card",
    [GTKHX_CHROME_POPOVER] = "popover",
    [GTKHX_CHROME_FG] = "fg",
    [GTKHX_CHROME_HEADERBAR_FG] = "headerbar_fg",
    [GTKHX_CHROME_ACCENT] = "accent",
    [GTKHX_CHROME_ACCENT_FG] = "accent_fg",
    [GTKHX_CHROME_ACCENT_TEXT] = "accent_text",
    [GTKHX_CHROME_ACTION] = "action",
};

static const char *const user_color_key_name[GTKHX_USER_COLOR_N] = {
    [GTKHX_USER_COLOR_ACTIVE] = "active",
    [GTKHX_USER_COLOR_IDLE] = "idle",
    [GTKHX_USER_COLOR_ADMIN] = "admin",
    [GTKHX_USER_COLOR_ADMIN_IDLE] = "admin_idle",
};

static void
load_palette_group (GtkhxTheme *self, GKeyFile *kf, const char *group,
                    int variant_idx)
{
    int r;

    if (!g_key_file_has_group (kf, group)) {
        return;
    }
    for (r = 0; r < GTKHX_PAL_N_ROLES; r++) {
        char *raw;
        int packed;

        if (!g_key_file_has_key (kf, group, palette_key_name[r], NULL)) {
            continue;
        }
        raw = g_key_file_get_string (kf, group, palette_key_name[r], NULL);
        packed = parse_hex_color (raw);
        if (packed >= 0) {
            self->palette_rgb[r][variant_idx] = packed;
        } else {
            /* Leave at -1 (unset) → falls back to the built-in
             * default for this slot. Log so a typo isn't silent. */
            g_warning ("gtkhx_theme: bad color in [%s] %s = %s", group,
                       palette_key_name[r], raw ? raw : "");
        }
        g_free (raw);
    }

    /* nick_colors: a list of colors, separated by commas, semicolons or
     * spaces. A bad entry is skipped with a warning; the rest stand. */
    if (g_key_file_has_key (kf, group, NICK_COLORS_KEY, NULL)) {
        g_autofree char *raw
            = g_key_file_get_value (kf, group, NICK_COLORS_KEY, NULL);
        g_auto (GStrv) items = g_strsplit_set (raw ? raw : "", ",; \t", -1);
        int n = 0;

        for (int i = 0; items[i]; i++) {
            int packed;

            if (items[i][0] == '\0') {
                continue;
            }
            packed = parse_hex_color (items[i]);
            if (packed < 0) {
                g_warning ("gtkhx_theme: bad color in [%s] %s: %s", group,
                           NICK_COLORS_KEY, items[i]);
                continue;
            }
            if (n == GTKHX_NICK_COLORS_MAX) {
                g_warning ("gtkhx_theme: [%s] %s has more than %d colors; "
                           "using the first %d",
                           group, NICK_COLORS_KEY, GTKHX_NICK_COLORS_MAX,
                           GTKHX_NICK_COLORS_MAX);
                break;
            }
            self->nick_rgb[n++][variant_idx] = packed;
        }
        self->n_nick[variant_idx] = n;
    }
}

/* Same shape as load_palette_group, but writes into user_color_rgb
 * and walks the active/idle/admin/admin_idle key set. Separate from
 * load_palette_group only because the underlying arrays have
 * different bounds and key tables — the body would otherwise be a
 * straight clone. */
static void
load_user_color_group (GtkhxTheme *self, GKeyFile *kf, const char *group,
                       int variant_idx)
{
    int s;

    if (!g_key_file_has_group (kf, group)) {
        return;
    }
    for (s = 0; s < GTKHX_USER_COLOR_N; s++) {
        char *raw;
        int packed;

        if (!g_key_file_has_key (kf, group, user_color_key_name[s], NULL)) {
            continue;
        }
        raw = g_key_file_get_string (kf, group, user_color_key_name[s], NULL);
        packed = parse_hex_color (raw);
        if (packed >= 0) {
            self->user_color_rgb[s][variant_idx] = packed;
        } else {
            g_warning ("gtkhx_theme: bad color in [%s] %s = %s", group,
                       user_color_key_name[s], raw ? raw : "");
        }
        g_free (raw);
    }
}

static void
load_chrome_group (GtkhxTheme *self, GKeyFile *kf, const char *group,
                   int variant_idx)
{
    int r;

    if (!g_key_file_has_group (kf, group)) {
        return;
    }
    for (r = 0; r < GTKHX_CHROME_N_ROLES; r++) {
        g_autofree char *raw = NULL;
        int packed;

        if (!g_key_file_has_key (kf, group, chrome_key_name[r], NULL)) {
            continue;
        }
        raw = g_key_file_get_string (kf, group, chrome_key_name[r], NULL);
        /* "system": leave the role to the system theme, and don't derive
         * it either — the way to keep stock window chrome around a
         * theme's chat colors. */
        if (raw && g_ascii_strcasecmp (g_strstrip (raw), "system") == 0) {
            self->chrome_rgb[r][variant_idx] = CHROME_SYSTEM;
            continue;
        }
        packed = parse_hex_color (raw);
        if (packed >= 0) {
            self->chrome_rgb[r][variant_idx] = packed;
        } else {
            g_warning ("gtkhx_theme: bad color in [%s] %s = %s", group,
                       chrome_key_name[r], raw ? raw : "");
        }
    }
}

void
gtkhx_theme_load_from_keyfile (GKeyFile *kf)
{
    GtkhxTheme *self = gtkhx_theme_get_default ();
    int a;
    int r;

    /* Reset everything to "unset" first — load_from_keyfile is a
     * replacement, not a merge. Done unconditionally so a NULL
     * argument is a clean "load empty theme" (resets state to
     * built-in defaults + still emits "changed"). The header
     * contract documents that behavior; callers / tests rely on
     * it as the "reset to defaults" primitive. */
    for (a = 0; a < GTKHX_SCALE_N_AREAS; a++) {
        self->scale_pct[a] = 0;
    }
    for (r = 0; r < GTKHX_PAL_N_ROLES; r++) {
        self->palette_rgb[r][0] = -1;
        self->palette_rgb[r][1] = -1;
    }
    for (r = 0; r < GTKHX_USER_COLOR_N; r++) {
        self->user_color_rgb[r][0] = -1;
        self->user_color_rgb[r][1] = -1;
    }
    for (r = 0; r < GTKHX_CHROME_N_ROLES; r++) {
        self->chrome_rgb[r][0] = -1;
        self->chrome_rgb[r][1] = -1;
    }
    self->n_nick[0] = 0;
    self->n_nick[1] = 0;
    self->classic_icons = FALSE;

    /* NULL keyfile: nothing else to parse — fall through to the
     * "changed" emit so subscribers reset in lockstep. */
    if (!kf) {
        g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
        return;
    }

    /* [gtkhx-theme] icons */
    {
        g_autofree char *icons
            = g_key_file_get_string (kf, META_GROUP, "icons", NULL);
        if (icons) {
            g_strstrip (icons);
            if (g_ascii_strcasecmp (icons, "classic") == 0) {
                self->classic_icons = TRUE;
            } else if (g_ascii_strcasecmp (icons, "symbolic") != 0) {
                g_warning ("gtkhx_theme: [%s] icons = %s: expected symbolic "
                           "or classic",
                           META_GROUP, icons);
            }
        }
    }

    /* [scale] */
    if (g_key_file_has_group (kf, SCALE_GROUP)) {
        for (a = 0; a < GTKHX_SCALE_N_AREAS; a++) {
            GError *err = NULL;
            int v;

            if (!g_key_file_has_key (kf, SCALE_GROUP, scale_key_name[a],
                                     NULL)) {
                continue;
            }
            v = g_key_file_get_integer (kf, SCALE_GROUP, scale_key_name[a],
                                        &err);
            if (err) {
                g_warning ("gtkhx_theme: bad integer in [%s] %s: %s",
                           SCALE_GROUP, scale_key_name[a], err->message);
                g_clear_error (&err);
                continue;
            }
            /* Clamp on load so a slightly-out-of-range value sticks at
             * the boundary rather than getting silently treated as
             * "unset". A zero or negative IS treated as unset (matches
             * the get_percent contract). */
            if (v > 0) {
                self->scale_pct[a] = gtkhx_theme_clamp_percent (v);
            }
        }
    }

    /* [palette.light] / [palette.dark] */
    load_palette_group (self, kf, PALETTE_LIGHT_GROUP, 0);
    load_palette_group (self, kf, PALETTE_DARK_GROUP, 1);

    /* [users.light] / [users.dark] — user-list name colors
     * (active / idle / admin / admin_idle). */
    load_user_color_group (self, kf, USERS_LIGHT_GROUP, 0);
    load_user_color_group (self, kf, USERS_DARK_GROUP, 1);

    /* [chrome.light] / [chrome.dark] — window chrome. */
    load_chrome_group (self, kf, CHROME_LIGHT_GROUP, 0);
    load_chrome_group (self, kf, CHROME_DARK_GROUP, 1);

    g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

/* Return the safe active-theme name (the THEMENAME pref value or
 * "default", with path-separator rejection). The returned pointer
 * is borrowed (lives in gtkhx_prefs or is a literal); caller does
 * NOT free. */
static const char *
safe_active_theme_name (void)
{
    const char *name = gtkhx_prefs.theme_name && *gtkhx_prefs.theme_name
                           ? gtkhx_prefs.theme_name
                           : "default";
    if (strchr (name, '/') || strchr (name, '\\')) {
        g_warning ("gtkhx_theme: rejecting theme name %s (path separator)",
                   name);
        return "default";
    }
    return name;
}

/* Read a theme from a GResource path into a freshly-allocated
 * GKeyFile. Returns NULL if the resource isn't present or fails to
 * parse. The default-theme resource is the only one we ship, but the
 * helper is general so a future "system themes" addition is
 * trivial. */
static GKeyFile *
load_keyfile_from_resource (const char *resource_path)
{
    GBytes *bytes;
    GKeyFile *kf;
    GError *err = NULL;

    /* "Not present" (G_RESOURCE_ERROR_NOT_FOUND) is a normal outcome
     * — load_builtin_theme tries the dir-form first and the flat-form
     * second, so the dir-form lookup for a flat-form built-in (e.g.
     * solarized) misses on every successful load. Returning NULL
     * silently for NOT_FOUND lets the fallback chain in
     * gtkhx_theme_load_active do its job without spamming the
     * console. Other GResource errors (internal resource-table
     * corruption, IO failures inside the bundle reader) DO get
     * warned about — they're real bugs worth surfacing rather than
     * silently treating as "no such theme". A genuine parse failure
     * (file present but unparseable) still warns below. */
    bytes = g_resources_lookup_data (resource_path,
                                     G_RESOURCE_LOOKUP_FLAGS_NONE, &err);
    if (!bytes) {
        if (err
            && !g_error_matches (err, G_RESOURCE_ERROR,
                                 G_RESOURCE_ERROR_NOT_FOUND)) {
            g_warning ("gtkhx_theme: resource lookup %s: %s", resource_path,
                       err->message);
        }
        g_clear_error (&err);
        return NULL;
    }

    kf = g_key_file_new ();
    if (!g_key_file_load_from_bytes (kf, bytes, G_KEY_FILE_NONE, &err)) {
        g_warning ("gtkhx_theme: parse %s failed: %s", resource_path,
                   err ? err->message : "(unknown)");
        g_clear_error (&err);
        g_key_file_free (kf);
        g_bytes_unref (bytes);
        return NULL;
    }
    g_bytes_unref (bytes);
    return kf;
}

/* Try a user-side theme file at one of two layouts:
 *
 *   $CONFIG/themes/<name>/theme.ini   (dir-form bundle; preferred
 *                                       because it can ship icons
 *                                       alongside)
 *   $CONFIG/themes/<name>.ini         (flat-form; no bundled icons)
 *
 * Returns an allocated GKeyFile on success (caller frees with
 * g_key_file_free), NULL otherwise. */
static GKeyFile *
load_user_theme (const char *name)
{
    /* Dir-form first — it's the richer layout. */
    char *dir_path
        = g_strdup_printf ("%s/themes/%s/theme.ini", gtkhx_config_dir (), name);
    GKeyFile *kf = NULL;
    GError *err = NULL;

    if (g_file_test (dir_path, G_FILE_TEST_IS_REGULAR)) {
        kf = g_key_file_new ();
        if (!g_key_file_load_from_file (kf, dir_path, G_KEY_FILE_NONE, &err)) {
            g_warning ("gtkhx_theme: load %s failed: %s", dir_path,
                       err ? err->message : "(unknown)");
            g_clear_error (&err);
            g_key_file_free (kf);
            kf = NULL;
        }
    }
    g_free (dir_path);

    if (kf) {
        return kf;
    }

    char *flat_path
        = g_strdup_printf ("%s/themes/%s.ini", gtkhx_config_dir (), name);
    if (g_file_test (flat_path, G_FILE_TEST_IS_REGULAR)) {
        kf = g_key_file_new ();
        if (!g_key_file_load_from_file (kf, flat_path, G_KEY_FILE_NONE, &err)) {
            g_warning ("gtkhx_theme: load %s failed: %s", flat_path,
                       err ? err->message : "(unknown)");
            g_clear_error (&err);
            g_key_file_free (kf);
            kf = NULL;
        }
    }
    g_free (flat_path);
    return kf;
}

/* Same shape but for the GResource side. Tries dir-form
 * (/com/nasledov/gtkhx/themes/<name>/theme.ini) then flat-form
 * (.../themes/<name>.ini). NULL on miss. */
static GKeyFile *
load_builtin_theme (const char *name)
{
    char *dir_res
        = g_strdup_printf ("/com/nasledov/gtkhx/themes/%s/theme.ini", name);
    GKeyFile *kf = load_keyfile_from_resource (dir_res);
    g_free (dir_res);
    if (kf) {
        return kf;
    }
    char *flat_res
        = g_strdup_printf ("/com/nasledov/gtkhx/themes/%s.ini", name);
    kf = load_keyfile_from_resource (flat_res);
    g_free (flat_res);
    return kf;
}

void
gtkhx_theme_load_active (void)
{
    const char *name = safe_active_theme_name ();
    GKeyFile *kf = load_user_theme (name);

    /* If the user side didn't have the theme, try the same name in
     * the GResource themes prefix — that's how the built-ins (default,
     * solarized) get loaded when the user hasn't dropped a same-name
     * override into $CONFIG/themes/. */
    if (!kf) {
        kf = load_builtin_theme (name);
    }

    /* Last-ditch fallback: the default GResource. If even that fails
     * (shouldn't — it ships in-binary), we load nothing and every
     * accessor returns its built-in default, which is the same shape
     * the user would see from a default-theme load anyway. */
    if (!kf) {
        kf = load_keyfile_from_resource (GTKHX_DEFAULT_THEME_RESOURCE);
    }

    if (kf) {
        gtkhx_theme_load_from_keyfile (kf);
        g_key_file_free (kf);
    } else {
        /* Still emit "changed" so subscribers reset to defaults on
         * the path where no source loaded — keeps the boot sequence
         * predictable. */
        g_signal_emit (gtkhx_theme_get_default (), signals[SIGNAL_CHANGED], 0);
    }
}

/* ---- Discovery (theme picker) ---------------------------------------- */

void
gtkhx_theme_entry_free (GtkhxThemeEntry *e)
{
    if (!e) {
        return;
    }
    g_free (e->name);
    g_free (e->display);
    g_free (e);
}

/* Read the [gtkhx-theme] name from a GKeyFile-format buffer. Returns
 * a fresh string (caller frees) or NULL on missing / parse error.
 * The buffer is borrowed; not mutated. */
static char *
read_display_name_from_bytes (const char *data, gsize len)
{
    GKeyFile *kf = g_key_file_new ();
    char *display = NULL;

    if (g_key_file_load_from_data (kf, data, len, G_KEY_FILE_NONE, NULL)) {
        display = g_key_file_get_string (kf, "gtkhx-theme", "name", NULL);
        if (display && !*display) {
            g_free (display);
            display = NULL;
        }
    }
    g_key_file_free (kf);
    return display;
}

/* Strip a trailing ".ini" if present; otherwise return a fresh copy.
 * Used to convert a discovered filename to a theme name (the
 * THEMENAME pref value). */
static char *
strip_ini_suffix (const char *filename)
{
    gsize n = strlen (filename);
    if (n > 4 && g_ascii_strcasecmp (filename + n - 4, ".ini") == 0) {
        return g_strndup (filename, n - 4);
    }
    return g_strdup (filename);
}

/* Pull the [gtkhx-theme] display name out of a theme file on disk.
 * Caller frees. NULL on missing key / parse error / unreadable file. */
static char *
read_display_name_from_file (const char *path)
{
    char *contents = NULL;
    gsize len = 0;
    char *display;

    if (!g_file_get_contents (path, &contents, &len, NULL)) {
        return NULL;
    }
    display = read_display_name_from_bytes (contents, len);
    g_free (contents);
    return display;
}

/* TRUE if `name` is safe to use as an active-theme name. Mirrors
 * the rejection rule applied by safe_active_theme_name /
 * gtkhx_theme_active_name at load time: any embedded path
 * separator could escape the themes directory. Surfacing such a
 * name in the picker would produce a phantom entry that
 * "selects" but loads as "default" — confusing. The discovery
 * walks apply this check before adding an entry so the picker
 * only ever lists themes the loader can actually open. */
static gboolean
theme_name_is_safe (const char *name)
{
    return name && !strchr (name, '/') && !strchr (name, '\\');
}

/* Same, but from a GResource path under the registered themes
 * prefix. Caller frees. NULL on parse error. */
static char *
read_display_name_from_resource (const char *resource_path)
{
    GBytes *bytes;
    char *display = NULL;

    bytes = g_resources_lookup_data (resource_path,
                                     G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
    if (bytes) {
        gsize len;
        const char *data = g_bytes_get_data (bytes, &len);
        display = read_display_name_from_bytes (data, len);
        g_bytes_unref (bytes);
    }
    return display;
}

/* Sort comparator: "default" pinned to the front, then alphabetical
 * by display name (locale-aware). qsort-style: returns <0 if a
 * sorts before b. */
static gint
theme_entry_cmp (gconstpointer ap, gconstpointer bp)
{
    const GtkhxThemeEntry *a = *(const GtkhxThemeEntry *const *)ap;
    const GtkhxThemeEntry *b = *(const GtkhxThemeEntry *const *)bp;
    gboolean a_default = g_strcmp0 (a->name, "default") == 0;
    gboolean b_default = g_strcmp0 (b->name, "default") == 0;

    if (a_default && !b_default) {
        return -1;
    }
    if (b_default && !a_default) {
        return 1;
    }
    /* Locale-aware so "Şarki" sorts under S rather than after Z, etc.
     * Display names are UTF-8 by GKeyFile contract. */
    return g_utf8_collate (a->display ? a->display : "",
                           b->display ? b->display : "");
}

GPtrArray *
gtkhx_theme_list_available_at (const char *resource_prefix,
                               const char *user_themes_dir)
{
    GPtrArray *out = g_ptr_array_new_with_free_func (
        (GDestroyNotify)gtkhx_theme_entry_free);
    /* Tracks names we've already added so user-dir entries shadow
     * GResource entries with the same basename. Borrows the
     * GtkhxThemeEntry::name pointer — destruction of the array
     * outlives this set. */
    GHashTable *seen = g_hash_table_new (g_str_hash, g_str_equal);

    /* Walk the user dir first so its entries take precedence.
     * A "theme" surfaces either as a flat .ini file
     * (<name>.ini, no bundled icons) OR a directory containing a
     * theme.ini (<name>/theme.ini, can ship icons under
     * <name>/icons/). Skip dotfiles and anything that doesn't
     * match either shape. */
    if (user_themes_dir) {
        GDir *dir = g_dir_open (user_themes_dir, 0, NULL);
        if (dir) {
            /* Buffer the names so we can make two ordered passes over
             * them: dir-form bundles first, then flat .ini files. That
             * makes the documented "dir-form preferred" rule
             * deterministic regardless of readdir order, and the
             * seen-set stops a flat <name>.ini from also listing a
             * <name>/ bundle of the same name. */
            GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
            const char *fn;
            int pass;

            while ((fn = g_dir_read_name (dir)) != NULL) {
                if (fn[0] == '.') {
                    continue;
                }
                g_ptr_array_add (names, g_strdup (fn));
            }
            g_dir_close (dir);

            /* pass 0 = dir-form bundles, pass 1 = flat .ini */
            for (pass = 0; pass < 2; pass++) {
                guint i;
                for (i = 0; i < names->len; i++) {
                    const char *cn = names->pdata[i];
                    char *child = g_build_filename (user_themes_dir, cn, NULL);
                    gboolean is_dir = g_file_test (child, G_FILE_TEST_IS_DIR);
                    char *name = NULL;
                    char *display = NULL;

                    if (pass == 0 && is_dir) {
                        /* Dir-form: <child>/theme.ini must exist. */
                        char *manifest
                            = g_build_filename (child, "theme.ini", NULL);
                        if (g_file_test (manifest, G_FILE_TEST_IS_REGULAR)) {
                            name = g_strdup (cn);
                            display = read_display_name_from_file (manifest);
                        }
                        g_free (manifest);
                    } else if (pass == 1 && !is_dir && strlen (cn) > 4
                               && g_ascii_strcasecmp (cn + strlen (cn) - 4,
                                                      ".ini")
                                      == 0
                               && g_file_test (child, G_FILE_TEST_IS_REGULAR)) {
                        /* Flat-form: strip the .ini suffix. Require
                         * a regular file so a FIFO / dead symlink /
                         * other non-regular entry named "foo.ini"
                         * doesn't surface as an unselectable theme
                         * (gtkhx_theme_load_active needs IS_REGULAR
                         * to open it). */
                        name = strip_ini_suffix (cn);
                        display = read_display_name_from_file (child);
                    }

                    if (name && !theme_name_is_safe (name)) {
                        /* Path-separator in the directory or .ini
                         * basename — the loader would reject it
                         * anyway, so don't surface it in the picker. */
                        g_free (name);
                        g_free (display);
                        name = NULL;
                    }
                    if (name) {
                        if (g_hash_table_contains (seen, name)) {
                            g_free (name);
                            g_free (display);
                        } else {
                            GtkhxThemeEntry *e = g_new0 (GtkhxThemeEntry, 1);
                            e->name = name;
                            e->display = display ? display : g_strdup (name);
                            g_ptr_array_add (out, e);
                            g_hash_table_add (seen, e->name);
                        }
                    }
                    g_free (child);
                }
            }
            g_ptr_array_free (names, TRUE);
        }
    }

    /* Then enumerate the GResource prefix, skipping anything already
     * shadowed by a user-dir entry. GResource enumeration returns
     * dirs with a trailing "/", files without — same as a normal
     * VFS walk. */
    if (resource_prefix) {
        char **children = g_resources_enumerate_children (
            resource_prefix, G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
        if (children) {
            const char *sep
                = g_str_has_suffix (resource_prefix, "/") ? "" : "/";
            int pass;

            /* Two ordered passes, same "dir-form preferred" rule as the
             * user dir: pass 0 takes <name>/ bundles, pass 1 takes flat
             * <name>.ini, and the seen-set drops a flat entry when a
             * bundle of the same name (or a user theme) already won.
             * GResource enumeration returns dirs with a trailing "/",
             * files without — same as a normal VFS walk. */
            for (pass = 0; pass < 2; pass++) {
                for (char **p = children; *p; p++) {
                    char *name = NULL;
                    char *display = NULL;
                    gsize n = strlen (*p);
                    gboolean is_dir = (n > 0 && (*p)[n - 1] == '/');

                    if (pass == 0 && is_dir) {
                        /* Dir-form: child is "subdir/"; check for
                         * <prefix>/subdir/theme.ini. */
                        char *subdir = g_strndup (*p, n - 1);
                        char *manifest_res = g_strdup_printf (
                            "%s%s%s/theme.ini", resource_prefix, sep, subdir);
                        display
                            = read_display_name_from_resource (manifest_res);
                        if (display
                            || g_resources_get_info (
                                manifest_res, G_RESOURCE_LOOKUP_FLAGS_NONE,
                                NULL, NULL, NULL)) {
                            name = subdir;
                            subdir = NULL;
                        }
                        g_free (subdir);
                        g_free (manifest_res);
                    } else if (pass == 1 && !is_dir && n > 4
                               && g_ascii_strcasecmp (*p + n - 4, ".ini")
                                      == 0) {
                        name = strip_ini_suffix (*p);
                        char *resource_path = g_strdup_printf (
                            "%s%s%s", resource_prefix, sep, *p);
                        display
                            = read_display_name_from_resource (resource_path);
                        g_free (resource_path);
                    }

                    if (!name) {
                        continue;
                    }
                    if (!theme_name_is_safe (name)) {
                        g_free (name);
                        g_free (display);
                        continue;
                    }
                    if (g_hash_table_contains (seen, name)) {
                        g_free (name);
                        g_free (display);
                        continue;
                    }
                    GtkhxThemeEntry *e = g_new0 (GtkhxThemeEntry, 1);
                    e->name = name;
                    e->display = display ? display : g_strdup (name);
                    g_ptr_array_add (out, e);
                    g_hash_table_add (seen, e->name);
                }
            }
            g_strfreev (children);
        }
    }

    /* Guarantee "default" is always there even if neither source
     * surfaced it (shouldn't happen — the GResource ships it — but
     * a Settings combo that's empty would be alarming). */
    if (!g_hash_table_contains (seen, "default")) {
        GtkhxThemeEntry *e = g_new0 (GtkhxThemeEntry, 1);
        e->name = g_strdup ("default");
        e->display = g_strdup ("Default");
        g_ptr_array_add (out, e);
    }

    g_hash_table_unref (seen);

    g_ptr_array_sort (out, theme_entry_cmp);

    return out;
}

GPtrArray *
gtkhx_theme_list_available (void)
{
    char *user_dir = g_build_filename (gtkhx_config_dir (), "themes", NULL);
    GPtrArray *out = gtkhx_theme_list_available_at (
        "/com/nasledov/gtkhx/themes/", user_dir);
    g_free (user_dir);
    return out;
}

/* ---- snapshot accessors for the Rust settings theme combo ---------
 *
 * gtkhx_theme_list_available() hands back a fresh GPtrArray<GtkhxThemeEntry*>
 * each call; rather than expose GPtrArray internals + the entry struct across
 * FFI, the Rust "GtkHx theme" combo snapshots the list once (begin → n), reads
 * name/display by index (strings borrowed, valid until end), then frees it
 * (end). Not re-entrant — the combo is built synchronously in one pass. */
static GPtrArray *hx_theme_names_snapshot;

int
gtkhx_theme_names_begin (void)
{
    g_clear_pointer (&hx_theme_names_snapshot, g_ptr_array_unref);
    hx_theme_names_snapshot = gtkhx_theme_list_available ();
    if (!hx_theme_names_snapshot) {
        return 0;
    }
    /* Clamp the guint length to G_MAXINT so an absurd theme count can't
     * overflow the int return into a negative value (which the Rust side
     * would turn into a huge allocation). */
    guint len = hx_theme_names_snapshot->len;
    return len > (guint)G_MAXINT ? G_MAXINT : (int)len;
}

const char *
gtkhx_theme_names_name (int i)
{
    if (!hx_theme_names_snapshot || i < 0
        || (guint)i >= hx_theme_names_snapshot->len) {
        return "";
    }
    return ((GtkhxThemeEntry *)g_ptr_array_index (hx_theme_names_snapshot, i))
        ->name;
}

const char *
gtkhx_theme_names_display (int i)
{
    if (!hx_theme_names_snapshot || i < 0
        || (guint)i >= hx_theme_names_snapshot->len) {
        return "";
    }
    return ((GtkhxThemeEntry *)g_ptr_array_index (hx_theme_names_snapshot, i))
        ->display;
}

void
gtkhx_theme_names_end (void)
{
    g_clear_pointer (&hx_theme_names_snapshot, g_ptr_array_unref);
}
