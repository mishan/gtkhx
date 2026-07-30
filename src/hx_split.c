/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * hx_split.c — recursive split container. See hx_split.h + the
 * design doc at docs/docking-splits.md.
 *
 * Implementation notes:
 *
 *  - HxSplit uses GtkBinLayout. The one visible child is either
 *    the leaf PanelFrame (when self->frame is non-NULL) or the
 *    GtkPaned (when self->paned is non-NULL). Both states being
 *    NULL is an error after the first hx_split_new returns.
 *
 *  - State transitions reparent widgets explicitly. GTK 4 has no
 *    gtk_widget_reparent; we use g_object_ref + gtk_widget_unparent
 *    + gtk_widget_set_parent, which is the standard idiom.
 *
 *  - close_leaf collapses the parent split: the surviving sibling
 *    is reparented into the grandparent slot (or the parent's
 *    parent if it's an HxSplit, or into self if it's the area root
 *    — but root close is a no-op so we never have to handle that
 *    case in close_leaf).
 *
 *  - hx_split_neighbor walks up to find an ancestor split whose
 *    orientation matches the requested direction, then walks back
 *    down picking the closest edge to the original leaf.
 */

#include "config.h"

#include "compat.h" /* _() gettext macro */
#include "dock_layout.h"
#include "hx_panel.h"
#include "hx_panel_frame.h"
#include "hx_split.h"

struct _HxSplit {
    GtkWidget parent_instance;

    /* Leaf state: frame non-NULL, paned NULL, child_a/b NULL. */
    PanelFrame *frame;

    /* Internal-split state: paned non-NULL, child_a/b non-NULL,
     * frame NULL. */
    GtkPaned *paned;
    HxSplit *child_a; /* start of paned */
    HxSplit *child_b; /* end of paned   */
};

G_DEFINE_FINAL_TYPE (HxSplit, hx_split, GTK_TYPE_WIDGET)

/* ----------------------------------------------------------------- */
/* GObject / GtkWidget glue                                          */
/* ----------------------------------------------------------------- */

static void
hx_split_dispose (GObject *object)
{
    HxSplit *self = HX_SPLIT (object);
    GtkWidget *child;

    /* GtkWidget owns its children via the widget tree, but we have
     * to unparent the single visible child during dispose so it
     * can be finalized cleanly. The leaf PanelFrame or the
     * GtkPaned is the only one; child_a / child_b ride inside the
     * paned's normal child slots. */
    while ((child = gtk_widget_get_first_child (GTK_WIDGET (self))) != NULL) {
        gtk_widget_unparent (child);
    }

    self->frame = NULL;
    self->paned = NULL;
    self->child_a = NULL;
    self->child_b = NULL;

    G_OBJECT_CLASS (hx_split_parent_class)->dispose (object);
}

static void
hx_split_class_init (HxSplitClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

    object_class->dispose = hx_split_dispose;

    /* BinLayout forwards size requests to our single visible child
     * and allocates the full content area to it. That gives the
     * leaf PanelFrame / internal GtkPaned a natural full-bleed
     * layout without any custom measure / size-allocate code. */
    gtk_widget_class_set_layout_manager_type (widget_class,
                                              GTK_TYPE_BIN_LAYOUT);
}

static void
hx_split_init (HxSplit *self)
{
}

/* ----------------------------------------------------------------- */
/* Save-trigger helpers                                              */
/* ----------------------------------------------------------------- */

static void
on_dock_change_notify (GObject *obj, GParamSpec *pspec, gpointer data)
{
    (void)obj;
    (void)pspec;
    (void)data;
    dock_layout_request_save ();
}

/* Track paned-level mutations: notify::position fires when the
 * user drags the divider. Called from hx_split_new_internal so
 * every paned in the tree gets the handler.
 *
 * Note for the next person: PanelFrame does NOT expose
 * page-added / page-removed (only page-closed and adopt-widget
 * — see panel-frame.c in libpanel). The AdwTabView nested inside
 * has page-attached / page-detached but it's a private template
 * child. Rather than walk descendants for the tab view, the
 * places that actually move panels between frames
 * (hx_panel.c on_frame_drop, hx_panel_do_move_in_direction,
 *  on_undocked_close_request, on_frame_close's migration loop
 *  in this file) call dock_layout_request_save explicitly. */
