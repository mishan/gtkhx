/*
 * gtkhx_theme.h — themable presentation state.
 *
 * GtkhxTheme is the singleton that owns the user's active theming
 * choices (per-area scales, xtext palette, and future axes like icon
 * pack name). Every consumer subscribes to its "changed" signal and
 * re-reads via the gtkhx_theme_* accessors.
 *
 * All theming state lives in *theme files* — GKeyFile-format .ini at
 * $CONFIG/themes/<name>.ini, with a built-in default shipped as a
 * GResource (/com/nasledov/gtkhx/themes/default.ini). The active
 * theme is named by the THEMENAME pref in gtkhxrc. Theme files are
 * the only storage for this state; gtkhx_prefs does NOT carry
 * scale_* / palette_* fields. The scoping doc
 * (docs/theming.md) and the file-format reference
 * (docs/theming-file-format.md) cover the why and the schema.
 *
 * The scale model is deliberately honest. The unscaled source art is
 * the true 100% — the 16×16 button pixmaps, the user-list icon's
 * natural size, the base font. A theme then supplies a per-area
 * scale, and the built-in default theme encodes the real factors
 * GtkHx has always applied (toolbar / window buttons at 200%, the
 * standalone Users window at 125%) as explicit values rather than
 * pretending they're 100%. A user theme that omits a scale key
 * inherits the built-in default for that area.
 *
 * Call sites pass *no* base multiplier — they hand gtkhx_theme_scale()
 * their raw source dimension and let the theme own the whole factor.
 * There is no global multiplier stacked on top of hidden per-area
 * constants (the misleading shape the earlier single-knob `ui-scale`
 * experiment had).
 *
 * User-facing controls: a Settings → Appearance "GtkHx theme"
 * combo picks from the discovered themes (built-in + user). Editing
 * a theme's body (scale spin rows, color pickers, "save as") is
 * deferred to a separate theme-editor phase. For now the only way
 * to *change* a theme's contents is to edit (or drop in) a .ini
 * file under $CONFIG/themes/.
 */
#ifndef GTKHX_THEME_H
#define GTKHX_THEME_H

#include <glib-object.h>
#include <gdk/gdk.h>

G_BEGIN_DECLS

/* Clamp range for every scale knob (percent). 50% keeps small art
 * legible; 300% covers HiDPI / accessibility without letting a
 * fat-fingered value balloon a window to a compositor-hostile size. */
#define GTKHX_SCALE_MIN 50
#define GTKHX_SCALE_MAX 300

/* The independently-scalable parts of the UI. Keep
 * GTKHX_SCALE_N_AREAS last — it bounds the prefs-slot iteration. */
typedef enum {
    GTKHX_SCALE_TOOLBAR,        /* toolbar window button icons */
    GTKHX_SCALE_WINDOW_BUTTONS, /* action buttons in Users / Files /
                                 * News / Tasks / Tracker windows */
    GTKHX_SCALE_USERLIST_ICON,  /* user-list avatar icon */
    GTKHX_SCALE_USERLIST_TEXT,  /* user-list name text */
    GTKHX_SCALE_TASKS_ROW_ICON, /* per-task glyph in the tasks list */
    GTKHX_SCALE_N_AREAS
} GtkhxScaleArea;

#define GTKHX_TYPE_THEME (gtkhx_theme_get_type ())
G_DECLARE_FINAL_TYPE (GtkhxTheme, gtkhx_theme, GTKHX, THEME, GObject)

/* Process-wide singleton. Created lazily on first call. Every
 * subscriber connects to *this* instance's "changed" signal. */
GtkhxTheme *gtkhx_theme_get_default (void);

/* Clamp a raw percentage to [GTKHX_SCALE_MIN, GTKHX_SCALE_MAX]. */
int gtkhx_theme_clamp_percent (int pct);

/* The default theme's percentage for an area (the shipped factor:
 * 200 for buttons, 125 for the user list). Independent of any loaded
 * theme — useful for a Settings "reset to default" affordance and as
 * the fallback when the active theme omits a scale key. */
