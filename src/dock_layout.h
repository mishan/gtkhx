/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * dock_layout.h — what the main window keeps between runs.
 *
 * File: $gtkhx_config_dir/dock-layout.ini (GKeyFile format).
 *
 *   [Dock]
 *   # The dock's layout, as mullion-gtk keeps it: a tree of splits
 *   # with a share per child and leaves holding panel ids, the closed
 *   # panels, and the undocked windows with their sizes.
 *   layout={"layout":{"dir":"row","size":[0.2,0.8],"kids":[...]},...}
 *
 *   [Chrome]
 *   toolbar=true            # the pixmap toolbar; written only when on
 *   pane-titles=true        # tab strips rather than corner controls
 *   hidden-actions=news;users   # panels whose action row is hidden
 *
 *   [Windows]
 *   files=900,600           # windows that are not panels
 *
 * A file from before the dock was mullion-gtk has [Dock] tree=, sizes=
 * and closed= and an [Undocked] group instead of layout=; it is read
 * once, as the same layout (dl_import_legacy), and written back in the
 * new form at the next save.
 *
 * Saves are debounced on a 200 ms timer, so a burst of changes writes
 * the file once; dock_layout_shutdown flushes a pending one.
 */

#ifndef GTKHX_DOCK_LAYOUT_H
#define GTKHX_DOCK_LAYOUT_H 1

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Read the file: the chrome and window sizes into this module, and the
 * dock's layout returned, for mln_panes_load. NULL when there is none, or
 * none that reads, and the dock comes up on its default. */
char *dock_layout_load (void);

/* The layout the dock asks to keep (its ::layout-kept), or NULL to
 * forget it; written at the next save. */
void dock_layout_keep (const char *json);

void dock_layout_request_save (void);

gboolean dock_layout_panel_actions_hidden (const char *id);
void dock_layout_set_panel_actions_hidden (const char *id, gboolean hidden);
gboolean dock_layout_toolbar_visible (void);
void dock_layout_set_toolbar_visible (gboolean visible);
gboolean dock_layout_pane_titles_visible (void);
void dock_layout_set_pane_titles_visible (gboolean visible);

/* Sizes of windows that are not panels (Files), by name. */
gboolean dock_layout_get_window_size (const char *name, int *w, int *h);
void dock_layout_set_window_size (const char *name, int w, int h);

void dock_layout_shutdown (void);

G_END_DECLS

#endif /* GTKHX_DOCK_LAYOUT_H */
