/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 * Public License for more details.
 */

/*
 * dock_bridge.h — generic C dock-embed shim for the Rust window ports.
 *
 * libpanel + the HxPanel / dock infrastructure (hx_panel.c,
 * panel_registry.c, hx_panel_frame.c, hx_split.c, dock_layout*.c, the
 * toolbar's dock construction) stay in C: the gtk-rs libpanel crate is
 * a whole gtk-rs generation ahead of our pinned 0.21 stack, and the dock
 * infra is the large part regardless of bindings (see
 * docs/rust/dock-porting-scoping.md, Option A). Instead, each docked
 * window ported to gtk4-rs builds its *content widget tree* in Rust and
 * hands it to this bridge, which does the libpanel plumbing
 * (hx_panel_new / panel_frame_add / registry). Rust never names a
 * libpanel type; the kind / area enums cross as small ints mirrored in
 * the crate's `mod dock`.
 *
 * This is the same leaf-up shape as tracker_bridge.c / gtkhx_ui_bridge.c:
 * a small, permanent shim that keeps the wire/session/dock boundary where
 * it already is.
 */

#ifndef GTKHX_DOCK_BRIDGE_H
#define GTKHX_DOCK_BRIDGE_H 1

#include <glib.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Panel kind — mirrors HxPanelKind (hx_panel.h). Passed as an int so
 * Rust doesn't name the libpanel-adjacent enum. */
typedef enum {
    GTKHX_DOCK_KIND_CENTER  = 0, /* chat, news 1.5 (news browser), files */
    GTKHX_DOCK_KIND_SIDEBAR = 1, /* users, tasks, news */
    GTKHX_DOCK_KIND_DYNAMIC = 2, /* private chats, private messages */
} GtkhxDockKind;

/* Dock home area — mirrors the four toolbar frames. Maps to a
 * PanelArea *and* the matching toolbar_*_frame inside the bridge, so
 * the caller picks a single value and the area→frame pairing stays in
 * one place. */
typedef enum {
    GTKHX_DOCK_AREA_START  = 0, /* toolbar_sidebar_frame — News */
    GTKHX_DOCK_AREA_END    = 1, /* toolbar_end_frame     — Users */
    GTKHX_DOCK_AREA_BOTTOM = 2, /* toolbar_bottom_frame  — Tasks */
    GTKHX_DOCK_AREA_CENTER = 3, /* toolbar_center_frame  — Chat/Files/News15 */
} GtkhxDockArea;

/* Raise an already-open panel to focus; returns TRUE iff a panel with
 * this id was registered (in which case it's re-attached + raised and
 * the caller returns early instead of rebuilding content). Mirrors the
 * lookup → ensure_attached → raise head of each old create_X_window. */
gboolean gtkhx_dock_raise_if_open (const char *id);

/* Set / clear the needs-attention hint on a registered panel (the dock
 * tab strip badges it when the panel isn't the visible tab). No-op if no
 * panel with this id is registered. Used by the Rust chat-tabs manager to
 * flag the Chat panel when a background tab wants attention. */
void gtkhx_dock_set_needs_attention (const char *id, gboolean state);

/* Create-or-embed a static (CENTER / SIDEBAR) panel: builds the HxPanel
 * for `id`, titles/icons it, sets `content` as its child, adds it to the
 * home frame for `area`, records the home frame, and registers it (the
 * registry strong-refs it so it survives Close-all-pages).
 *
 * Returns TRUE on success. `content` is *always consumed* either way: on
 * success the panel takes its reference; on failure (the toolbar dock
 * isn't built yet — a g_critical) it is sunk and destroyed here, so the
 * caller never has to clean it up. Callers should skip any post-embed work
 * (e.g. after_embed) when this returns FALSE. Do not touch `content` after
 * the call regardless. */
gboolean gtkhx_dock_embed (const char   *id,
                           GtkhxDockKind kind,
                           GtkhxDockArea area,
                           const char   *title,
                           const char   *icon_name,
                           GtkWidget    *content);

/* Dynamic-panel variant (per-pchat / per-PM tabs): same embed, plus a
 * close trampoline. When the tab is closed, `on_close(user_data)` fires
 * (so Rust can tear down the backing gchat / msgwin state) before the
 * panel is unregistered and finalized. `user_data` is owned by the
 * caller conceptually but the bridge keeps it alive for the panel's
 * lifetime; if `destroy` is non-NULL it runs on `user_data` when the
 * panel finalizes. The panel is always created with
 * GTKHX_DOCK_KIND_DYNAMIC.
 *
 * Returns TRUE on success. Same `content` ownership as gtkhx_dock_embed
 * (always consumed — embedded on success, destroyed on failure). On
 * failure the close callback is never installed and `destroy` is run on
 * `user_data` immediately so the caller's teardown still fires. */
gboolean gtkhx_dock_embed_dynamic (const char   *id,
                                   GtkhxDockArea area,
                                   const char   *title,
                                   const char   *icon_name,
                                   GtkWidget    *content,
                                   void        (*on_close) (gpointer user_data),
                                   gpointer      user_data,
                                   GDestroyNotify destroy);

G_END_DECLS

#endif /* GTKHX_DOCK_BRIDGE_H */
