/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * hx_panel_frame.c — PanelFrame subclass that hijacks the
 * libpanel chevron's page.move-* items. See hx_panel_frame.h.
 */

#include "config.h"

#include "hx_panel_frame.h"
#include "dock_layout.h"
#include "hx_panel.h"
#include "debug.h"

#include <adwaita.h>

struct _HxPanelFrame {
    PanelFrame parent_instance;
};

G_DEFINE_FINAL_TYPE (HxPanelFrame, hx_panel_frame, PANEL_TYPE_FRAME)

/* ----------------------------------------------------------------- */
/* Class action handlers                                             */
/* ----------------------------------------------------------------- */

static void
do_move (GtkWidget *widget, GtkDirectionType dir)
{
    PanelWidget *visible;

    visible = panel_frame_get_visible_child (PANEL_FRAME (widget));
    if (visible == NULL || !HX_IS_PANEL (visible)) {
        return;
    }
    hx_panel_do_move_in_direction (HX_PANEL (visible), dir);
}

static void
move_left_action (GtkWidget *widget, const char *name, GVariant *param)
{
    (void)name;
    (void)param;
    do_move (widget, GTK_DIR_LEFT);
}

static void
move_right_action (GtkWidget *widget, const char *name, GVariant *param)
{
    (void)name;
    (void)param;
    do_move (widget, GTK_DIR_RIGHT);
}

static void
move_up_action (GtkWidget *widget, const char *name, GVariant *param)
{
    (void)name;
    (void)param;
    do_move (widget, GTK_DIR_UP);
}

static void
move_down_action (GtkWidget *widget, const char *name, GVariant *param)
{
    (void)name;
    (void)param;
    do_move (widget, GTK_DIR_DOWN);
}

/* ----------------------------------------------------------------- */
/* Enabled-state refresh                                             */
/* ----------------------------------------------------------------- */

/* Recompute per-direction enabled state for the four page.move-*
 * actions and push it through gtk_widget_action_set_enabled.
 *
 * libpanel's panel_frame_update_actions calls
 *   gtk_widget_action_set_enabled (frame, "page.move-*", FALSE)
 * on every selected-page change / root / unroot because the frame
 * has no PanelGrid ancestor (we use HxSplit). Those calls walk
 * priv->actions head-first and match by strcmp, so they land on
 * OUR class actions (head of the list in this subclass) and flip
 * the disable bit. This refresh is called from the same hook
 * points where libpanel does its update (root, unroot,
 * notify::visible-child) and from the AdwTabView setup-menu hook
 * just before the chevron popover observer registers — that's the
 * one that actually drives the menu rendering.
 *
 * Per-direction enable: hx_panel_can_move_in_direction returns
 * TRUE iff there's a neighbour leaf in that direction in the dock
 * tree. No visible HxPanel → all four greyed (no panel to move).
 * Panel present but no neighbour in a direction → that direction
 * specifically greyed (a no-op move stays inert in the menu).
 *
 * The header's close button has the same "libpanel disables what we
 * enable" problem but could NOT be fixed this way — see the note on
 * frame-ops.close-page in hx_split.c for why a menu item can be
 * rescued here and a button can't. */
static void
refresh_move_enabled (HxPanelFrame *self)
{
    GtkWidget *w = GTK_WIDGET (self);
    PanelWidget *visible = panel_frame_get_visible_child (PANEL_FRAME (w));
    HxPanel *panel = (visible != NULL && HX_IS_PANEL (visible))
                         ? HX_PANEL (visible)
                         : NULL;
    gboolean left
        = panel && hx_panel_can_move_in_direction (panel, GTK_DIR_LEFT);
    gboolean right
        = panel && hx_panel_can_move_in_direction (panel, GTK_DIR_RIGHT);
    gboolean up = panel && hx_panel_can_move_in_direction (panel, GTK_DIR_UP);
    gboolean down
        = panel && hx_panel_can_move_in_direction (panel, GTK_DIR_DOWN);

    debug_log ("dock", "HxPanelFrame[%p]: refresh L=%d R=%d U=%d D=%d", w, left,
               right, up, down);

    gtk_widget_action_set_enabled (w, "page.move-left", left);
    gtk_widget_action_set_enabled (w, "page.move-right", right);
    gtk_widget_action_set_enabled (w, "page.move-up", up);
    gtk_widget_action_set_enabled (w, "page.move-down", down);
}

static void
on_visible_child_notify (HxPanelFrame *self, GParamSpec *pspec,
                         gpointer user_data)
{
    (void)pspec;
    (void)user_data;
    /* connect_after: libpanel's notify::visible-child handler
     * (which calls panel_frame_update_actions) has already run. */
    refresh_move_enabled (self);

    /* Which page a frame shows is part of the saved layout (the '*'
     * marker in the tree expression), so a tab switch is a layout
     * change. The 200 ms debounce absorbs the burst of these that
     * startup produces as panels are added one by one; the write
     * that lands is the settled state. */
    dock_layout_request_save ();
}

/* AdwTabView's setup-menu signal fires immediately before the
 * chevron popover menu is shown — that's when libpanel rewrites
 * the joined menu model and when the popover's observer registers
 * with the action muxer (action lookup happens at this moment).
 *
 * Connecting AFTER libpanel's panel_frame_setup_menu_cb lets us
 * push fresh per-direction state right before the observer
 * queries. Without this, the bit can still be stale from
 * libpanel's last update_actions even though we re-set state at
 * earlier hook points — the observer caches state at registration
 * time and only flips when a fresh action-enabled-changed signal
 * arrives. */
