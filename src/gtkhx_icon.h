/*
 * gtkhx_icon.h — chrome icon resolver. Icons are part of the theme
 * bundle (see gtkhx_theme.{c,h}), not a separate icon-pack pref.
 *
 * Every chrome icon (toolbar + window-button glyphs, task-row icons,
 * news-thread row icons, file-type icons) goes through one chokepoint
 * — gtkhx_icon_load() — so a theme can swap the look by shipping its
 * own glyphs. A theme that wants custom icons lives as a *directory*
 * ($CONFIG/themes/<name>/ with an icons/ subdir) instead of a flat
 * .ini file. Hotline user icons (cicn) are explicitly NOT covered by
 * this — they're protocol-shaped, see cicn.c.
 *
 * Lookup order (per icon, per call; <active> is the active theme name):
 *
 *   1. $CONFIG/themes/<active>/icons/<logical>.png   (user theme dir)
 *   2. /com/nasledov/gtkhx/themes/<active>/icons/<logical>.png
 *                                                    (built-in theme dir)
 *   3. /com/nasledov/gtkhx/pixmaps/<logical>.png     (stock fallback;
 *      always present for stock GtkHx icons)
 *
 * Per-icon fallback: a theme dir that ships only "connect.png" works
 * fine — every other icon falls through to the stock pixmap. A
 * flat-form theme (just an .ini, no sibling dir) has nothing at steps
 * 1+2 and always lands on step 3. Returns NULL only if no source has
 * the icon (shouldn't happen for stock names).
 *
 * The brand logo (gtkhx.png) is deliberately NOT routed through this
 * resolver — it's loaded via gdk_pixbuf_new_from_resource directly in
 * about.c.
 *
 * PNG-only in v1. SVG-pack support is a planned follow-up routing
 * through the existing hx-image-decode (glycin) Rust crate — see
 * docs/theming.md.
 *
 * See docs/theming-file-format.md for the theme bundle layout.
 */
#ifndef GTKHX_ICON_H
#define GTKHX_ICON_H

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>

G_BEGIN_DECLS

/* Load a chrome icon at its native source size.
 *
 * The argument may be either a logical name ("connect") or a full
 * GResource path ("/com/nasledov/gtkhx/pixmaps/connect.png"). In
 * either case the resolver extracts the basename-without-.png and
 * runs the lookup order above. Caller owns the returned reference;
 * g_object_unref when done. NULL on miss. */
GdkPixbuf *gtkhx_icon_load (const char *name_or_path);

/* Drop any cached resolved-pixbuf state. Called from the theme
 * "changed" handler in gtkhx.c so a theme switch causes buttons
 * rebuilding via button_load_source to re-resolve against the new
 * theme's icons/ dir. Buttons subscribe to the same theme signal,
 * so the invalidate + rebuild happen in lockstep. */
void gtkhx_icon_invalidate_cache (void);

G_END_DECLS

#endif /* ndef GTKHX_ICON_H */