static void
install_paned_save_triggers (GtkPaned *paned)
{
    g_signal_connect (paned, "notify::position",
                      G_CALLBACK (on_dock_change_notify), NULL);
}

/* ----------------------------------------------------------------- */
/* Construction                                                      */
/* ----------------------------------------------------------------- */

HxSplit *
hx_split_new (void)
{
    PanelFrame *frame = hx_panel_frame_new ();
    panel_frame_set_header (frame,
                            PANEL_FRAME_HEADER (panel_frame_header_bar_new ()));
    return hx_split_new_with_frame (frame);
}

HxSplit *
hx_split_new_with_frame (PanelFrame *frame)
{
    HxSplit *self;

    g_return_val_if_fail (PANEL_IS_FRAME (frame), NULL);
    g_return_val_if_fail (gtk_widget_get_parent (GTK_WIDGET (frame)) == NULL,
                          NULL);

    self = g_object_new (HX_TYPE_SPLIT, NULL);
    self->frame = frame;
    gtk_widget_set_parent (GTK_WIDGET (frame), GTK_WIDGET (self));

    return self;
}

HxSplit *
hx_split_new_internal (HxSplit *child_a, HxSplit *child_b,
                       GtkOrientation orientation)
{
    HxSplit *self;
    GtkWidget *paned;

    g_return_val_if_fail (HX_IS_SPLIT (child_a), NULL);
    g_return_val_if_fail (HX_IS_SPLIT (child_b), NULL);
    g_return_val_if_fail (gtk_widget_get_parent (GTK_WIDGET (child_a)) == NULL,
                          NULL);
    g_return_val_if_fail (gtk_widget_get_parent (GTK_WIDGET (child_b)) == NULL,
                          NULL);

    self = g_object_new (HX_TYPE_SPLIT, NULL);

    paned = gtk_paned_new (orientation);
    gtk_paned_set_resize_start_child (GTK_PANED (paned), TRUE);
    gtk_paned_set_resize_end_child (GTK_PANED (paned), TRUE);
    gtk_paned_set_shrink_start_child (GTK_PANED (paned), FALSE);
    gtk_paned_set_shrink_end_child (GTK_PANED (paned), FALSE);

    gtk_paned_set_start_child (GTK_PANED (paned), GTK_WIDGET (child_a));
    gtk_paned_set_end_child (GTK_PANED (paned), GTK_WIDGET (child_b));

    self->paned = GTK_PANED (paned);
    self->child_a = child_a;
    self->child_b = child_b;

    gtk_widget_set_parent (paned, GTK_WIDGET (self));
    install_paned_save_triggers (GTK_PANED (paned));
    return self;
}

/* ----------------------------------------------------------------- */
/* Accessors                                                         */
/* ----------------------------------------------------------------- */

gboolean
hx_split_is_leaf (HxSplit *self)
{
    g_return_val_if_fail (HX_IS_SPLIT (self), FALSE);
    return self->frame != NULL;
}

PanelFrame *
hx_split_get_frame (HxSplit *self)
{
    g_return_val_if_fail (HX_IS_SPLIT (self), NULL);
    return self->frame;
}

HxSplit *
hx_split_get_child_a (HxSplit *self)
{
    g_return_val_if_fail (HX_IS_SPLIT (self), NULL);
    return self->child_a;
}

HxSplit *
hx_split_get_child_b (HxSplit *self)
{
    g_return_val_if_fail (HX_IS_SPLIT (self), NULL);
    return self->child_b;
}

GtkOrientation
hx_split_get_orientation (HxSplit *self)
{
    g_return_val_if_fail (HX_IS_SPLIT (self), GTK_ORIENTATION_HORIZONTAL);
    if (self->paned == NULL) {
        return GTK_ORIENTATION_HORIZONTAL; /* arbitrary; leaf */
    }
    return gtk_orientable_get_orientation (GTK_ORIENTABLE (self->paned));
}

GtkPaned *
hx_split_get_paned (HxSplit *self)
{
    g_return_val_if_fail (HX_IS_SPLIT (self), NULL);
    return self->paned;
}

