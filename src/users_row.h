/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

/*
 * users_row.h — one row in the chat / pchat / standalone users list.
 *
 * A GObject so it can sit inside a GListStore that GtkColumnView
 * consumes. Caches the values the cell renderers read (uid, nick
 * colour, name string, icon id, status color, computed foreground
 * GdkRGBA) — the authoritative per-chat membership lives in the Rust
 * HxMemberModel; the row is a self-contained view snapshot.
 *
 *   - The row owns its `name` g_strdup, its foreground GdkRGBA,
 *     and its GObject self. That's all.
 *
 * Every state-changing call (hx_user_row_set_state) fires a
 * "changed" signal so HxUserListView can poke its sort model and
 * its column-view cells re-snapshot — the bind callbacks read
 * fresh values out of the row's getters on each refresh.
 */

#ifndef HX_USERS_ROW_H
#define HX_USERS_ROW_H 1

#include <glib-object.h>
#include <gdk/gdk.h>

#include "hx.h" /* session + shared typedefs */

G_BEGIN_DECLS

#define HX_TYPE_USER_ROW (hx_user_row_get_type ())
G_DECLARE_FINAL_TYPE (HxUserRow, hx_user_row, HX, USER_ROW, GObject)

/* M4b.3b-ii-B: the row is built + mutated from Rust (HxUserListView), so
 * hx_user_row_new / _set_state / _touch are no longer part of the C ABI;
 * and hx_user_row_get_user is gone — the row no longer holds an hx_user*.
 * Only the cell-facing field getters below remain C-visible. */

/* Field accessors. Strings are NUL-terminated UTF-8 owned by the
 * row; callers MUST NOT free. user_color_gdk-style foreground is
 * NULL for the "regular user, theme default" slot. */
extern const char *hx_user_row_get_name (HxUserRow *row);
extern guint16 hx_user_row_get_icon (HxUserRow *row);
extern guint16 hx_user_row_get_color (HxUserRow *row);
extern const GdkRGBA *hx_user_row_get_foreground (HxUserRow *row);
extern guint16 hx_user_row_get_uid (HxUserRow *row);

G_END_DECLS

#endif /* HX_USERS_ROW_H */
