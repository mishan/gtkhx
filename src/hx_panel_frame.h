/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * hx_panel_frame.h — HxPanelFrame : PanelFrame subclass.
 *
 * libpanel's PanelFrame installs class actions page.move-{left,
 * right,up,down} whose default handlers reflow pages across a
 * PanelGrid. We don't use PanelGrid (we use HxSplit), so those
 * defaults are a no-op and panel_frame_update_actions disables
 * them — which leaves the "Move Page L/R/U/D" items in the
 * libpanel-supplied chevron menu always greyed.
 *
 * Subclassing is the cleanest way to take those items over:
 * gtk_widget_class_install_action prepends to the per-class
 * priv->actions list, so installing page.move-{left,right,up,down}
 * here puts our actions at the head of HxPanelFrame's list. The
 * action muxer's lookup (action_muxer_query_action in
 * gtkactionmuxer.c) walks the list head-first by strcmp on the
 * action name, so our entries are what fires both for activation
 * and for enabled-state queries.
 *
 * libpanel still calls gtk_widget_action_set_enabled on the
 * 'page.move-*' names every selected-page / root / unroot;
 * because the lookup is by name, those calls disable OUR actions.
 * The hx_panel_frame instance therefore re-enables the four
 * actions after every relevant lifecycle event:
 *
 *   - notify::visible-child  (connect_after — runs once libpanel's
 *                             own handler has finished)
 *   - root vfunc override    (chain up, then re-enable)
 *   - unroot vfunc override  (same)
 *
 * Net effect on the chevron menu: the libpanel-supplied items
 * actually work (enabled when there's a panel to move; activation
 * calls our cross-frame neighbour move via
 * hx_panel_do_move_in_direction on the visible HxPanel).
 */

#ifndef GTKHX_HX_PANEL_FRAME_H
#define GTKHX_HX_PANEL_FRAME_H 1

#include <gtk/gtk.h>
#include <libpanel.h>

G_BEGIN_DECLS

#define HX_TYPE_PANEL_FRAME (hx_panel_frame_get_type ())
G_DECLARE_FINAL_TYPE (HxPanelFrame, hx_panel_frame, HX, PANEL_FRAME, PanelFrame)

/* Construct a fresh empty PanelFrame (subclass instance). Drop-in
 * replacement for panel_frame_new() at MAKE_LEAF_FRAME (toolbar.c)
 * and at hx_split_new()'s sibling-leaf factory (hx_split.c). */
PanelFrame *hx_panel_frame_new (void);

G_END_DECLS

#endif /* GTKHX_HX_PANEL_FRAME_H */