/* ----------------------------------------------------------------- */
/* Split — leaf → internal                                           */
/* ----------------------------------------------------------------- */

HxSplit *
hx_split_split (HxSplit *self, GtkOrientation orientation)
{
    PanelFrame *original_frame;
    HxSplit *sibling;
    GtkWidget *paned;

    g_return_val_if_fail (HX_IS_SPLIT (self), NULL);
    if (!hx_split_is_leaf (self)) {
        g_warning ("hx_split_split: called on internal split");
        return NULL;
    }

    /* Steal a strong ref on the existing frame so the widget tree
     * shuffle below doesn't dispose it. */
    original_frame = self->frame;
    g_object_ref (original_frame);

    /* Detach the leaf frame from self. */
    gtk_widget_unparent (GTK_WIDGET (original_frame));
    self->frame = NULL;

    /* Build the paned + two child splits. child_a wraps the
     * original frame so its contents (panels) move with it. */
    paned = gtk_paned_new (orientation);
    gtk_paned_set_resize_start_child (GTK_PANED (paned), TRUE);
    gtk_paned_set_resize_end_child (GTK_PANED (paned), TRUE);
    gtk_paned_set_shrink_start_child (GTK_PANED (paned), FALSE);
    gtk_paned_set_shrink_end_child (GTK_PANED (paned), FALSE);

    self->child_a = hx_split_new_with_frame (original_frame);
    g_object_unref (original_frame); /* the new leaf parented it */

    self->child_b = hx_split_new ();

    gtk_paned_set_start_child (GTK_PANED (paned), GTK_WIDGET (self->child_a));
    gtk_paned_set_end_child (GTK_PANED (paned), GTK_WIDGET (self->child_b));

    self->paned = GTK_PANED (paned);
    gtk_widget_set_parent (paned, GTK_WIDGET (self));
    install_paned_save_triggers (GTK_PANED (paned));

    sibling = self->child_b;
    dock_layout_request_save ();
    return sibling;
}

/* ----------------------------------------------------------------- */
/* close_leaf — promote sibling up                                   */
/* ----------------------------------------------------------------- */

