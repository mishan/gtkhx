/*
 * gtkhx_theme.c — implementation of the themable-state singleton.
 * See gtkhx_theme.h, docs/theming-scoping.md, and
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

#define GTKHX_DEFAULT_THEME_RESOURCE \
    "/com/nasledov/gtkhx/themes/default.ini"

struct _GtkhxTheme {
    GObject parent_instance;

    /* Loaded scale overrides. 0 = "not set in the active theme" → fall
	 * back to default_theme_pct[]. */
    int scale_pct[GTKHX_SCALE_N_AREAS];

    /* Loaded palette overrides, packed as 0x00RRGGBB. -1 = "not set"
	 * → fall back to default_palette_{light,dark}[]. Two variants per
	 * role (index 0 = light, index 1 = dark). */
    int palette_rgb[GTKHX_PAL_N_ROLES][2];
};

G_DEFINE_FINAL_TYPE (GtkhxTheme, gtkhx_theme, G_TYPE_OBJECT)

enum {
    SIGNAL_CHANGED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

/* The built-in "default theme" scale table. These are the *real*
 * factors GtkHx has always applied, expressed honestly against the
 * unscaled source art (the 16×16 button pixmaps; the user-list icon's
 * natural size and the base font). The toolbar / window buttons have
 * always drawn at 2× their source art, and the standalone Users
 * window at 1.25×. */
static const int default_theme_pct[GTKHX_SCALE_N_AREAS] = {
    200, /* GTKHX_SCALE_TOOLBAR */
    200, /* GTKHX_SCALE_WINDOW_BUTTONS */
    125, /* GTKHX_SCALE_USERLIST_ICON */
    125, /* GTKHX_SCALE_USERLIST_TEXT */
};

/* Built-in default palette. Matches the historical chat.c
 * gtkhx_apply_theme_palette constants byte-for-byte. mIRC slots 0..31
 * stay in chat.c::colors[] because they're protocol-shaped, not
 * theme-shaped (servers send specific indices; users don't get to
 * remap "red"). */
#define RGB8(r, g, b) { (r) / 255.0, (g) / 255.0, (b) / 255.0, 1.0 }

static const GdkRGBA default_palette_light[GTKHX_PAL_N_ROLES] = {
    [GTKHX_PAL_FG]            = RGB8 (0x1d, 0x1d, 0x1d), /* near-black on white */
    [GTKHX_PAL_BG]            = RGB8 (0xfa, 0xfa, 0xfa), /* Adwaita view bg */
    [GTKHX_PAL_MARK_FG]       = RGB8 (0xff, 0xff, 0xff), /* selection contrast */
    [GTKHX_PAL_MARK_BG]       = RGB8 (0x35, 0x84, 0xe4), /* Adwaita accent */
    [GTKHX_PAL_MARKER]        = RGB8 (0xcc, 0x00, 0x00), /* red marker line */
    [GTKHX_PAL_HISTORY_MUTED] = RGB8 (0x5e, 0x5e, 0x5e), /* ~5.7:1 vs #fafafa */
};

static const GdkRGBA default_palette_dark[GTKHX_PAL_N_ROLES] = {
    [GTKHX_PAL_FG]            = RGB8 (0xcc, 0xcc, 0xcc), /* light grey on black */
    [GTKHX_PAL_BG]            = RGB8 (0x00, 0x00, 0x00),
    [GTKHX_PAL_MARK_FG]       = RGB8 (0xee, 0xee, 0xee),
    [GTKHX_PAL_MARK_BG]       = RGB8 (0x20, 0x4a, 0x87), /* Tango blue, original */
    [GTKHX_PAL_MARKER]        = RGB8 (0xcc, 0x00, 0x00),
    [GTKHX_PAL_HISTORY_MUTED] = RGB8 (0x9a, 0x9a, 0x9a), /* ~7:1 vs #000 */
};

#undef RGB8

static void
gtkhx_theme_class_init (GtkhxThemeClass *klass)
{
    /* Zero-argument notification. Subscribers re-read whatever
	 * scales / colors they care about and refresh. A single coalesced
	 * signal keeps fan-out cheap — every consumer already re-reads
	 * only its own slots. */
    signals[SIGNAL_CHANGED]
        = g_signal_new ("changed", G_TYPE_FROM_CLASS (klass),
                        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
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
        return gtkhx_theme_get_default_color (role, dark);
    }
    rgba.red   = ((packed >> 16) & 0xff) / 255.0;
    rgba.green = ((packed >>  8) & 0xff) / 255.0;
    rgba.blue  = ((packed      ) & 0xff) / 255.0;
    rgba.alpha = 1.0;
    return rgba;
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
    [GTKHX_SCALE_TOOLBAR]        = "toolbar",
    [GTKHX_SCALE_WINDOW_BUTTONS] = "window_buttons",
    [GTKHX_SCALE_USERLIST_ICON]  = "userlist_icon",
    [GTKHX_SCALE_USERLIST_TEXT]  = "userlist_text",
};

/* Mapping for the [palette.light] / [palette.dark] keys. */
static const char *const palette_key_name[GTKHX_PAL_N_ROLES] = {
    [GTKHX_PAL_FG]            = "fg",
    [GTKHX_PAL_BG]            = "bg",
    [GTKHX_PAL_MARK_FG]       = "mark_fg",
    [GTKHX_PAL_MARK_BG]       = "mark_bg",
    [GTKHX_PAL_MARKER]        = "marker",
    [GTKHX_PAL_HISTORY_MUTED] = "history_muted",
};

#define SCALE_GROUP "scale"
#define PALETTE_LIGHT_GROUP "palette.light"
#define PALETTE_DARK_GROUP "palette.dark"

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
            g_warning ("gtkhx_theme: bad color in [%s] %s = %s",
                       group, palette_key_name[r], raw ? raw : "");
        }
        g_free (raw);
    }
}

