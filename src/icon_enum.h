/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * icon_enum.h — the set of available Hotline icon IDs, for the Rust Settings
 * icon picker. See icon_enum.c for why this is a snapshot rather than a
 * mirrored struct.
 */

#ifndef GTKHX_ICON_ENUM_H
#define GTKHX_ICON_ENUM_H 1

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>

G_BEGIN_DECLS

/* Take a snapshot of the distinct icon IDs across every loaded resource file,
 * sorted ascending. Returns the count. Replaces any previous snapshot, so a
 * caller that forgets to end one leaks nothing worse than the last grid's
 * worth of `guint16`s. */
int hx_icon_ids_begin (void);

/* The `i`th ID, or -1 when `i` is out of range or no snapshot is live. */
int hx_icon_ids_nth (int i);

/* Release the snapshot. Safe to call without a live one. */
void hx_icon_ids_end (void);

/* Render one icon by ID, or NULL if nothing has it. Resolved through
 * `load_icon`, so the same first-match-wins file precedence the user list
 * draws with applies here — the picker cannot show an icon the rest of the
 * client would render differently. Transfer full.
 *
 * `fallback` selects between the two questions a caller can be asking. The
 * picker asks "is there an icon with this ID" and wants NULL when there
 * isn't (fallback=0). A *preview* is answering "what will other people see",
 * and every display path in the tree — the user list, the chat avatar —
 * substitutes DEFAULT_ICON for an unknown ID, so a preview that didn't would
 * show a blank where the client is about to draw something (fallback=1). */
GdkPixbuf *hx_icon_pixbuf_for_id (int id, int fallback);

G_END_DECLS

#endif /* ndef GTKHX_ICON_ENUM_H */