gboolean
hx_split_close_leaf (HxSplit *self)
{
    HxSplit *parent;
    HxSplit *sibling;
    gboolean result = FALSE;

    g_return_val_if_fail (HX_IS_SPLIT (self), FALSE);

    /* Keep `self' alive across the widget-tree shuffle below.
     * gtk_paned_set_*_child (parent->paned, NULL) unparents self
     * from the paned, which can drop the last ref and finalize
     * self mid-function — running a method on a freed object is
     * UAF. Hold a strong ref for the whole function; release in
     * the single exit at the bottom. */
    g_object_ref (self);

    if (!hx_split_is_leaf (self)) {
        g_warning ("hx_split_close_leaf: called on internal split");
        goto out;
    }

    /* Pages still present — caller must move panels out first. */
    if (panel_frame_get_n_pages (self->frame) > 0) {
        g_warning ("hx_split_close_leaf: leaf has %u pages; refusing",
                   panel_frame_get_n_pages (self->frame));
        goto out;
    }

    /* The parent split is the HxSplit grandparent of self. The
     * widget chain is: self → GtkPaned (the parent split's
     * `paned' field) → HxSplit (the parent split). */
    {
        GtkWidget *paned_widget = gtk_widget_get_parent (GTK_WIDGET (self));
        GtkWidget *parent_widget;
        if (paned_widget == NULL) {
            goto out; /* detached — shouldn't happen, but bail */
        }
        parent_widget = gtk_widget_get_parent (paned_widget);
        if (parent_widget == NULL || !HX_IS_SPLIT (parent_widget)) {
            goto out; /* area root — close-on-root is a no-op */
        }
        parent = HX_SPLIT (parent_widget);
    }

    sibling = (parent->child_a == self) ? parent->child_b : parent->child_a;
    /* Structural invariant: a HxSplit parent always has two distinct
     * children, so the sibling lookup above can only fail if the split
     * tree is corrupted (NULL or self-aliasing). Use g_critical + bail
     * (via the existing `out:` cleanup) rather than g_assert, which
     * downstream packagers can compile out via G_DISABLE_ASSERT. */
    if (sibling == NULL || sibling == self) {
        g_critical ("hx_split_close_leaf: corrupt parent split (sibling=%p, "
                    "self=%p) — refusing to close",
                    (void *)sibling, (void *)self);
        goto out;
    }

    /* Detach the sibling from the dying paned, hold a ref, drop
     * the paned, reparent the sibling into the parent split. */
    g_object_ref (sibling);
    gtk_paned_set_start_child (parent->paned, NULL);
    gtk_paned_set_end_child (parent->paned, NULL);

    /* Now the parent's HxSplit holds a GtkPaned child that's about
     * to be unparented + dropped. */
    gtk_widget_unparent (GTK_WIDGET (parent->paned));

    /* Promote the sibling's contents to the parent's slot. If the
     * sibling is a leaf, copy its frame; if it's an internal split,
     * copy its paned + child_a/b. The sibling HxSplit itself is
     * then disposable. */
    if (hx_split_is_leaf (sibling)) {
        PanelFrame *frame = sibling->frame;
        g_object_ref (frame);
        gtk_widget_unparent (GTK_WIDGET (frame));
        sibling->frame = NULL;

        parent->frame = frame;
        parent->paned = NULL;
        parent->child_a = NULL;
        parent->child_b = NULL;
        gtk_widget_set_parent (GTK_WIDGET (frame), GTK_WIDGET (parent));
        g_object_unref (frame);
    } else {
        GtkPaned *p = sibling->paned;
        HxSplit *a = sibling->child_a;
        HxSplit *b = sibling->child_b;
        g_object_ref (p);
        gtk_widget_unparent (GTK_WIDGET (p));
        sibling->paned = NULL;
        sibling->child_a = NULL;
        sibling->child_b = NULL;

        parent->frame = NULL;
        parent->paned = p;
        parent->child_a = a;
        parent->child_b = b;
        gtk_widget_set_parent (GTK_WIDGET (p), GTK_WIDGET (parent));
        g_object_unref (p);
    }

    g_object_unref (sibling);

    /* `self' was unparented during the paned drop above. Our
     * explicit ref taken at the top of the function is the only
     * thing still keeping it alive at this point; it'll finalize
     * on the matching g_object_unref below. */
    result = TRUE;

out:
    g_object_unref (self);
    if (result) {
        dock_layout_request_save ();
    }
    return result;
}

/* ----------------------------------------------------------------- */
/* Neighbor — find adjacent leaf in a direction                      */
/* ----------------------------------------------------------------- */

/* Walk DOWN from the given subtree, picking the edge nearest the
 * requested side. e.g. for "leaf to the LEFT of me" we want the
 * rightmost descendant of this sub-tree: for horizontal paneds
 * that's child_b (end), for vertical paneds either child works
 * (both are equally horizontally-rightmost) — we pick child_a for
 * determinism. Symmetric for the other three directions. */
static HxSplit *
descend_to_edge (HxSplit *node, GtkDirectionType dir)
{
    while (!hx_split_is_leaf (node)) {
        GtkOrientation o = hx_split_get_orientation (node);
        gboolean horiz = (o == GTK_ORIENTATION_HORIZONTAL);
        switch (dir) {
        case GTK_DIR_LEFT:
            /* want rightmost: horizontal → end child, vertical → either */
            node = horiz ? node->child_b : node->child_a;
            break;
        case GTK_DIR_RIGHT:
            /* want leftmost: horizontal → start child, vertical →
             * either (pick child_a for determinism). */
            node = node->child_a;
            break;
        case GTK_DIR_UP:
            /* want bottommost: horizontal → either, vertical → end child */
            node = horiz ? node->child_a : node->child_b;
            break;
        case GTK_DIR_DOWN:
            /* want topmost: horizontal → either (pick child_a for
             * determinism), vertical → start child. */
            node = node->child_a;
            break;
        case GTK_DIR_TAB_FORWARD:
        case GTK_DIR_TAB_BACKWARD:
        default:
            return NULL;
        }
    }
    return node;
}

