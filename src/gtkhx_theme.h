/*
 * gtkhx_theme.h — themable presentation state.
 *
 * First landed for the per-area UI scaling work (see
 * docs/theming-scoping.md). GtkhxTheme is the singleton that owns the
 * user's theming choices and is the change-signal hub the rest of the
 * UI subscribes to. Today it carries the per-area scale knobs; icon
 * packs and the color palette plug into the same object and the same
 * "changed" signal as those axes land.
 *
 * The scale model is deliberately honest. The unscaled source art is
 * the true 100% — the 16x16 button pixmaps, the user-list icon's
 * natural size, the base font. A *theme* then supplies a per-area
 * scale, and the built-in "default theme" encodes the real factors
 * GtkHx has always applied (toolbar / window buttons at 200%, the
 * standalone Users window at 125%) as explicit values rather than
 * pretending they're 100%. A user override, when set, replaces the
 * default-theme value for that area.
 *
 * Call sites pass *no* base multiplier — they hand gtkhx_theme_scale()
 * their raw source dimension and let the theme own the whole factor.
 * There is no global multiplier stacked on top of hidden per-area
 * constants (the misleading shape the earlier single-knob `ui-scale`
 * experiment had).
 */
#ifndef GTKHX_THEME_H
#define GTKHX_THEME_H

#include <glib-object.h>

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
 * 200 for buttons, 125 for the user list). Independent of any user
 * override — useful for a Settings "reset to default" affordance. */
int gtkhx_theme_get_default_percent (GtkhxScaleArea area);

/* Effective integer percentage for an area: the user override if set,
 * otherwise the default theme's value. Clamped. */
int gtkhx_theme_get_percent (GtkhxScaleArea area);

/* Scale factor (percent / 100.0) for an area — the value a call site
 * multiplies into its base icon size or font size. */
double gtkhx_theme_scale (GtkhxScaleArea area);

/* Set an area's override percentage, write it back into gtkhx_prefs,
 * and emit "changed" if the value actually changed. A pct <= 0 clears
 * the override (the area reverts to the default theme's factor); a
 * positive value is clamped to [GTKHX_SCALE_MIN, GTKHX_SCALE_MAX]. */
void gtkhx_theme_set_percent (GtkhxScaleArea area, int pct);

/* Normalise every area's prefs value in place (clamp) and emit
 * "changed". Call after a bulk prefs reload / Settings apply that
 * wrote the gtkhx_prefs.scale_* fields directly. */
void gtkhx_theme_notify_changed (void);

G_END_DECLS

#endif /* ndef GTKHX_THEME_H */
