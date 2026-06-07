/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * panel_registry.h — single source of truth for "which HxPanels exist
 * and how to find them by id".
 *
 * Phase 1 / docking. Replaces:
 *
 *   - The session-bound fields on struct _session:
 *     toolbar_window, news_window, chat_window, tasks_window,
 *     users_window. (These stay on the session struct during the
 *     Phase 2 transition — each migration drops the field and
 *     repoints any call site at the registry.)
 *
 *   - The file-static window globals:
 *     tracker_window      (tracker.c)
 *     post_window         (news.c)
 *     options_window      (options.c)
 *     about_window        (about.c)
 *     connect_window      (connect.c)
 *     the_browser         (news_browser.c)
 *     the_browser         (files_browser.c)
 *
 * Lookup is by stable string id (the same one stored on HxPanel).
 * The registry holds a strong reference to each registered panel
 * for the lifetime of the registration so the panel survives a
 * Frame removal during Undock without being destroyed.
 *
 * Lifetime: the registry is a process-global singleton (a
 * GHashTable lazily created on first call to register / lookup /
 * unregister). Panels are added via an explicit
 * hx_panel_registry_register call from each per-window factory.
 * Static panels (Users, Tasks, News, Chat, Files, News15) stay
 * registered for the lifetime of the process — they're permanent
 * dock residents and re-attachment after a close goes through
 * hx_panel_ensure_attached. Dynamic panels (Phase 3 reservation)
 * call hx_panel_registry_unregister explicitly when their backing
 * model object goes away. The HxPanel finalize handler does NOT
 * touch the registry — that would create a chicken-and-egg around
 * the registry's strong ref on the panel.
 *
 * Threading: main thread only. Workers that need to display a
 * panel must marshal via g_idle_add same as every other GTK call.
 */

#ifndef GTKHX_PANEL_REGISTRY_H
#define GTKHX_PANEL_REGISTRY_H 1

#include <glib.h>

#include "hx_panel.h"

G_BEGIN_DECLS

/* Standard ids — keep in sync with the per-window construction
 * sites as they're migrated in Phase 2. New ids land here so
 * collisions show up at compile time, not runtime. */
#define HX_PANEL_ID_CHAT      "chat"
#define HX_PANEL_ID_USERS     "users"
#define HX_PANEL_ID_TASKS     "tasks"
#define HX_PANEL_ID_NEWS      "news"
#define HX_PANEL_ID_NEWS15    "news15"
#define HX_PANEL_ID_FILES     "files"
#define HX_PANEL_ID_TRACKER   "tracker"

/* Register / lookup / unregister. register_panel is normally
 * called from each HxPanel-owning factory once the panel is
 * constructed; unregister is called when the panel is being
 * destroyed for good (not on a transient Undock). */
void     hx_panel_registry_register   (HxPanel    *panel);
void     hx_panel_registry_unregister (const char *id);
HxPanel *hx_panel_registry_lookup     (const char *id);

/* Iterate over every registered panel. Iteration order is
 * undefined; the callback must not register / unregister inside
 * the loop (GLib's hash-table iter would dislike it). */
typedef void (*HxPanelRegistryForeachFunc) (HxPanel  *panel,
                                            gpointer  user_data);
void     hx_panel_registry_foreach    (HxPanelRegistryForeachFunc func,
                                       gpointer                   user_data);

G_END_DECLS

#endif /* GTKHX_PANEL_REGISTRY_H */
