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

    /* Loaded user-list name color overrides, same packed-int shape as
	 * palette_rgb. -1 = "not set" → caller (users.c::user_color_gdk)
	 * keeps its historical default. */
    int user_color_rgb[GTKHX_USER_COLOR_N][2];
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
    200, /* GTKHX_SCALE_TASKS_ROW_ICON — matches historical GTASK_ICON_SCALE = 2 */
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
    for (r = 0; r < GTKHX_USER_COLOR_N; r++) {
        self->user_color_rgb[r][0] = -1;
        self->user_color_rgb[r][1] = -1;
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

gboolean
gtkhx_theme_palette_role_is_set (GtkhxPaletteRole role, gboolean dark)
{
    GtkhxTheme *self = gtkhx_theme_get_default ();
    if (role < 0 || role >= GTKHX_PAL_N_ROLES) {
        return FALSE;
    }
    return self->palette_rgb[role][dark ? 1 : 0] >= 0;
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
    out->red   = ((packed >> 16) & 0xff) / 255.0;
    out->green = ((packed >>  8) & 0xff) / 255.0;
    out->blue  = ((packed      ) & 0xff) / 255.0;
    out->alpha = 1.0;
    return TRUE;
}

const char *
gtkhx_theme_active_name (void)
{
    return (gtkhx_prefs.theme_name && *gtkhx_prefs.theme_name)
           ? gtkhx_prefs.theme_name
           : "default";
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
    [GTKHX_SCALE_TOOLBAR]        = "toolbar",
    [GTKHX_SCALE_WINDOW_BUTTONS] = "window_buttons",
    [GTKHX_SCALE_USERLIST_ICON]  = "userlist_icon",
    [GTKHX_SCALE_USERLIST_TEXT]  = "userlist_text",
    [GTKHX_SCALE_TASKS_ROW_ICON] = "tasks_row_icon",
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
#define USERS_LIGHT_GROUP "users.light"
#define USERS_DARK_GROUP "users.dark"

static const char *const user_color_key_name[GTKHX_USER_COLOR_N] = {
    [GTKHX_USER_COLOR_ACTIVE]     = "active",
    [GTKHX_USER_COLOR_IDLE]       = "idle",
    [GTKHX_USER_COLOR_ADMIN]      = "admin",
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
            g_warning ("gtkhx_theme: bad color in [%s] %s = %s",
                       group, palette_key_name[r], raw ? raw : "");
        }
        g_free (raw);
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
            g_warning ("gtkhx_theme: bad color in [%s] %s = %s",
                       group, user_color_key_name[s], raw ? raw : "");
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

    /* NULL keyfile: nothing else to parse — fall through to the
	 * "changed" emit so subscribers reset in lockstep. */
    if (!kf) {
        g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
        return;
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
    char *dir_path = g_strdup_printf ("%s/themes/%s/theme.ini",
                                      gtkhx_config_dir (), name);
    GKeyFile *kf = NULL;
    GError *err = NULL;

    if (g_file_test (dir_path, G_FILE_TEST_IS_REGULAR)) {
        kf = g_key_file_new ();
        if (!g_key_file_load_from_file (kf, dir_path, G_KEY_FILE_NONE,
                                        &err)) {
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

    char *flat_path = g_strdup_printf ("%s/themes/%s.ini",
                                       gtkhx_config_dir (), name);
    if (g_file_test (flat_path, G_FILE_TEST_IS_REGULAR)) {
        kf = g_key_file_new ();
        if (!g_key_file_load_from_file (kf, flat_path, G_KEY_FILE_NONE,
                                        &err)) {
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
    char *dir_res = g_strdup_printf ("/com/nasledov/gtkhx/themes/%s/theme.ini",
                                     name);
    GKeyFile *kf = load_keyfile_from_resource (dir_res);
    g_free (dir_res);
    if (kf) {
        return kf;
    }
    char *flat_res = g_strdup_printf ("/com/nasledov/gtkhx/themes/%s.ini",
                                      name);
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
        (GDestroyNotify) gtkhx_theme_entry_free);
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
            const char *fn;
            while ((fn = g_dir_read_name (dir)) != NULL) {
                if (fn[0] == '.') {
                    continue;
                }
                char *child = g_build_filename (user_themes_dir, fn, NULL);
                char *name = NULL;
                char *display = NULL;

                if (g_file_test (child, G_FILE_TEST_IS_DIR)) {
                    /* Dir-form: <child>/theme.ini must exist. */
                    char *manifest
                        = g_build_filename (child, "theme.ini", NULL);
                    if (g_file_test (manifest, G_FILE_TEST_IS_REGULAR)) {
                        name = g_strdup (fn);
                        display = read_display_name_from_file (manifest);
                    }
                    g_free (manifest);
                } else if ((g_str_has_suffix (fn, ".ini")
                            || g_str_has_suffix (fn, ".INI"))
                           && g_file_test (child, G_FILE_TEST_IS_REGULAR)) {
                    /* Flat-form: strip the .ini suffix. Require a
					 * regular file so a FIFO / dead symlink /
					 * other non-regular entry named "foo.ini"
					 * doesn't surface as an unselectable theme
					 * (gtkhx_theme_load_active needs IS_REGULAR
					 * to open it). */
                    name = strip_ini_suffix (fn);
                    display = read_display_name_from_file (child);
                }

                if (name) {
                    GtkhxThemeEntry *e = g_new0 (GtkhxThemeEntry, 1);
                    e->name = name;
                    e->display = display ? display : g_strdup (name);
                    g_ptr_array_add (out, e);
                    g_hash_table_add (seen, e->name);
                }
                g_free (child);
            }
            g_dir_close (dir);
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
            for (char **p = children; *p; p++) {
                char *name = NULL;
                char *display = NULL;
                gsize n = strlen (*p);

                if (n > 0 && (*p)[n - 1] == '/') {
                    /* Dir-form: child is "subdir/"; check for
                     * <prefix>/subdir/theme.ini. */
                    char *subdir = g_strndup (*p, n - 1);
                    char *manifest_res = g_strdup_printf (
                        "%s%s%s/theme.ini", resource_prefix, sep, subdir);
                    display = read_display_name_from_resource (manifest_res);
                    if (display
                        || g_resources_get_info (
                            manifest_res, G_RESOURCE_LOOKUP_FLAGS_NONE,
                            NULL, NULL, NULL)) {
                        name = subdir;
                        subdir = NULL;
                    }
                    g_free (subdir);
                    g_free (manifest_res);
                } else if (n > 4
                           && g_ascii_strcasecmp (*p + n - 4, ".ini") == 0) {
                    name = strip_ini_suffix (*p);
                    char *resource_path = g_strdup_printf (
                        "%s%s%s", resource_prefix, sep, *p);
                    display = read_display_name_from_resource (resource_path);
                    g_free (resource_path);
                }

                if (!name) {
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