void
gtkhx_theme_load_from_keyfile (GKeyFile *kf)
{
    GtkhxTheme *self = gtkhx_theme_get_default ();
    int a;
    int r;

    if (!kf) {
        return;
    }

    /* Reset everything to "unset" first — load_from_keyfile is a
	 * replacement, not a merge. */
    for (a = 0; a < GTKHX_SCALE_N_AREAS; a++) {
        self->scale_pct[a] = 0;
    }
    for (r = 0; r < GTKHX_PAL_N_ROLES; r++) {
        self->palette_rgb[r][0] = -1;
        self->palette_rgb[r][1] = -1;
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

    g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

/* Resolve the on-disk path for the active theme. Returns a freshly
 * allocated string the caller frees, or NULL if no usable name. */
static char *
active_theme_path (void)
{
    const char *name;
    extern struct gtkhx_prefs gtkhx_prefs;

    name = gtkhx_prefs.theme_name && *gtkhx_prefs.theme_name
           ? gtkhx_prefs.theme_name
           : "default";

    /* Defensive: a name with a path separator could escape the themes
	 * directory. Reject and fall back to "default". */
    if (strchr (name, '/') || strchr (name, '\\')) {
        g_warning ("gtkhx_theme: rejecting theme name %s (path separator)",
                   name);
        name = "default";
    }

    return g_strdup_printf ("%s/themes/%s.ini", gtkhx_config_dir (), name);
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

    bytes = g_resources_lookup_data (resource_path,
                                     G_RESOURCE_LOOKUP_FLAGS_NONE, &err);
    if (!bytes) {
        if (err) {
            g_warning ("gtkhx_theme: no resource %s: %s",
                       resource_path, err->message);
            g_clear_error (&err);
        }
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

void
gtkhx_theme_load_active (void)
{
    char *path = active_theme_path ();
    GKeyFile *kf = NULL;

    if (path && g_file_test (path, G_FILE_TEST_IS_REGULAR)) {
        GError *err = NULL;
        kf = g_key_file_new ();
        if (!g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, &err)) {
            g_warning ("gtkhx_theme: load %s failed: %s — falling back to "
                       "built-in default",
                       path, err ? err->message : "(unknown)");
            g_clear_error (&err);
            g_key_file_free (kf);
            kf = NULL;
        }
    }

    /* Fall back to the built-in default GResource. If even that fails
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
		 * the path where neither the user file nor the resource
		 * loaded — keeps the boot sequence predictable. */
        g_signal_emit (gtkhx_theme_get_default (), signals[SIGNAL_CHANGED], 0);
    }

    g_free (path);
}