int gtkhx_theme_get_default_percent (GtkhxScaleArea area);

/* Effective integer percentage for an area: the active theme's value
 * if set, otherwise the built-in default. Clamped. */
int gtkhx_theme_get_percent (GtkhxScaleArea area);

/* Scale factor (percent / 100.0) for an area — the value a call site
 * multiplies into its base icon size or font size. */
double gtkhx_theme_scale (GtkhxScaleArea area);

/* ---- Palette (UI-role color slots) -----------------------------------
 *
 * The chat palette (see chat.c::colors[] and chat_view.h's
 * HX_CHAT_PAL_*) opens with the 32 mIRC slots, which are semantically
 * locked: servers send specific color indices and expect specific
 * colors. After them come the UI roles below, which are what a theme
 * sets.
 *
 * Each role carries two values: a *light* variant and a *dark*
 * variant. The active one is selected by AdwStyleManager's `dark`
 * property at apply time (see chat.c::gtkhx_apply_theme_palette) so
 * the chat surface follows the system theme without the user having
 * to redo their palette twice.
 *
 * FG and BG have no built-in color: left unset, they resolve to a
 * fully transparent GdkRGBA, which means "follow the system" — the
 * chat view and the text surfaces then take libadwaita's view colors,
 * so an untinted theme's chat matches the window around it. */

typedef enum {
    GTKHX_PAL_FG,             /* default text fg */
    GTKHX_PAL_BG,             /* default text bg */
    GTKHX_PAL_MARK_FG,        /* selection fg */
    GTKHX_PAL_MARK_BG,        /* selection bg */
    GTKHX_PAL_MARKER,         /* marker line */
    GTKHX_PAL_HISTORY_MUTED,  /* chat-history secondary text */
    GTKHX_PAL_TIMESTAMP,      /* timestamp column; defaults to history_muted */
    GTKHX_PAL_NICK,           /* other people's nicks; defaults to fg */
    GTKHX_PAL_SELF_NICK,      /* your own nick; defaults to fg */
    GTKHX_PAL_NICK_BRACKET,   /* the < > around other people's nicks */
    GTKHX_PAL_SELF_BRACKET,   /* the < > around your own nick */
    GTKHX_PAL_SYSTEM,         /* the "hx" in a "[hx]" status line */
    GTKHX_PAL_SYSTEM_BRACKET, /* the [ ] around it */
    GTKHX_PAL_HIGHLIGHT,      /* the nick on a line that mentions you */
    GTKHX_PAL_RULE,           /* the chat's column divider; defaults to fg */
    GTKHX_PAL_N_ROLES
} GtkhxPaletteRole;

/* Upper bound on a theme's `nick_colors` list. */
#define GTKHX_NICK_COLORS_MAX 8

/* Built-in default theme: the GdkRGBA shipped for each (role, variant).
 * Independent of any loaded theme — used as the fallback when the
 * active theme file omits a key and as the "reset to default" value
 * for a future theme editor. */
GdkRGBA gtkhx_theme_get_default_color (GtkhxPaletteRole role, gboolean dark);

/* Effective color for a (role, variant): the active theme's value if
 * set, otherwise the built-in default. */
GdkRGBA gtkhx_theme_get_color (GtkhxPaletteRole role, gboolean dark);

/* TRUE iff the active theme file explicitly set this (role, variant).
 * Distinguishes "theme intentionally chose this colour" from "theme
 * omitted the key and inherits the built-in default". Used by
 * gtkhx.c::gtkhx_refresh_css to decide whether to color the
 * listview-shaped surfaces (tracker / users / tasks / files / news
 * row backgrounds): if the active theme didn't opt in to chat fg/bg,
 * leave those surfaces at the system theme instead of forcing the
 * built-in defaults onto them. */
gboolean gtkhx_theme_palette_role_is_set (GtkhxPaletteRole role, gboolean dark);

/* The theme's `nick_colors` list for a variant: fills up to
 * GTKHX_NICK_COLORS_MAX entries of `out` and returns how many. Zero
 * when the theme has none, in which case every other person's nick
 * takes the NICK role. */
