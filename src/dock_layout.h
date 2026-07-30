/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * dock_layout.h — main-dock layout persistence.
 *
 * Saves the HxSplit tree shape, per-leaf panel placement, paned
 * divider positions, and toolbar window size to disk so the next
 * launch comes up with the user's last arrangement.
 *
 * File: $gtkhx_config_dir/dock-layout.ini (GKeyFile format).
 *
 *   [Dock]
 *   # Recursive s-expression:
 *   #   h(A,B)              horizontal split
 *   #   v(A,B)              vertical split
 *   #   L[id1,id2,...]      leaf with panel IDs (tab order)
 *   #   L[id1,id2,...:role] leaf tagged with a role (start, end,
 *   #                       bottom, center) for the four
 *   #                       toolbar_*_frame globals
 *   tree=h(L[news:start],h(v(L[chat,files,news15:center],L[tasks:bottom]),L[users:end]))
 *
 *   # Paned divider positions in depth-first order. Same count
 *   # as internal splits in the tree.
 *   sizes=240;620;380
 *
 * Save is coalesced on a 200 ms idle so a burst of changes (e.g.
 * the divider being dragged) writes the file once.
 *
 * Load policy: parse the file at toolbar build time; if either
 * the file is missing, malformed, or references unrecognised
 * panel IDs in a way the loader can't reconcile, fall through to
 * the default-layout code in toolbar.c. The defaults are always
 * the recovery path.
 *
 * Window size persistence lives in gtkhxrc via the existing
 * Window_Geo / gtkhx_prefs.geo.tool mechanism — don't duplicate
 * it here.
 *
 * Undocked windows: written under [Undocked] as one key per panel
 * id with value "W,H":
 *
 *   [Undocked]
 *   tracker=600,400
 *
 * Save: walks the panel registry; any panel whose root is not the
 * main toolbar window gets a key. Load: parses the section into a
 * one-shot pending-undock map. When each panel registers (factory
 * runs at startup), dock_layout_place_panel checks the map and,
 * if the panel was saved as undocked, calls hx_panel_undock + sets
 * the saved size on the resulting top-level window.
 */

#ifndef GTKHX_DOCK_LAYOUT_H
#define GTKHX_DOCK_LAYOUT_H 1

#include <gtk/gtk.h>
#include <libpanel.h>

#include "hx_panel.h"
#include "hx_split.h"

G_BEGIN_DECLS

/* Build an HxSplit tree from the saved layout file (if any) and
 * assign it to *out_root. Returns TRUE if a saved layout was
 * loaded and the tree was built; FALSE if no saved layout exists
 * or it couldn't be parsed (the caller falls through to the
 * default-layout code). On success, fills the four toolbar_*_frame
 * pointers from the role-tagged leaves and primes the internal
 * "place panel id X into frame Y on register" map. */
gboolean dock_layout_load (HxSplit **out_root, GtkWidget **out_sidebar_frame,
                           GtkWidget **out_center_frame,
                           GtkWidget **out_bottom_frame,
                           GtkWidget **out_end_frame);

/* Apply restored window size + paned positions. Call from the
 * toolbar window after the dock has been attached to its surface
 * (so widgets are realized and paned positions stick).
 *
 * No-op if dock_layout_load wasn't successful — the defaults are
 * already in place. */
void dock_layout_apply_geometry (GtkWindow *toolbar_window);

/* Request a save. Coalesces with other requests in the same
 * 200 ms window so a burst of changes (paned drag, repeated
 * splits) writes the file once. Safe to call from any of the
 * dock-mutation paths.
 *
 * The save walks the live HxSplit tree from the root, serialises
 * panel placement based on the registry, captures paned positions
 * depth-first, and writes the file atomically (g_file_set_contents). */
void dock_layout_request_save (void);

/* Hook called from hx_panel_registry_register: if the panel's id
 * appears in a saved-layout entry, reseat it to the saved frame.
 * No-op for unknown ids or when no saved layout is in effect. */
void dock_layout_place_panel (HxPanel *panel);

/* Delete the saved file and reset the in-memory map so the next
 * launch comes up with defaults. Wired into the hamburger menu's
 * Reset Layout action. */
void dock_layout_reset (void);

/* Init / shutdown bookends.
 *
 * dock_layout_init lazily allocates the id_to_frame hash table so
 * dock_layout_place_panel can be called even before the load /
 * default-build path runs. The registry-register hook lives in
 * src/panel_registry.c::hx_panel_registry_register, which calls
 * dock_layout_place_panel directly — no wiring is done here. The
 * autosave debounce lives inside dock_layout_request_save itself.
 *
 * dock_layout_shutdown flushes any pending debounced save
 * synchronously so the file is up to date by the time hx_quit
 * exits. */
void dock_layout_init (void);
void dock_layout_shutdown (void);

/* Tell the layout module which HxSplit node is the dock root.
 * Called by toolbar.c once the tree is mounted in toolbar_dock
 * — used by the save path to walk + serialise the current tree.
 * Stable for the lifetime of the toolbar window (hx_split_close_leaf
 * refuses to close the root). */
void dock_layout_set_dock_root (HxSplit *root);

G_END_DECLS

#endif /* GTKHX_DOCK_LAYOUT_H */
