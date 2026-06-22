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
 * (docs/theming-scoping.md) and the file-format reference
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
 * User-facing controls for editing themes (a theme picker, color
 * pickers, "save as") are deferred to a separate theme-editor phase.
 * For now the only way to change a theme is to edit (or drop in) a
 * .ini file under $CONFIG/themes/ and set THEMENAME in gtkhxrc.
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
 * The xtext chat palette has 38 slots (see chat.c::colors[] and
 * xtext.h). Slots 0..31 are the mIRC palette — semantically locked
 * because servers send specific color indices and expect specific
 * colors. The remaining 6 are *UI roles* (foreground, background,
 * selection foreground / background, marker line, history-muted
 * secondary text), and these are the ones a theme can override.
 *
 * Each role carries two values: a *light* variant and a *dark*
 * variant. The active one is selected by AdwStyleManager's `dark`
 * property at apply time (see chat.c::gtkhx_apply_theme_palette) so
 * the chat surface follows the system theme without the user having
 * to redo their palette twice. */

typedef enum {
    GTKHX_PAL_FG,             /* XTEXT_FG (slot 34): default text fg */
    GTKHX_PAL_BG,             /* XTEXT_BG (slot 35): default text bg */
    GTKHX_PAL_MARK_FG,        /* XTEXT_MARK_FG (slot 32): selection fg */
    GTKHX_PAL_MARK_BG,        /* XTEXT_MARK_BG (slot 33): selection bg */
    GTKHX_PAL_MARKER,         /* XTEXT_MARKER (slot 36): marker line */
    GTKHX_PAL_HISTORY_MUTED,  /* XTEXT_HISTORY_MUTED (slot 37): secondary */
    GTKHX_PAL_N_ROLES
} GtkhxPaletteRole;

/* Built-in default theme: the GdkRGBA shipped for each (role, variant).
 * Independent of any loaded theme — used as the fallback when the
 * active theme file omits a key and as the "reset to default" value
 * for a future theme editor. */
GdkRGBA gtkhx_theme_get_default_color (GtkhxPaletteRole role, gboolean dark);

/* Effective color for a (role, variant): the active theme's value if
 * set, otherwise the built-in default. */
GdkRGBA gtkhx_theme_get_color (GtkhxPaletteRole role, gboolean dark);

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
    char *name;     /* basename without .ini — the THEMENAME value */
    char *display;  /* user-visible title from the file, or basename */
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

G_END_DECLS

#endif /* ndef GTKHX_THEME_H */
