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
 * docs/docking.md, Option A). Instead, each docked
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
    GTKHX_DOCK_KIND_CENTER = 0,  /* chat, news 1.5 (news browser), files */
    GTKHX_DOCK_KIND_SIDEBAR = 1, /* users, tasks, news */
    GTKHX_DOCK_KIND_DYNAMIC = 2, /* private chats, private messages */
} GtkhxDockKind;

/* Dock home area — mirrors the four toolbar frames. Maps to a
 * PanelArea *and* the matching toolbar_*_frame inside the bridge, so
 * the caller picks a single value and the area→frame pairing stays in
 * one place. */
typedef enum {
    GTKHX_DOCK_AREA_START = 0,  /* toolbar_sidebar_frame — News */
    GTKHX_DOCK_AREA_END = 1,    /* toolbar_end_frame     — Users */
    GTKHX_DOCK_AREA_BOTTOM = 2, /* toolbar_bottom_frame  — Tasks */
    GTKHX_DOCK_AREA_CENTER = 3, /* toolbar_center_frame  — Chat/Files/News15 */
} GtkhxDockArea;

/* Raise an already-open panel to focus; returns TRUE iff a panel with
 * this id was registered (in which case it's re-attached + raised and
 * the caller returns early instead of rebuilding content). Mirrors the
 * lookup → ensure_attached → raise head of each old create_X_window. */
gboolean gtkhx_dock_raise_if_open (const char *id);

/* Whether `id` names an embedded panel — the same question
 * gtkhx_dock_raise_if_open answers, without doing anything about it.
 *
 * Both exist because two callers ask with opposite intent. One asks "should I
 * skip building?", and raising is the whole point of asking. The other asks
 * "does a panel already exist, so this connection's content is a page rather
 * than a new panel?" — a routing question, whose answer decides *how* to
 * place content, and which must not raise on its own account: whether to show
 * the result is a separate decision, made after, and only for the connection
 * the user is looking at. */
gboolean gtkhx_dock_is_embedded (const char *id);

/* Set / clear the needs-attention hint on a registered panel (the dock
 * tab strip badges it when the panel isn't the visible tab). No-op if no
 * panel with this id is registered. Used by the Rust chat-tabs manager to
 * flag the Chat panel when a background tab wants attention. */
void gtkhx_dock_set_needs_attention (const char *id, gboolean state);

/* Create-or-embed a static (CENTER / SIDEBAR) panel: builds the HxPanel
 * for `id`, titles/icons it, sets `content` as its first content page named
 * `page`, adds it to the home frame for `area`, records the home frame, and
 * registers it (the registry strong-refs it so it survives Close-all-pages).
 *
 * `page` names the connection the content belongs to — see the per-connection
 * page section below. This is the panel's *first* page; another connection's
 * content goes in through gtkhx_dock_add_page.
 *
 * Returns TRUE on success. `content` is *always consumed* either way: on
 * success the panel takes its reference; on failure (the toolbar dock
 * isn't built yet — a g_critical) it is sunk and destroyed here, so the
 * caller never has to clean it up. Callers should skip any post-embed work
 * (e.g. after_embed) when this returns FALSE. Do not touch `content` after
 * the call regardless. */
gboolean gtkhx_dock_embed (const char *id, GtkhxDockKind kind,
                           GtkhxDockArea area, const char *title,
                           const char *icon_name, const char *page,
                           GtkWidget *content);

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
/* ---- Per-connection content pages ------------------------------------ *
 *
 * A panel holds a *set* of named content pages rather than one child, so the
 * tab-switched layout can swap every per-connection panel at once when the
 * user changes connection. See dock_pages.h for the shape and for why
 * switching must not remove pages.
 *
 * `page` names the connection the content belongs to: its serial, rendered
 * as a string on the Rust side. While there is one connection a panel holds
 * exactly one page and behaves as the old single child did.
 *
 * An id with no panel reads as "no pages" throughout, matching what the
 * panel-level calls above do with an unknown id. */

/* The page name for content that belongs to no connection in particular.
 *
 * Nothing reaches it any more: the six per-connection panels name every page
 * after a connection, and the only other caller is gtkhx_dock_embed_dynamic,
 * which has no callers of its own — private chats and PMs became tabs inside
 * the Chat panel's shared AdwTabView rather than dynamic panels. Kept because
 * a dynamic panel would still need *a* page name, and deleting the constant
 * would not make the dead function less dead. */
#define HX_DOCK_PAGE_DEFAULT "default"

/* Add content as a new page of an already-embedded panel. Takes ownership of
 * `content` exactly as gtkhx_dock_embed does, on the failure path too.
 * FALSE if the panel isn't embedded or the page name is taken. */
gboolean gtkhx_dock_add_page (const char *id, const char *page,
                              GtkWidget *content);

/* Whether this connection already has content in this panel — the
 * per-connection form of the panel-level "is it open?" test. */
gboolean gtkhx_dock_has_page (const char *id, const char *page);

/* Make this connection's content the visible one. FALSE if there is no such
 * page, which is a no-op: a caller switching every panel at once shouldn't
 * have to know which roles a connection has content for. */
gboolean gtkhx_dock_show_page (const char *id, const char *page);

/* Remove a page and destroy its content tree. A real teardown — the content
 * modules' destroy handlers are their model-side teardown — so this is for
 * closing a connection, never for switching one. */
gboolean gtkhx_dock_remove_page (const char *id, const char *page);

/* How many content pages the panel holds. */
guint gtkhx_dock_page_count (const char *id);

gboolean gtkhx_dock_embed_dynamic (const char *id, GtkhxDockArea area,
                                   const char *title, const char *icon_name,
                                   GtkWidget *content,
                                   void (*on_close) (gpointer user_data),
                                   gpointer user_data, GDestroyNotify destroy);

G_END_DECLS

#endif /* GTKHX_DOCK_BRIDGE_H */
