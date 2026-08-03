/*
 * Copyright (C) 2000-2026 Misha Nasledov <misha@nasledov.com>
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
 * users_bridge.h — thin content-build shim for the Rust Users window.
 *
 * The Users *window shell* (raise-if-open, dock registration, lifecycle)
 * moved to gtkhx-ui's `users` module (Phase R5). The panel *content
 * leaves* that are still C — the custom HxUserListView (users_view.c),
 * the themed action buttons wired to the C view_*_btn handlers plus the
 * six sensitivity globals gtkutil.c toggles, and the optional voice
 * panel — are built here and handed back to Rust as opaque GtkWidgets.
 * Rust assembles them into the panel content tree and registers it
 * through dock_bridge. This keeps the view widget + button handlers in
 * C until they're ported in their own increment, matching the leaf-up
 * shape of tracker_bridge.c / dock_bridge.c.
 */

#ifndef GTKHX_USERS_BRIDGE_H
#define GTKHX_USERS_BRIDGE_H 1

#include <gtk/gtk.h>
#include "session.h"

G_BEGIN_DECLS

/* Build the Users panel content: a vertical GtkBox of the action-button
 * bar (Msg / Chat / [voice] / Info / Kick / Ban / Ignore, wired to the C
 * view_*_btn handlers, and stashed on sess->user_btns for the sensitivity
 * gating gtkutil.c drives) over the scrolled HxUserListView. Stashes the view
 * on sess->users_view and refreshes the user-list CSS/font. Returns the
 * container (a single still-floating widget) for the Rust shell to hand
 * to dock_bridge as the panel child. */
GtkWidget *gtkhx_users_bridge_build_content (session *sess);

/* Post-embed lifecycle: mark the panel open in prefs and, if this session's
 * connection is already up, sensitize its action buttons and populate the
 * list. */
void gtkhx_users_bridge_after_embed (session *sess);

G_END_DECLS

#endif /* GTKHX_USERS_BRIDGE_H */