int gtkhx_theme_get_nick_colors (gboolean dark,
                                 GdkRGBA out[GTKHX_NICK_COLORS_MAX]);

/* ---- User-list name colors ------------------------------------------
 *
 * Names in the user list are colored by 2-bit status (idle / admin)
 * via a 4-slot palette: regular / idle / admin / admin-idle. The
 * historical defaults live in gtkhx.c::init_colors
 * (defaults_gdk_user_colors) and remain the fallback when a theme
 * doesn't opt in. A theme that DOES set these (Solarized does) gets
 * its values applied so names stay readable against the themed chat
 * background instead of inheriting CSS color from the row.
 *
 * Loaded via [users.light] / [users.dark] sections in the theme
 * file with keys `active`, `idle`, `admin`, `admin_idle`. Per-(slot,
 * variant) opt-in: a theme can set only the two it cares about and
 * the others fall back to the historical default. */

typedef enum {
    GTKHX_USER_COLOR_ACTIVE,     /* regular user */
    GTKHX_USER_COLOR_IDLE,       /* idle / away */
    GTKHX_USER_COLOR_ADMIN,      /* admin */
    GTKHX_USER_COLOR_ADMIN_IDLE, /* admin + idle */
    GTKHX_USER_COLOR_N
} GtkhxUserColor;

/* Fill *out with the active theme's user-list color for (slot,
 * variant) and return TRUE. If the theme didn't set this slot,
 * leave *out untouched and return FALSE — the caller falls back to
 * its historical default. */
gboolean gtkhx_theme_get_user_color (GtkhxUserColor slot, gboolean dark,
                                     GdkRGBA *out);

/* ---- Window chrome ---------------------------------------------------
 *
 * A theme can tint the whole window, not just the chat: the header
 * bar, pane headers, lists, popovers and the accent all follow it.
 * This works by overriding libadwaita's named colors (the
 * `--window-bg-color` family of CSS variables), so every stock widget
 * picks the theme up without per-widget rules.
 *
 * Loaded from [chrome.light] / [chrome.dark]. Every key is optional,
 * and any key may be `system`: left to the system theme and not
 * derived either, which is how a theme keeps stock chrome around its
 * own chat colors.
 * What a theme leaves out is derived: a missing `window` and `fg` fall
 * back to the chat palette's `bg` and `fg` — only when the palette sets
 * both, since a window background without its text color is unreadable
 * — and the header bar / sidebar / card / popover surfaces are nudged
 * off `window` toward `fg`. A theme that sets neither chrome keys nor
 * the chat palette's fg/bg leaves the chrome at the system theme, which
 * is what keeps the built-in default looking like stock GNOME. */

typedef enum {
    GTKHX_CHROME_WINDOW,       /* window background, dock gutters */
    GTKHX_CHROME_VIEW,         /* lists, text views */
    GTKHX_CHROME_HEADERBAR,    /* header bar, pane headers */
    GTKHX_CHROME_SIDEBAR,      /* sidebars */
    GTKHX_CHROME_CARD,         /* cards, boxed lists */
    GTKHX_CHROME_POPOVER,      /* popovers, dialogs, menus */
    GTKHX_CHROME_FG,           /* text on every surface above */
    GTKHX_CHROME_HEADERBAR_FG, /* header-bar text; defaults to fg */
    GTKHX_CHROME_ACCENT,       /* accent background (selection, toggles) */
    GTKHX_CHROME_ACCENT_FG,    /* text on the accent; picked if unset */
    GTKHX_CHROME_ACCENT_TEXT,  /* the accent as text (links, labels) */
    GTKHX_CHROME_ACTION,       /* suggested-action buttons, as an outline */
    GTKHX_CHROME_N_ROLES
} GtkhxChromeRole;

/* Resolve a chrome role for a variant — the theme's explicit value, or
 * the derived one. Returns FALSE when the theme leaves this role to
 * the system theme; *out is untouched in that case. */