HxSplit *
hx_split_neighbor (HxSplit *leaf, GtkDirectionType direction)
{
    HxSplit *child;
    HxSplit *parent;

    g_return_val_if_fail (HX_IS_SPLIT (leaf), NULL);
    g_return_val_if_fail (hx_split_is_leaf (leaf), NULL);

    /* Walk up looking for the first ancestor split whose
     * orientation matches the direction AND where we entered from
     * the side that allows further travel. */
    child = leaf;
    parent = NULL;
    {
        GtkWidget *p = gtk_widget_get_parent (GTK_WIDGET (leaf));
        if (p != NULL) {
            p = gtk_widget_get_parent (p); /* skip the GtkPaned */
        }
        if (p != NULL && HX_IS_SPLIT (p)) {
            parent = HX_SPLIT (p);
        }
    }

    while (parent != NULL) {
        GtkOrientation o = hx_split_get_orientation (parent);
        gboolean horiz = (o == GTK_ORIENTATION_HORIZONTAL);
        gboolean on_a = (parent->child_a == child);
        HxSplit *other = on_a ? parent->child_b : parent->child_a;

        gboolean direction_matches = FALSE;
        gboolean travelling_to_other = FALSE;

        switch (direction) {
        case GTK_DIR_LEFT:
            direction_matches = horiz;
            travelling_to_other = !on_a; /* we're on B; A is to our left */
            break;
        case GTK_DIR_RIGHT:
            direction_matches = horiz;
            travelling_to_other = on_a; /* we're on A; B is to our right */
            break;
        case GTK_DIR_UP:
            direction_matches = !horiz;
            travelling_to_other = !on_a; /* we're on B (lower); A is up */
            break;
        case GTK_DIR_DOWN:
            direction_matches = !horiz;
            travelling_to_other = on_a; /* we're on A (upper); B is down */
            break;
        default:
            return NULL;
        }

        if (direction_matches && travelling_to_other) {
            return descend_to_edge (other, direction);
        }

        /* Otherwise keep climbing. */
        child = parent;
        {
            GtkWidget *p = gtk_widget_get_parent (GTK_WIDGET (parent));
            if (p != NULL) {
                p = gtk_widget_get_parent (p); /* skip the GtkPaned */
            }
            parent = (p != NULL && HX_IS_SPLIT (p)) ? HX_SPLIT (p) : NULL;
        }
    }

    return NULL;
}

/* ----------------------------------------------------------------- */
/* Tree-wide helpers                                                 */
/* ----------------------------------------------------------------- */

HxSplit *
hx_split_find_for_frame (HxSplit *root, PanelFrame *frame)
{
    g_return_val_if_fail (HX_IS_SPLIT (root), NULL);
    g_return_val_if_fail (PANEL_IS_FRAME (frame), NULL);

    if (hx_split_is_leaf (root)) {
        return (root->frame == frame) ? root : NULL;
    }

    {
        HxSplit *hit = hx_split_find_for_frame (root->child_a, frame);
        if (hit != NULL) {
            return hit;
        }
    }
    return hx_split_find_for_frame (root->child_b, frame);
}

void
hx_split_foreach_leaf (HxSplit *root, HxSplitLeafFunc func, gpointer user_data)
{
    g_return_if_fail (HX_IS_SPLIT (root));
    g_return_if_fail (func != NULL);

    if (hx_split_is_leaf (root)) {
        func (root, user_data);
        return;
    }
    hx_split_foreach_leaf (root->child_a, func, user_data);
    hx_split_foreach_leaf (root->child_b, func, user_data);
}

/* ----------------------------------------------------------------- */
/* Per-frame split UI (menu button + GActions)                       */
/* ----------------------------------------------------------------- */

/* Find the HxSplit leaf that wraps the given PanelFrame. The frame
 * is parented directly to a leaf HxSplit by hx_split_new_with_frame,
 * so walking up one widget step gets us there. NULL when the frame
 * isn't in a split tree (shouldn't happen for any frame the
 * toolbar built). */
static HxSplit *
frame_to_leaf (GtkWidget *frame)
{
    GtkWidget *parent = gtk_widget_get_parent (frame);
    return (parent != NULL && HX_IS_SPLIT (parent)) ? HX_SPLIT (parent) : NULL;
}

/* Find the parent split (the internal-split HxSplit whose paned
 * holds this leaf as one of its two children). NULL when the leaf
 * is an area root. */
