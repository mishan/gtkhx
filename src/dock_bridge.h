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
 * dock_bridge.h — the dock, and the C dock-embed shim for the Rust window
 * ports.
 *
 * The dock is one mullion-gtk MlnPanes: a split tree of tabbed panes the
 * user drags, splits, closes and moves into windows of its own, kept in
 * dock-layout.ini (dock_layout.c). Every static panel (panel_registry.h)
 * is a pane of it from startup, with an empty page stack for content;
 * each docked window builds its *content widget tree* in Rust and hands
 * it to this bridge, which puts it in the pane's stack as a page. Rust
 * never names a dock type; the kind / area enums cross as small ints
 * mirrored in the crate's `mod dock`.
 */

#ifndef GTKHX_DOCK_BRIDGE_H
#define GTKHX_DOCK_BRIDGE_H 1

#include <glib.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Panel kind, as the Rust side names it. The dock does not tell them
 * apart any more; kept for the ABI. */
typedef enum {
    GTKHX_DOCK_KIND_CENTER = 0,  /* chat, news 1.5 (news browser), files */
    GTKHX_DOCK_KIND_SIDEBAR = 1, /* users, tasks, news */
    GTKHX_DOCK_KIND_DYNAMIC = 2, /* private chats, private messages */
} GtkhxDockKind;

/* Dock home area: the slot a panel goes to where the layout has no place
 * for it (see dock_bridge.c's panel table, which is what decides now; the
 * area a caller passes is kept for the ABI). */
typedef enum {
    GTKHX_DOCK_AREA_START = 0,  /* News */
    GTKHX_DOCK_AREA_END = 1,    /* Users */
    GTKHX_DOCK_AREA_BOTTOM = 2, /* Tasks */
    GTKHX_DOCK_AREA_CENTER = 3, /* Chat, News15 */
} GtkhxDockArea;

/* Raise an embedded panel to focus, out of the drawer if it was closed;
 * returns TRUE iff the panel has content (in which case the caller returns
 * early instead of rebuilding it). Until gtkhx_dock_settled, it raises
 * nothing: startup's own opens would otherwise decide which tab is in
 * front of each leaf, which the saved layout says. */
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

/* Set / clear the needs-attention mark on a panel's tab, until the panel
 * comes into view. Used by the Rust chat-tabs manager to flag the Chat
 * panel when a background tab wants attention. */
void gtkhx_dock_set_needs_attention (const char *id, gboolean state);

/* Embed a static panel's first content: titles it, and puts `content` in
 * its pane as the first page, named `page`. The pane is the dock's from
 * startup, wherever the layout has it; a closed one stays closed.
 *
 * `page` names the connection the content belongs to — see the per-connection
 * page section below. This is the panel's *first* page; another connection's
 * content goes in through gtkhx_dock_add_page.
 *
 * Returns TRUE on success. `content` is *always consumed* either way: on
 * success the panel takes its reference; on failure (no dock yet, or no
 * panel by that id — a g_critical) it is sunk and destroyed here, so the
 * caller never has to clean it up. Callers should skip any post-embed work
 * (e.g. after_embed) when this returns FALSE. Do not touch `content` after
 * the call regardless. */
gboolean gtkhx_dock_embed (const char *id, GtkhxDockKind kind,
                           GtkhxDockArea area, const char *title,
                           const char *icon_name, const char *page,
                           GtkWidget *content);

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

/* The page name for content that belongs to no connection in particular. */
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

/* Destroy a session's content page in every per-connection panel — the whole
 * of what closing a connection has to unwind on the view side. Implemented in
 * gtkhx-ui (dock.rs), which is where the list of per-connection panels lives.
 *
 * A remove, not a switch-away: each content module's destroy handler is its
 * model-side teardown. */
struct _session;
extern void gtkhx_dock_remove_session_pages (struct _session *sess);

/* Call `func' each time the panel comes into view: raised, its tab
 * clicked, reopened. For the News browser's fetch on open. */
void gtkhx_dock_connect_shown (const char *id, void (*func) (void));

/* ---- The dock itself: C only ----------------------------------------- */

/* Make the dock: every static panel a pane, the saved layout read and put
 * up (or the default). Once, for the main window. */
GtkWidget *gtkhx_dock_new (void);

/* Startup is done opening panels: raises from here on are the user's. */
void gtkhx_dock_settled (void);

/* Whether a panel is in the dock or a window of its own, rather than
 * closed. */
gboolean gtkhx_dock_is_open (const char *id);

/* In front, out of the drawer if it was closed, with the focus -- and its
 * window presented, when it is in one of its own. */
void gtkhx_dock_present (const char *id);

/* In front of its leaf if it is open, without the focus; nothing if it
 * was closed. For a panel that has something to show, as Tasks does when
 * a transfer starts. */
void gtkhx_dock_show_if_open (const char *id);

/* Tab strips (TRUE) or the corner's icons (FALSE): Pane Titles. */
void gtkhx_dock_set_pane_titles (gboolean on);

/* The default layout, now. */
void gtkhx_dock_reset (void);

/* The per-panel Show Action Bar actions, app.pane-actions-<id>. */
void gtkhx_dock_add_actions (GActionMap *map);

G_END_DECLS

#endif /* GTKHX_DOCK_BRIDGE_H */
