/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

/*
 * users_view.h — GtkColumnView-backed user list. Used by the
 * standalone Users window and by the per-pchat sidebars.
 *
 * Model chain per view:
 *
 *   GListStore<HxUserRow *>
 *     → GtkSortListModel (column-view header sorter)
 *     → GtkSingleSelection
 *     → GtkColumnView (UID + Name columns)
 *
 * Two columns:
 *   - UID  — narrow, right-aligned, sorted numerically
 *   - Name — Mac-classic overlay rendering. The icon paints at
 *            natural width starting at the cell's start edge;
 *            the name paints on top at a fixed text-x-offset
 *            from the start edge. Wide banner icons (60+ px on
 *            Badmoon and friends) render as the row background,
 *            so names stay column-aligned regardless of how wide
 *            the user's icon happens to be.
 *
 * Per-view construction parameters cover the visual differences
 * between the standalone Users window and the per-chat sidebars:
 * row height, pixel scale (Users window does 1.25× both icon
 * and font), text outline (Users window only), and text x-offset
 * (36 px in Users, 22 px in pchat lists).
 *
 * Selection state belongs to the view: callers ask via
 * hx_user_list_view_get_selected_user() rather than maintaining
 * a parallel `user_storow` integer global. The view also installs
 * its own right-click gesture + double-click activation; the
 * popup + msgwin opens are wired in via per-view callbacks set at
 * construction time so users.c keeps the existing user_popup /
 * msgwin code.
 */

#ifndef HX_USERS_VIEW_H
#define HX_USERS_VIEW_H 1

#include <gtk/gtk.h>

#include "hx.h" /* struct hx_user, session */

G_BEGIN_DECLS

#define HX_TYPE_USER_LIST_VIEW (hx_user_list_view_get_type ())
G_DECLARE_FINAL_TYPE (HxUserListView, hx_user_list_view, HX, USER_LIST_VIEW,
                      GObject)

/* Visual style flags. The Users window passes USERS for the
 * standalone-window appearance (26 px rows, 1.25× pixel scale,
 * text outline, 36-px text offset). Chat / pchat sidebars pass
 * CHAT for the compact appearance (18-px rows, 1.0× scale, no
 * outline, 36-px offset — same offset as the Users window so
 * medium-wide non-banner icons clear the name in both layouts). */
typedef enum {
    HX_USER_LIST_STYLE_USERS = 0, /* standalone Users window */
    HX_USER_LIST_STYLE_CHAT,      /* chat / pchat sidebar */
} HxUserListStyle;

/* Construct a fresh view. `sess` is borrowed for the lifetime
 * of the view — feeds the right-click handler so it can drive
 * the existing user_popup. */
extern HxUserListView *hx_user_list_view_new (session *sess,
                                              HxUserListStyle style);

/* Return the top-level widget to pack into a window. The widget
 * is the GtkColumnView itself; callers wrap it in their own
 * GtkScrolledWindow because the chrome differs by call site
 * (Users window has frame=FALSE, pchat windows have a GtkFrame
 * border, etc.). */
extern GtkWidget *hx_user_list_view_get_widget (HxUserListView *v);

/* Row management. Each `struct hx_user *` is a borrowed pointer;
 * the row holds it without ownership. Caller is responsible for
 * making sure the hx_user outlives any row entries that point
 * at it (in practice: the per-chat `chat->users` GHashTable
 * outlives the view). */
extern void hx_user_list_view_add (HxUserListView *v, struct hx_user *user,
                                   const char *nam, guint16 icon,
                                   guint16 color);
extern void hx_user_list_view_remove (HxUserListView *v, struct hx_user *user);
extern void hx_user_list_view_update (HxUserListView *v, struct hx_user *user,
                                      const char *nam, guint16 icon,
                                      guint16 color);
/* Re-snapshot `user`'s row to pick up a GIF avatar change (Phase 10.B).
 * No state mutation — the avatar lives in the gif_avatar cache, keyed
 * by uid; this just nudges the cell to re-read it. No-op if the view
 * has no row for `user`. */
extern void hx_user_list_view_refresh_avatar (HxUserListView *v,
                                              struct hx_user *user);
extern void hx_user_list_view_clear (HxUserListView *v);

/* Return the user under the live single-selection, or NULL if no
 * row is selected. This is what the headerbar toolbar buttons
 * (Msg / Kick / Info / Ban / Chat / Ignore) consult to know
 * which user to act on — replaces the old `user_storow` global
 * that was stamped on every primary press. */
extern struct hx_user *hx_user_list_view_get_selected_user (HxUserListView *v);

/* Apply font changes from the global users_font_desc (Settings
 * → Misc → User-list font). Called on prefs change so live views
 * pick up the new font without a window restart. */
extern void hx_user_list_view_refresh_font (HxUserListView *v);

/* The session this view was built against — borrowed pointer set
 * at construction. Shared with the headerbar / sidebar button
 * handlers (users.c::view_*_btn) so they can reach sess->htlc
 * without keeping a parallel "sess" qdata stash on every button. */
extern session *hx_user_list_view_get_session (HxUserListView *v);

G_END_DECLS

#endif /* HX_USERS_VIEW_H */