static HxSplit *
leaf_parent_split (HxSplit *leaf)
{
    GtkWidget *paned_anc;
    GtkWidget *p;

    if (leaf == NULL) {
        return NULL;
    }
    paned_anc = gtk_widget_get_parent (GTK_WIDGET (leaf));
    if (paned_anc == NULL) {
        return NULL;
    }
    p = gtk_widget_get_parent (paned_anc);
    return (p != NULL && HX_IS_SPLIT (p)) ? HX_SPLIT (p) : NULL;
}

/* Forward decl — installed via toolbar.h. We don't want to
 * include toolbar.h here (it pulls session.h indirectly) so
 * declare the one symbol we need. */
extern void toolbar_install_panel_hooks_on_frame (GtkWidget *frame);

/* The four default-leaf PanelFrame globals defined in toolbar.c.
 * Static-panel factories (users.c, news.c, etc.) use these as
 * their panel_frame_add target. When a user closes a default
 * leaf via the per-frame menu, on_frame_close reseats the
 * relevant pointer onto the surviving sibling's frame BEFORE
 * the close — otherwise the globals dangle and the next
 * static-panel re-attach (toolbar button click on a closed
 * panel) crashes. */
extern GtkWidget *toolbar_sidebar_frame;
extern GtkWidget *toolbar_end_frame;
extern GtkWidget *toolbar_bottom_frame;
extern GtkWidget *toolbar_center_frame;

/* Forward decl — used by frame_do_split, defined further down. */
static void refresh_close_enabled_leaf (HxSplit *leaf, gpointer user_data);

static void
frame_do_split (GtkWidget *frame, GtkOrientation orientation)
{
    HxSplit *leaf = frame_to_leaf (frame);
    HxSplit *new_leaf;
    PanelFrame *new_frame;

    if (leaf == NULL) {
        return;
    }

    new_leaf = hx_split_split (leaf, orientation);
    if (new_leaf == NULL) {
        return;
    }

    new_frame = hx_split_get_frame (new_leaf);
    if (new_frame == NULL) {
        return;
    }

    /* The new sibling leaf's PanelFrame needs the same plumbing
     * every other leaf carries (close-dispatcher / drag-out /
     * defang drop-controls) AND its own split UI. The helper
     * covers all four hooks (close-dispatcher, drag-out,
     * defang, frame-ui) — calling install_frame_ui separately
     * would duplicate the menu suffix button. */
    toolbar_install_panel_hooks_on_frame (GTK_WIDGET (new_frame));

    /* The parent split's other leaf was previously an area root
     * — close-frame was greyed because no parent split existed.
     * Now that the split DOES exist, its enabled state needs to
     * flip true. Walk every leaf in the area's tree and refresh. */
    {
        HxSplit *area_root = leaf;
        while (TRUE) {
            GtkWidget *p = gtk_widget_get_parent (GTK_WIDGET (area_root));
            if (p == NULL) {
                break;
            }
            p = gtk_widget_get_parent (p);
            if (p == NULL || !HX_IS_SPLIT (p)) {
                break;
            }
            area_root = HX_SPLIT (p);
        }
        hx_split_foreach_leaf (area_root, refresh_close_enabled_leaf, NULL);
    }
}

static void
on_frame_split_h (GSimpleAction *action, GVariant *parameter,
                  gpointer user_data)
{
    (void)action;
    (void)parameter;
    frame_do_split (GTK_WIDGET (user_data), GTK_ORIENTATION_HORIZONTAL);
}

static void
on_frame_split_v (GSimpleAction *action, GVariant *parameter,
                  gpointer user_data)
{
    (void)action;
    (void)parameter;
    frame_do_split (GTK_WIDGET (user_data), GTK_ORIENTATION_VERTICAL);
}

