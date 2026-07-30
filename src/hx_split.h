/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * hx_split.h — recursive split container for the dock.
 *
 * Phase 5b / docking. See docs/docking-splits.md for the design
 * and migration history.
 *
 * An HxSplit is either:
 *   - a LEAF wrapping a single PanelFrame, or
 *   - an INTERNAL SPLIT wrapping a 2-way GtkPaned whose two
 *     children are themselves HxSplits.
 *
 * The two states are mutually exclusive and the widget tree mirrors
 * them. The two transitions are:
 *
 *   hx_split_split (leaf, orientation) — turn the leaf into a
 *     paned with the leaf's original PanelFrame in the start
 *     child and a fresh empty leaf in the end child.
 *
 *   hx_split_close_leaf (leaf) — destroy the leaf; its sibling
 *     under the same parent paned collapses up to replace the
 *     parent's position in the tree. No-op when the leaf has no
 *     parent split (i.e. it IS the dock root) — the dock must
 *     always have at least one leaf, possibly empty.
 *
 * The toolbar dock is a SINGLE recursive HxSplit tree (Phase 5b
 * unified the previous four per-area trees into one). Splits,
 * moves, and closes all operate over the same tree;
 * hx_split_neighbor walks the whole dock for relative-direction
 * moves; there are no internal area boundaries.
 *
 * The four default-leaf PanelFrames (Users, Tasks, News+News15,
 * Chat+Files) start the dock; the toolbar_*_frame globals in
 * toolbar.c reference them so static-panel factories have stable
 * panel_frame_add targets across user splits.
 */

#ifndef GTKHX_HX_SPLIT_H
#define GTKHX_HX_SPLIT_H 1

#include <gtk/gtk.h>
#include <libpanel.h>

G_BEGIN_DECLS

#define HX_TYPE_SPLIT (hx_split_get_type ())
G_DECLARE_FINAL_TYPE (HxSplit, hx_split, HX, SPLIT, GtkWidget)

/* Construct a leaf wrapping a fresh empty PanelFrame. Mostly used
 * by hx_split_split when the new sibling needs a blank canvas. */
HxSplit *hx_split_new (void);

/* Construct a leaf wrapping an existing PanelFrame. The frame
 * becomes a child of the new HxSplit (its previous parent, if any,
 * MUST be NULL — the typical use site is right after
 * panel_frame_new). */
HxSplit *hx_split_new_with_frame (PanelFrame *frame);

/* Construct an internal-split HxSplit directly from two existing
 * HxSplit children + an orientation. Both children must be
 * unparented; they become start_child / end_child of the new
 * GtkPaned. Useful for building a default layout tree top-down
 * (the leaves get constructed first, then nested splits wrap
 * them) without going through hx_split_split which manufactures
 * a blank sibling for you. */
HxSplit *hx_split_new_internal (HxSplit *child_a, HxSplit *child_b,
                                GtkOrientation orientation);

/* True iff this node currently holds a PanelFrame leaf. */
gboolean hx_split_is_leaf (HxSplit *self);

/* Leaf accessor. NULL on an internal split. */
PanelFrame *hx_split_get_frame (HxSplit *self);

/* Internal-split accessors. NULL / 0 on a leaf. The orientation
 * is the orientation of the paned (GTK_ORIENTATION_HORIZONTAL =
 * side-by-side children, GTK_ORIENTATION_VERTICAL = stacked). */
HxSplit *hx_split_get_child_a (HxSplit *self);
HxSplit *hx_split_get_child_b (HxSplit *self);
GtkOrientation hx_split_get_orientation (HxSplit *self);

/* Underlying GtkPaned for an internal-split node. NULL on a leaf.
 * Exposed so callers can tweak paned-level properties — divider
 * position, shrink semantics — that don't have a meaningful
 * HxSplit-level abstraction. The pointer is owned by the HxSplit;
 * don't unparent or sink. */
GtkPaned *hx_split_get_paned (HxSplit *self);

/* Convert a leaf into an internal split. The current PanelFrame
 * becomes the start child (child_a)'s frame; a brand-new empty
 * PanelFrame leaf is created for child_b. Returns the new sibling
 * leaf so the caller can populate it. Returns NULL and warns when
 * called on an internal split. */
HxSplit *hx_split_split (HxSplit *self, GtkOrientation orientation);

/* Collapse a leaf. The leaf's PanelFrame is destroyed and its
 * sibling under the same parent paned takes the parent's place in
 * the tree. Returns TRUE if the close actually happened. Returns
 * FALSE (and is a no-op) when:
 *   - the leaf is the area root (no parent to collapse into), OR
 *   - the leaf's PanelFrame still has pages (caller should move
 *     panels out first via panel_frame_remove / panel_frame_add). */
gboolean hx_split_close_leaf (HxSplit *self);

/* Walk the tree from this leaf looking for the leaf that lies in
 * the given direction. Returns NULL if no such leaf exists in this
 * tree. Used by the chevron menu's relative-move actions
 * (panel.move-left / right / up / down). */
HxSplit *hx_split_neighbor (HxSplit *leaf, GtkDirectionType direction);

/* Find the HxSplit leaf that contains the given PanelFrame. NULL
 * if the frame isn't in this tree. */
HxSplit *hx_split_find_for_frame (HxSplit *root, PanelFrame *frame);

/* Iterate over every leaf in left-to-right / top-to-bottom order.
 * The callback must not mutate the tree (no split / close from
 * inside; collect leaves first if a mutating pass is needed). */
typedef void (*HxSplitLeafFunc) (HxSplit *leaf, gpointer user_data);
void hx_split_foreach_leaf (HxSplit *root, HxSplitLeafFunc func,
                            gpointer user_data);

/* Install the per-frame split UI on the given PanelFrame:
 *
 *   - A GtkMenuButton suffix on the frame's header with a small
 *     menu containing "Split horizontally", "Split vertically",
 *     and "Close frame".
 *   - A GSimpleActionGroup at prefix "frame-ops" on the frame
 *     widget itself (via gtk_widget_insert_action_group, NOT
 *     PanelActionMuxer — keeps the action plumbing simple and
 *     avoids the two-level page.panel.* prefix that the panel
 *     chevron menu has to use).
 *
 * Called once per leaf PanelFrame at construction (toolbar dock
 * setup and every hx_split_split). Empty frames keep the button
 * visible — that's how the user discovers split + close on an
 * otherwise-empty area. */
void hx_split_install_frame_ui (GtkWidget *frame);

G_END_DECLS

#endif /* GTKHX_HX_SPLIT_H */
