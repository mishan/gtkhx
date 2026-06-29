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
 * consumes. Wraps a borrowed `struct hx_user *` plus the display
 * state the cell renderers read (name string, icon id, status
 * color, computed foreground GdkRGBA). Lifetime split mirrors
 * tracker_row:
 *
 *   - The underlying `struct hx_user` is still owned by the
 *     per-chat `chat->users` GHashTable. The row holds a
 *     borrowed pointer; the row's GObject finalize never
 *     touches it.
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

#include "hx.h" /* struct hx_user — borrowed pointer */

G_BEGIN_DECLS

#define HX_TYPE_USER_ROW (hx_user_row_get_type ())
G_DECLARE_FINAL_TYPE (HxUserRow, hx_user_row, HX, USER_ROW, GObject)

/* Construct a row over a borrowed `user`. `nam` is g_strdup'd into
 * the row; `icon` and `color` are stored verbatim. `color` is the
 * 2-bit status field (idle / admin), same shape user_color_gdk
 * reads. The foreground is computed via user_nick_color_gdk and
 * cached for the cell renderer; pass user==NULL only in the
 * unusual "placeholder row" case. */
extern HxUserRow *hx_user_row_new (struct hx_user *user, const char *nam,
                                   guint16 icon, guint16 color);

/* In-place mutator. Triggers the "changed" signal so the view's
 * sort model re-orders + the cell re-snapshots. Stash whatever
 * the caller just received from a USER_CHANGE event — name, icon,
 * status color — without throwing away the row identity. */
extern void hx_user_row_set_state (HxUserRow *row, const char *nam,
                                   guint16 icon, guint16 color);

/* Fire "changed" without mutating any state. Used when something the
 * cell reads outside the row's own fields changed — e.g. this user's
 * GIF avatar landed in the avatar cache — so the cell re-snapshots and
 * picks it up. The sort key is unchanged, so the row keeps its
 * position. */
extern void hx_user_row_touch (HxUserRow *row);

/* Field accessors. Strings are NUL-terminated UTF-8 owned by the
 * row; callers MUST NOT free. user_color_gdk-style foreground is
 * NULL for the "regular user, theme default" slot. */
extern struct hx_user *hx_user_row_get_user (HxUserRow *row);
extern const char *hx_user_row_get_name (HxUserRow *row);
extern guint16 hx_user_row_get_icon (HxUserRow *row);
extern guint16 hx_user_row_get_color (HxUserRow *row);
extern const GdkRGBA *hx_user_row_get_foreground (HxUserRow *row);
extern guint16 hx_user_row_get_uid (HxUserRow *row);

G_END_DECLS

#endif /* HX_USERS_ROW_H */