static void
on_frame_close (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    GtkWidget *frame = GTK_WIDGET (user_data);
    HxSplit *leaf = frame_to_leaf (frame);
    HxSplit *parent;
    HxSplit *sibling_leaf;
    PanelFrame *sibling_frame;
    PanelFrame *current_frame;

    (void)action;
    (void)parameter;

    if (leaf == NULL) {
        return;
    }

    parent = leaf_parent_split (leaf);
    if (parent == NULL) {
        return; /* area root — close-on-root is refused */
    }

    /* Pick the sibling and walk down to the leaf nearest the
     * split boundary — i.e. the side of the sibling subtree
     * that's adjacent to where the closing leaf used to be.
     * Going always via child_a (the previous code) gave the
     * wrong edge when the closing leaf was child_b and the
     * sibling subtree was itself split: pages migrated to a
     * far corner of the sibling instead of the leaf right next
     * to where they came from.
     *
     * descend_to_edge takes the direction of MOTION from the
     * closing leaf into the sibling subtree, which is the
     * direction-toward-sibling along the parent's orientation
     * axis. */
    {
        gboolean horiz
            = (hx_split_get_orientation (parent) == GTK_ORIENTATION_HORIZONTAL);
        gboolean closing_is_a = (hx_split_get_child_a (parent) == leaf);
        HxSplit *sib = closing_is_a ? hx_split_get_child_b (parent)
                                    : hx_split_get_child_a (parent);
        GtkDirectionType dir;
        if (horiz) {
            dir = closing_is_a ? GTK_DIR_RIGHT : GTK_DIR_LEFT;
        } else {
            dir = closing_is_a ? GTK_DIR_DOWN : GTK_DIR_UP;
        }

        sibling_leaf = descend_to_edge (sib, dir);
        if (sibling_leaf == NULL) {
            return;
        }
        sibling_frame = hx_split_get_frame (sibling_leaf);
        if (sibling_frame == NULL) {
            return;
        }
    }

    current_frame = PANEL_FRAME (frame);

    /* Move every page out of the dying frame into the sibling
     * before collapsing the leaf. Iterate from the tail —
     * panel_frame_remove compacts indices.
     *
     * Reseat each panel's home_frame onto sibling_frame at the
     * same time. home_frame is a GWeakRef; if we leave it pointing
     * at `current_frame`, the impending hx_split_close_leaf
     * destroys that frame and the weak ref expires. The next
     * Close-all-pages / re-show round trip (via
     * hx_panel_ensure_attached) would then fall back to home_area
     * rather than the sibling frame where the panel actually lives
     * now — losing the user's placement, exactly the regression
     * that the home_frame mechanism exists to prevent. And undock
     * + close-undocked-window (on_undocked_close_request) would
     * fail to find a frame to redock into. */
    {
        guint n = panel_frame_get_n_pages (current_frame);
        for (guint i = n; i > 0; i--) {
            PanelWidget *p = panel_frame_get_page (current_frame, i - 1);
            if (p == NULL) {
                continue;
            }
            g_object_ref (p);
            panel_frame_remove (current_frame, p);
            panel_frame_add (sibling_frame, p);
            if (HX_IS_PANEL (p)) {
                hx_panel_set_home_frame (HX_PANEL (p),
                                         GTK_WIDGET (sibling_frame));
            }
            g_object_unref (p);
        }
    }

    /* Static-panel factories use toolbar_*_frame as their
     * panel_frame_add target. If `frame' is one of the four
     * default-leaf frames the closing leaf is about to destroy,
     * those globals would dangle after hx_split_close_leaf
     * returns. Reseat the relevant global on the surviving
     * sibling frame BEFORE the close. */
    if (frame == toolbar_sidebar_frame) {
        toolbar_sidebar_frame = GTK_WIDGET (sibling_frame);
    } else if (frame == toolbar_end_frame) {
        toolbar_end_frame = GTK_WIDGET (sibling_frame);
    } else if (frame == toolbar_bottom_frame) {
        toolbar_bottom_frame = GTK_WIDGET (sibling_frame);
    } else if (frame == toolbar_center_frame) {
        toolbar_center_frame = GTK_WIDGET (sibling_frame);
    }

    hx_split_close_leaf (leaf);

    /* The collapse may have promoted `parent' to be the new area
     * root (e.g. closing one leaf of a two-leaf area's only
     * internal split). The surviving leaves' close-frame
     * enabled state then needs to refresh, exactly the same way
     * frame_do_split refreshes after a split changes the
     * topology — otherwise the area-root leaf's close-frame can
     * stay enabled even though closing it is now a no-op. */
    {
        HxSplit *area_root = parent;
        while (TRUE) {
            GtkWidget *p = gtk_widget_get_parent (GTK_WIDGET (area_root));
            if (p == NULL) {
                break;
            }
            p = gtk_widget_get_parent (p);
            if (p == NULL || !HX_IS_SPLIT (p)) {
                break;
            }
            area_root = HX_SPLIT (p);
        }
        hx_split_foreach_leaf (area_root, refresh_close_enabled_leaf, NULL);
    }
}