gboolean gtkhx_theme_get_chrome_color (GtkhxChromeRole role, gboolean dark,
                                       GdkRGBA *out);

/* The CSS that applies the active theme's chrome for a variant: a
 * `:root` block of libadwaita color variables. Empty string when the
 * theme doesn't tint the chrome. Caller frees. */
char *gtkhx_theme_build_chrome_css (gboolean dark);

/* Active theme name — the THEMENAME pref value, or "default" if
 * the pref is unset / empty. Never NULL. Caller does NOT free.
 * Used by gtkhx_icon to find the active theme's bundled icons. */
const char *gtkhx_theme_active_name (void);

/* TRUE when the active theme asks for the classic pixel-art chrome
 * icons ([gtkhx-theme] icons = classic) rather than the symbolic set.
 * See gtkhx_icon_symbolic_name. */
gboolean gtkhx_theme_classic_icons (void);

/* ---- Loader ----------------------------------------------------------
 *
 * Theme files are GKeyFile .ini at $CONFIG/themes/<name>.ini, with the
 * built-in default at GResource /com/nasledov/gtkhx/themes/default.ini.
 * See docs/theming-file-format.md for the schema. */

/* Load the theme named by the THEMENAME pref (or "default" if unset).
 * Tries $CONFIG/themes/<name>.ini first, falls back to the built-in
 * default GResource. Emits "changed" on the singleton. Call once at
 * startup and again when THEMENAME changes. */
void gtkhx_theme_load_active (void);

/* Load theme state from an already-parsed GKeyFile. Exposed for tests
 * that want to drive a fixture without touching the filesystem.
 * Replaces all loaded state (so missing keys revert to the built-in
 * default — there is no "merge on top of previous"). Emits "changed". */
void gtkhx_theme_load_from_keyfile (GKeyFile *kf);

/* ---- Discovery -------------------------------------------------------
 *
 * Enumerate the themes available to pick from: built-ins shipped as
 * GResources under /com/nasledov/gtkhx/themes/<stem>.ini plus any
 * user themes at $CONFIG/themes/<stem>.ini. A user file shadows a same-name
 * GResource (so a user can override "default" or "solarized" with a
 * personal variant just by dropping a file in place).
 *
 * Each entry carries both the *file basename* (which is what goes
 * into the THEMENAME pref) and the *display name* (the
 * [gtkhx-theme] name = ... key from the file, falling back to the
 * basename if unset). The list is sorted with "default" first
 * (the obvious starting point) then alphabetically by display name. */

typedef struct {
    char *name;    /* basename without .ini — the THEMENAME value */
    char *display; /* user-visible title from the file, or basename */
} GtkhxThemeEntry;

void gtkhx_theme_entry_free (GtkhxThemeEntry *e);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GtkhxThemeEntry, gtkhx_theme_entry_free)

/* Returns GPtrArray<GtkhxThemeEntry *> with element free-func set,
 * so g_ptr_array_unref disposes the whole thing. Never returns NULL
 * — at minimum the built-in "default" theme is always present. */
GPtrArray *gtkhx_theme_list_available (void);

/* Same as gtkhx_theme_list_available but driven by an explicit
 * GResource enumeration prefix and a user-themes directory path,
 * for tests that want to drive a fixture instead of the live
 * GResource registry + $CONFIG. Either argument may be NULL to skip
 * that source. */
GPtrArray *gtkhx_theme_list_available_at (const char *resource_prefix,
                                          const char *user_themes_dir);

/* Snapshot accessors for the Rust settings "GtkHx theme" combo — begin
 * snapshots gtkhx_theme_list_available() and returns the count; name/display
 * read entry i (strings borrowed, valid until end frees the snapshot). Not
 * re-entrant. See gtkhx_theme.c. */
int gtkhx_theme_names_begin (void);
const char *gtkhx_theme_names_name (int i);
const char *gtkhx_theme_names_display (int i);
void gtkhx_theme_names_end (void);

G_END_DECLS

#endif /* ndef GTKHX_THEME_H */
