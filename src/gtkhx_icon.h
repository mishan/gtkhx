/*
 * gtkhx_icon.h — chrome icon resolver + icon-pack discovery.
 *
 * Every chrome icon (toolbar + window-button glyphs, task-row icons,
 * news-thread row icons, file-type icons) goes through one chokepoint
 * — gtkhx_icon_load() — so a user can swap the look by dropping a
 * directory of replacement PNGs at $CONFIG/icons/<pack>/ and naming
 * it in the ICONPACK pref. Hotline user icons (cicn) are explicitly
 * NOT covered by this — they're protocol-shaped, see cicn.c.
 *
 * Lookup order (per icon, per call):
 *
 *   1. $CONFIG/icons/<active-pack>/<logical>.png  (the user's pack
 *      — works even for "default" so users CAN shadow the built-in
 *      glyphs by dropping files in $CONFIG/icons/default/)
 *   2. /com/nasledov/gtkhx/pixmaps/<logical>.png  (built-in GResource;
 *      always present for stock GtkHx icons)
 *
 * Per-icon fallback: a pack that ships only "connect.png" works fine
 * — every other icon falls back to the GResource. Returns NULL only
 * if neither source has the icon (shouldn't happen for stock names).
 *
 * The brand logo (gtkhx.png) is deliberately NOT swappable — it's
 * also loaded via gdk_pixbuf_new_from_resource directly in about.c,
 * not through this resolver.
 *
 * PNG-only in v1. SVG-pack support is a planned follow-up routing
 * through the existing hx-image-decode (glycin) Rust crate — see
 * docs/theming-scoping.md.
 *
 * See docs/icon-pack-format.md for the on-disk layout.
 */
#ifndef __gtkhx_ICON_H
#define __gtkhx_ICON_H

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

#endif /* ndef __gtkhx_ICON_H */