/* Update enabled state of the three GActions based on the current
 * tree shape. Called once at install and again after every split
 * (the parent shape changes), so that close-frame is greyed only
 * on the area root and split items are always enabled. */
static void
update_frame_action_enabled (GtkWidget *frame)
{
    HxSplit *leaf = frame_to_leaf (frame);
    gboolean can_close = (leaf != NULL && leaf_parent_split (leaf) != NULL);

    gtk_widget_action_set_enabled (frame, "frame-ops.close-frame", can_close);
    gtk_widget_action_set_enabled (frame, "frame-ops.split-h", TRUE);
    gtk_widget_action_set_enabled (frame, "frame-ops.split-v", TRUE);
}

/* hx_split_foreach_leaf callback — runs update_frame_action_enabled
 * on each leaf's frame so that after a split, every leaf in the
 * area's tree (most importantly, the leaf that JUST became a
 * non-root via the split) has the right close-frame enabled
 * state. */
static void
refresh_close_enabled_leaf (HxSplit *leaf, gpointer user_data)
{
    PanelFrame *frame;
    (void)user_data;
    frame = hx_split_get_frame (leaf);
    if (frame != NULL) {
        update_frame_action_enabled (GTK_WIDGET (frame));
    }
}

void
hx_split_install_frame_ui (GtkWidget *frame)
{
    GSimpleActionGroup *group;
    GMenu *menu;
    GtkWidget *button;
    PanelFrameHeader *header;

    g_return_if_fail (PANEL_IS_FRAME (frame));

    /* Action group at prefix "frame-ops" on the frame widget
     * itself. Standard gtk_widget_insert_action_group — no
     * PanelActionMuxer indirection. Menu items reference
     * "frame-ops.split-h" etc directly. */
    group = g_simple_action_group_new ();
    {
        GSimpleAction *act;

        act = g_simple_action_new ("split-h", NULL);
        g_signal_connect_object (act, "activate", G_CALLBACK (on_frame_split_h),
                                 frame, G_CONNECT_DEFAULT);
        g_action_map_add_action (G_ACTION_MAP (group), G_ACTION (act));
        g_object_unref (act);

        act = g_simple_action_new ("split-v", NULL);
        g_signal_connect_object (act, "activate", G_CALLBACK (on_frame_split_v),
                                 frame, G_CONNECT_DEFAULT);
        g_action_map_add_action (G_ACTION_MAP (group), G_ACTION (act));
        g_object_unref (act);

        act = g_simple_action_new ("close-frame", NULL);
        g_signal_connect_object (act, "activate", G_CALLBACK (on_frame_close),
                                 frame, G_CONNECT_DEFAULT);
        g_action_map_add_action (G_ACTION_MAP (group), G_ACTION (act));
        g_object_unref (act);
    }
    gtk_widget_insert_action_group (frame, "frame-ops", G_ACTION_GROUP (group));
    g_object_unref (group);

    update_frame_action_enabled (frame);

    /* Build the menu model and a small GtkMenuButton, then add as
     * a suffix on the frame's header. priority=0 puts it at the
     * start of the suffix area, before the existing controls. */
    menu = g_menu_new ();
    g_menu_append (menu, _ ("Split horizontally"), "frame-ops.split-h");
    g_menu_append (menu, _ ("Split vertically"), "frame-ops.split-v");
    g_menu_append (menu, _ ("Close frame"), "frame-ops.close-frame");

    button = gtk_menu_button_new ();
    gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (button),
                                   "view-split-symbolic");
    gtk_widget_set_tooltip_text (button, _ ("Frame options"));
    gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (button),
                                    G_MENU_MODEL (menu));
    g_object_unref (menu);

    header = panel_frame_get_header (PANEL_FRAME (frame));
    if (header != NULL) {
        panel_frame_header_add_suffix (header, 0, button);
    }
}
