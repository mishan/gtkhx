/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * panel_registry.h — the dock's panel ids, and which panels have been
 * built.
 *
 * The panels themselves are the dock's (dock_bridge.c): every id below
 * is a pane of it from startup, and the dock finds them by id.
 */

#ifndef GTKHX_PANEL_REGISTRY_H
#define GTKHX_PANEL_REGISTRY_H 1

#include <glib.h>

G_BEGIN_DECLS

/* Standard ids. New ids land here so collisions show up at compile
 * time, not runtime. */
#define HX_PANEL_ID_CHAT "chat"
#define HX_PANEL_ID_USERS "users"
#define HX_PANEL_ID_TASKS "tasks"
#define HX_PANEL_ID_NEWS "news"
#define HX_PANEL_ID_NEWS15 "news15"
/* No HX_PANEL_ID_FILES any more: the files browser is a window per
 * connection (gtkhx-ui/src/files.rs). A layout from before that still
 * names "files"; the import leaves it out (dl_import_legacy), and the
 * dock drops an id it has no pane for. */
#define HX_PANEL_ID_VIDEO "video"
/* No HX_PANEL_ID_TRACKER: the Tracker is a standalone top-level window,
 * not a docked panel. */

/* Every static panel id, NULL-terminated, in the order the startup path
 * builds them. Each is a pane of the dock from startup (dock_bridge.c). */
extern const char *const hx_panel_static_ids[];

/* "Has this panel ever been constructed?"
 *
 * A latch: set once when a panel finishes embedding, never cleared. It
 * used to be a one-bit field on the preferences struct, which is why
 * that struct also carried a shadow byte beside it — a bitfield has no
 * address for the settings table to point at. Both are gone; the flag
 * was never a preference, and it lives here because the panels do. */
void hx_panel_mark_constructed (const char *id);
gboolean hx_panel_was_constructed (const char *id);

G_END_DECLS

#endif /* GTKHX_PANEL_REGISTRY_H */