static void
on_setup_menu (AdwTabView *tab_view, AdwTabPage *page, gpointer user_data)
{
    HxPanelFrame *self = HX_PANEL_FRAME (user_data);

    (void)tab_view;
    (void)page;
    refresh_move_enabled (self);
}

/* Recursively walk descendants for ADW_IS_TAB_VIEW. PanelFrame's
 * .ui template wraps the tab_view a couple layers deep
 * (GtkOverlay → GtkBox → GtkStack → AdwTabView), so the search
 * has to descend; gtk_widget_get_first_child + sibling-iter is
 * the standard GTK 4 idiom. */
static AdwTabView *
find_descendant_tab_view (GtkWidget *root)
{
    GtkWidget *child;

    if (ADW_IS_TAB_VIEW (root)) {
        return ADW_TAB_VIEW (root);
    }

    for (child = gtk_widget_get_first_child (root); child != NULL;
         child = gtk_widget_get_next_sibling (child)) {
        AdwTabView *found = find_descendant_tab_view (child);
        if (found != NULL) {
            return found;
        }
    }
    return NULL;
}

/* ----------------------------------------------------------------- */
/* Vfunc overrides                                                   */
/* ----------------------------------------------------------------- */

static void
hx_panel_frame_constructed (GObject *object)
{
    GtkWidget *widget = GTK_WIDGET (object);
    AdwTabView *tab_view;

    G_OBJECT_CLASS (hx_panel_frame_parent_class)->constructed (object);

    /* Parent (PanelFrame) constructed has now built the tab_view
     * from its .ui template and connected its own setup-menu
     * handler. Connect ours after so we run last and our
     * refresh_move_enabled is the most recent action-state change
     * before the popover observer registers. */
    tab_view = find_descendant_tab_view (widget);
    if (tab_view != NULL) {
        g_signal_connect_after (tab_view, "setup-menu",
                                G_CALLBACK (on_setup_menu), widget);
    } else {
        g_warning ("HxPanelFrame: no AdwTabView descendant found at "
                   "constructed time — chevron move-page items will "
                   "not refresh on menu open");
    }
}

static void
hx_panel_frame_root (GtkWidget *widget)
{
    GTK_WIDGET_CLASS (hx_panel_frame_parent_class)->root (widget);
    /* libpanel's panel_frame_root calls update_actions which
     * disables our moves; recompute proper per-direction state. */
    refresh_move_enabled (HX_PANEL_FRAME (widget));
}

static void
hx_panel_frame_unroot (GtkWidget *widget)
{
    GTK_WIDGET_CLASS (hx_panel_frame_parent_class)->unroot (widget);
    refresh_move_enabled (HX_PANEL_FRAME (widget));
}

/* ----------------------------------------------------------------- */
/* Class init                                                        */
/* ----------------------------------------------------------------- */

static void
hx_panel_frame_class_init (HxPanelFrameClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->constructed = hx_panel_frame_constructed;
    widget_class->root = hx_panel_frame_root;
    widget_class->unroot = hx_panel_frame_unroot;

    /* gtk_widget_class_install_action prepends onto the
     * priv->actions list inherited from PanelFrameClass; lookup is
     * head-first by strcmp, so ours beat libpanel's same-named
     * class actions for both activation and enabled-state queries. */
    gtk_widget_class_install_action (widget_class, "page.move-left", NULL,
                                     move_left_action);
    gtk_widget_class_install_action (widget_class, "page.move-right", NULL,
                                     move_right_action);
    gtk_widget_class_install_action (widget_class, "page.move-up", NULL,
                                     move_up_action);
    gtk_widget_class_install_action (widget_class, "page.move-down", NULL,
                                     move_down_action);
}

static void
hx_panel_frame_init (HxPanelFrame *self)
{
    /* Clip content to the frame's allocation. GtkWidget defaults
     * to GTK_OVERFLOW_VISIBLE, which lets children draw past
     * their parent's bounds. Every paned in the dock runs with
     * shrink_start_child=FALSE / shrink_end_child=FALSE (the
     * hx_split_new_internal default — see src/hx_split.c), and
     * toolbar.c floors every leaf at DEFAULT_LEAF_MIN_WIDTH via
     * gtk_widget_set_size_request, so steady-state allocations
     * stay >= the content's minimum.
     *
     * That doesn't cover every transient: paned animations,
     * resize-during-allocation passes, and split / merge / undock
     * operations can briefly hand a frame a smaller slot than its
     * children's natural width, during which children would
     * otherwise render at natural width starting from the frame's
     * left edge and spill onto neighbouring frames or off the
     * window. GTK_OVERFLOW_HIDDEN keeps the visible content
     * bounded to the slot regardless. Cheap defence, applies to
     * every leaf for consistency. */
    gtk_widget_set_overflow (GTK_WIDGET (self), GTK_OVERFLOW_HIDDEN);

    /* notify::visible-child fires when libpanel switches the
     * AdwTabView's selected page. connect_after places our handler
     * past libpanel's own panel_frame_notify_selected_page_cb,
     * which calls panel_frame_update_actions and disables the move
     * actions. */
    g_signal_connect_after (self, "notify::visible-child",
                            G_CALLBACK (on_visible_child_notify), NULL);

    /* Initial state — visible_child is NULL at this point, so all
     * four end up disabled until a panel is added. refresh handles
     * that case correctly. */
    refresh_move_enabled (self);
}

PanelFrame *
hx_panel_frame_new (void)
{
    return g_object_new (HX_TYPE_PANEL_FRAME, NULL);
}
