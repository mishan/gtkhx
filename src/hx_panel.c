/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * hx_panel.c — HxPanel : PanelWidget subclass.
 *
 * See hx_panel.h for the design rationale.  The implementation is
 * deliberately tiny: HxPanel is a marker subclass that carries an
 * id + kind + home-area triple. All actual UI work (title, icon,
 * child widget, menu model) goes through the inherited PanelWidget
 * API so Phase 2 migrations don't have to learn a parallel one.
 */

#include "config.h"

#include "hx_panel.h"
#include "hx_panel_frame.h"
#include "hx_split.h"
#include "dock_layout.h"
#include "panel_registry.h"
#include "debug.h"
#include "compat.h"   /* _() gettext macro for menu labels */
#include "hx.h"       /* session typedef, required by gtkutil.h and toolbar.h */
#include "gtkutil.h"  /* init_keyaccel for undocked windows */
#include "toolbar.h"  /* toolbar_window — main-dock root comparison */

#include <adwaita.h>

struct _HxPanel
{
    PanelWidget  parent_instance;

    char        *id;
    HxPanelKind  kind;
    PanelArea    home_area;
    GWeakRef     home_frame;     /* PanelFrame the Undock action returns to */

    /* DYNAMIC panel close callback. NULL on static
     * panels; for pchat / msg panels the factory installs a
     * tear-down function that runs from the frame dispatcher
     * (page-closed signal) before the registry unregisters the
     * panel. */
    HxPanelCloseFunc  close_func;
    gpointer          close_data;
};

G_DEFINE_FINAL_TYPE (HxPanel, hx_panel, PANEL_TYPE_WIDGET)

/* Forward decls */
static void        on_undock_activate           (GSimpleAction *action,
                                                 GVariant      *parameter,
                                                 gpointer       user_data);
static gboolean    on_undocked_close_request    (GtkWindow     *window,
                                                 gpointer       user_data);
static PanelFrame *hx_panel_undocked_create_frame (PanelGrid   *grid,
                                                   gpointer     user_data);

/* Toolbar globals exposed by toolbar.c — the home frames for each
 * of the four sidebar areas. NULL until create_toolbar_window has
 * run, but Phase 2 panels are constructed inside its tail so the
 * pointers are live by the time any move action fires. */
extern GtkWidget *toolbar_sidebar_frame;  /* PANEL_AREA_START  */
extern GtkWidget *toolbar_end_frame;      /* PANEL_AREA_END    */
extern GtkWidget *toolbar_bottom_frame;   /* PANEL_AREA_BOTTOM */
extern GtkWidget *toolbar_center_frame;   /* PANEL_AREA_CENTER */
extern GtkWidget *toolbar_dock;

/* Map a default-leaf frame back to its PanelArea. Returns TRUE and
 * writes *out when `frame' is one of the four toolbar_*_frame
 * defaults; returns FALSE and leaves *out untouched for any other
 * frame (user-created split leaves, undocked-window frames, NULL).
 *
 * Used by in-dock relocation paths (DnD and move-direction) to
 * decide whether to update the panel's home_area fallback. Updating
 * home_area only on default-frame targets is intentional: when the
 * user drops a panel into a custom split leaf, home_frame already
 * captures the exact leaf; if that leaf is later closed while the
 * panel is detached, hx_panel_ensure_attached's home_area fallback
 * should return the panel to its ORIGINAL area's default (where a
 * sidebar panel started life, say) rather than to whatever sentinel
 * the user-created leaf would map to. Overwriting home_area on
 * every move would silently coerce sidebar-kind panels to the
 * center default once their custom leaf vanished. */
static gboolean
panel_area_for_default_frame (GtkWidget *frame, PanelArea *out)
{
    if (frame == toolbar_sidebar_frame) { *out = PANEL_AREA_START;  return TRUE; }
    if (frame == toolbar_end_frame)     { *out = PANEL_AREA_END;    return TRUE; }
    if (frame == toolbar_bottom_frame)  { *out = PANEL_AREA_BOTTOM; return TRUE; }
    if (frame == toolbar_center_frame)  { *out = PANEL_AREA_CENTER; return TRUE; }
    return FALSE;
}

static void
hx_panel_finalize (GObject *object)
{
    HxPanel *self = HX_PANEL (object);

    g_clear_pointer (&self->id, g_free);
    g_weak_ref_clear (&self->home_frame);

    G_OBJECT_CLASS (hx_panel_parent_class)->finalize (object);
}

static void
hx_panel_class_init (HxPanelClass *klass)
{
    G_OBJECT_CLASS (klass)->finalize = hx_panel_finalize;
}

static void
hx_panel_init (HxPanel *self)
{
    GSimpleActionGroup *group;
    GSimpleAction      *undock;
    GMenu              *menu;

    /* Sensible defaults: every panel starts assuming it's a center
     * document. SIDEBAR / DYNAMIC panels override via hx_panel_new. */
    self->kind      = HX_PANEL_KIND_CENTER;
    self->home_area = PANEL_AREA_CENTER;
    g_weak_ref_init (&self->home_frame, NULL);

    /* Install panel.undock as a per-instance GAction. libpanel
     * routes per-panel actions through its own PanelActionMuxer,
     * NOT through gtk_widget_insert_action_group's standard muxer
     * — that's the muxer the joined menu in PanelFrameHeaderBar's
     * menu button (the pan-down-symbolic chevron) consults when
     * resolving the "panel.<action>" prefix. Using the GTK API
     * silently installs the action on the wrong muxer; the menu
     * item appears greyed out (or doesn't appear at all in older
     * libpanel) because the action lookup fails. */
    group  = g_simple_action_group_new ();
    undock = g_simple_action_new ("undock", NULL);
    g_signal_connect (undock, "activate",
                      G_CALLBACK (on_undock_activate), self);
    g_action_map_add_action (G_ACTION_MAP (group), G_ACTION (undock));
    g_object_unref (undock);

    /* Move-direction actions used to live here as panel.move-*
     * and surfaced via our own chevron-menu section. They migrated
     * to the per-frame "page.move-{left,right,up,down}" inserted
     * action group (hx_panel_install_page_move_actions) so they
     * REPURPOSE libpanel's built-in "Move Page L/R/U/D" items in
     * the chevron's joined menu — those items used to be
     * always-greyed because libpanel's default handlers assume a
     * PanelGrid layout we don't have. Net: one set of Move items
     * in the chevron now, not two, and they actually work. */

    panel_widget_insert_action_group (PANEL_WIDGET (self), "panel",
                                      G_ACTION_GROUP (group));
    g_object_unref (group);

    /* Per-panel chevron menu. The only entry here is Undock; the
     * Move Page items in the chevron come from libpanel's
     * frame_menu template and are rerouted to our cross-frame
     * neighbour move by the per-frame "page" action group install
     * in hx_panel_install_page_move_actions. Split + close-frame
     * live on the small menu button on each PanelFrame's header
     * (hx_split_install_frame_ui). */
    menu = g_menu_new ();
    g_menu_append (menu, _ ("Undock"), "page.panel.undock");
    panel_widget_set_menu_model (PANEL_WIDGET (self), G_MENU_MODEL (menu));
    g_object_unref (menu);
}

HxPanel *
hx_panel_new (const char  *id,
              HxPanelKind  kind,
              PanelArea    home_area)
{
    HxPanel *self;

    g_return_val_if_fail (id != NULL && id[0] != '\0', NULL);

    self = g_object_new (HX_TYPE_PANEL, NULL);
    self->id        = g_strdup (id);
    self->kind      = kind;
    self->home_area = home_area;

    /* The base class already exposes "id" as a property
     * (panel_widget_set_id) — populate it from our copy so the rest
     * of libpanel sees the same string. We keep our own copy too
     * because panel_widget_get_id can return NULL transiently
     * (e.g. during dispose). */
    panel_widget_set_id (PANEL_WIDGET (self), id);

    return self;
}

const char *
hx_panel_get_id (HxPanel *self)
{
    g_return_val_if_fail (HX_IS_PANEL (self), NULL);
    return self->id;
}

HxPanelKind
hx_panel_get_kind (HxPanel *self)
{
    g_return_val_if_fail (HX_IS_PANEL (self), HX_PANEL_KIND_CENTER);
    return self->kind;
}

PanelArea
hx_panel_get_home_area (HxPanel *self)
{
    g_return_val_if_fail (HX_IS_PANEL (self), PANEL_AREA_CENTER);
    return self->home_area;
}

void
hx_panel_set_close_handler (HxPanel          *self,
                            HxPanelCloseFunc  func,
                            gpointer          user_data)
{
    g_return_if_fail (HX_IS_PANEL (self));
    self->close_func = func;
    self->close_data = user_data;
}

/* PanelFrame::page-closed handler. Fires after the page has been
 * detached from the tab view (so the widget no longer has a frame
 * ancestor at this point — the page-closed contract from
 * libpanel's testsuite/test-frame.c). Dispatches to the dynamic-
 * panel teardown path; static panels fall through unchanged. */
static void
on_frame_page_closed (PanelFrame  *frame,
                      PanelWidget *page,
                      gpointer     user_data)
{
    HxPanel *self;
    const char *id;
    HxPanelCloseFunc cb;
    gpointer cb_data;

    (void)frame;
    (void)user_data;

    if (!HX_IS_PANEL (page))
        return;

    self = HX_PANEL (page);
    if (self->kind != HX_PANEL_KIND_DYNAMIC)
        return;

    /* Snapshot the callback fields before the teardown runs — the
     * callback is allowed to set them to NULL (and may even
     * destroy the gchat/msgwin struct that close_data points at). */
    cb      = self->close_func;
    cb_data = self->close_data;
    self->close_func = NULL;
    self->close_data = NULL;

    if (cb)
        cb (self, cb_data);

    /* Take the registry's last strong ref off. The panel widget
     * is already orphaned (page-closed runs post-detach); dropping
     * the registry ref hits refcount 0 → finalize. */
    id = self->id;
    if (id != NULL)
        hx_panel_registry_unregister (id);
}

void
hx_panel_install_close_dispatcher (GtkWidget *frame)
{
    g_return_if_fail (frame == NULL || PANEL_IS_FRAME (frame));
    if (frame == NULL)
        return;

    /* g_signal_connect is fine — frames live for the application's
     * lifetime (the toolbar dock owns them) so there's no
     * disconnect-on-frame-death case to worry about. */
    g_signal_connect (frame, "page-closed",
                      G_CALLBACK (on_frame_page_closed), NULL);
}

/* --- Drag-out detection ----------------------------------------- */

/* Depth-first traversal: find a GtkButton whose icon name is
 * "list-drag-handle-symbolic". libpanel's PanelFrameHeaderBar sets
 * this icon on its drag handle button at template-init time
 * (panel-frame-header-bar.ui); the button is the GtkDragSource
 * carrier for the per-frame drag system. There's no public API to
 * access it, so we hunt by widget property. The icon name has been
 * stable since libpanel 1.0 and the .ui is in the public source. */
static GtkButton *
find_drag_button (GtkWidget *root)
{
    GtkWidget *child;

    if (root == NULL)
        return NULL;

    if (GTK_IS_BUTTON (root)) {
        const char *icon = gtk_button_get_icon_name (GTK_BUTTON (root));
        if (g_strcmp0 (icon, "list-drag-handle-symbolic") == 0)
            return GTK_BUTTON (root);
    }

    for (child = gtk_widget_get_first_child (root);
         child != NULL;
         child = gtk_widget_get_next_sibling (child)) {
        GtkButton *found = find_drag_button (child);
        if (found != NULL)
            return found;
    }
    return NULL;
}

/* Find the GtkDragSource controller installed on a widget. The
 * drag_button installs exactly one (from panel-frame-header-bar.ui),
 * so the first hit is the one we want. */
static GtkDragSource *
find_drag_source (GtkWidget *widget)
{
    GListModel *controllers;
    guint i, n;
    GtkDragSource *result = NULL;

    controllers = gtk_widget_observe_controllers (widget);
    if (controllers == NULL)
        return NULL;

    n = g_list_model_get_n_items (controllers);
    for (i = 0; i < n; i++) {
        GObject *obj = g_list_model_get_item (controllers, i);
        if (GTK_IS_DRAG_SOURCE (obj)) {
            result = GTK_DRAG_SOURCE (obj);
            g_object_unref (obj);
            break;
        }
        g_object_unref (obj);
    }
    g_object_unref (controllers);
    return result;
}

/* GtkDragSource::drag-cancel — fires when the user releases the
 * drag outside any accepting target (NO_TARGET), or hits Esc
 * (USER_CANCELLED), or an error happens (ERROR). Only NO_TARGET
 * triggers our spawn-a-new-window path; the others should leave
 * the panel where it was.
 *
 * The cancel signal expects a boolean — TRUE inhibits the default
 * cancel handling. We return FALSE so libpanel's preview clearing
 * runs as usual; the panel undock is performed as a side effect
 * before we return. */
static gboolean
on_libpanel_drag_cancel (GtkDragSource       *src,
                         GdkDrag             *drag,
                         GdkDragCancelReason  reason,
                         gpointer             user_data)
{
    GdkContentProvider *cp;
    GValue value = G_VALUE_INIT;

    (void)src;
    (void)user_data;

    debug_log ("dnd", "drag-cancel: reason=%d", (int)reason);

    /* Trigger undock for any drag failure that wasn't an explicit
     * Esc-press by the user. On Wayland a drag released over an
     * area with no accepting surface frequently comes back as
     * GDK_DRAG_CANCEL_ERROR rather than NO_TARGET — the Wayland
     * drag protocol surfaces protocol-level "couldn't complete"
     * as ERROR generically. Treating both as undock-intent gives
     * the user the behaviour they expect on either backend. */
    if (reason == GDK_DRAG_CANCEL_USER_CANCELLED)
        return FALSE;

    /* libpanel installs the content via the GtkDragSource::prepare
     * signal's return value (not gtk_drag_source_set_content), so
     * gtk_drag_source_get_content returns NULL. The actual content
     * lives on the GdkDrag for the duration of the operation. */
    cp = gdk_drag_get_content (drag);
    if (cp == NULL)
        return FALSE;

    g_value_init (&value, PANEL_TYPE_WIDGET);
    if (gdk_content_provider_get_value (cp, &value, NULL)) {
        GObject *obj = g_value_get_object (&value);
        if (obj != NULL && HX_IS_PANEL (obj))
            hx_panel_undock (HX_PANEL (obj));
    }
    g_value_unset (&value);
    return FALSE;
}

void
hx_panel_install_drag_out_on_frame (GtkWidget *frame)
{
    GtkButton     *btn;
    GtkDragSource *src;

    g_return_if_fail (frame == NULL || PANEL_IS_FRAME (frame));
    if (frame == NULL)
        return;

    btn = find_drag_button (frame);
    if (btn == NULL) {
        //g_warning ("hx_panel_install_drag_out_on_frame: no drag handle on "
        //           "%s — libpanel API changed?", G_OBJECT_TYPE_NAME (frame));
        return;
    }

    src = find_drag_source (GTK_WIDGET (btn));
    if (src == NULL) {
        //g_warning ("hx_panel_install_drag_out_on_frame: drag handle has "
        //           "no GtkDragSource — libpanel API changed?");
        return;
    }

    g_signal_connect (src, "drag-cancel",
                      G_CALLBACK (on_libpanel_drag_cancel), NULL);

    /* libpanel doesn't set actions on its drag source, leaving it at
     * the default (0). With actions=0 the drag-drop action
     * negotiation fails everywhere — no drop target can ever accept,
     * which is what was making in-dock drag-between-frames look
     * impossible. Set MOVE | COPY explicitly so drop targets along
     * the path have something to negotiate against. */
    gtk_drag_source_set_actions (src, GDK_ACTION_MOVE | GDK_ACTION_COPY);

    debug_log ("dnd", "installed drag-out on frame=%p (drag_source=%p)",
               frame, src);
}

/* --- Per-frame drop target -------------------------------------- */

/* The high-level redock target on toolbar_dock + toolbar_window
 * we tried earlier interfered with libpanel's per-frame
 * PanelDropControls. We then learned that PanelDropControls
 * weren't accepting drops in our setup at all — drag-within-dock
 * was already broken before our outer target was added.
 *
 * Solution: own the drop entirely. Install a GtkDropTarget on
 * each PanelFrame. Accepts PANEL_TYPE_WIDGET drops. On drop, we
 * pop the panel out of its source frame and add it to the target
 * frame, raising. Same handler works for in-dock moves and
 * cross-dock (from an undocked window) moves. */

/* Forward decl — used by on_frame_drop to check whether the source
 * undocked dock is empty after the drop, so we only destroy windows
 * that truly have no panels left. */
static void collect_frames (GtkWidget *root, GPtrArray *out);

/* Forward decl — needed by on_frame_drop to disconnect the
 * undocked-window close handler before destroying the source
 * window, just like the old toolbar-level redock handler did. */
static gboolean on_undocked_close_request (GtkWindow *window,
                                           gpointer   user_data);

static gboolean
on_frame_drop (GtkDropTarget *target,
               const GValue  *value,
               double         x,
               double         y,
               gpointer       user_data)
{
    PanelFrame *target_frame = PANEL_FRAME (user_data);
    GObject   *obj;
    HxPanel   *panel;
    GtkWidget *src_frame;
    GtkWidget *src_dock;
    GtkWidget *target_dock;
    GtkRoot   *src_root = NULL;
    gboolean   was_cross_dock = FALSE;

    (void)target;
    (void)x;
    (void)y;

    debug_log ("dnd", "frame_drop: value type=%s, target_frame=%p",
               G_VALUE_TYPE_NAME (value), target_frame);

    if (!G_VALUE_HOLDS (value, PANEL_TYPE_WIDGET))
        return FALSE;
    obj = g_value_get_object (value);
    if (obj == NULL || !HX_IS_PANEL (obj))
        return FALSE;
    panel = HX_PANEL (obj);

    src_frame = gtk_widget_get_ancestor (GTK_WIDGET (panel),
                                         PANEL_TYPE_FRAME);
    src_dock  = gtk_widget_get_ancestor (GTK_WIDGET (panel),
                                         PANEL_TYPE_DOCK);
    target_dock = gtk_widget_get_ancestor (GTK_WIDGET (target_frame),
                                           PANEL_TYPE_DOCK);

    /* No-op when dropping onto the panel's current frame. */
    if (src_frame == (GtkWidget *)target_frame)
        return FALSE;

    /* Was this a cross-dock drag? If so we'll close the source
     * window once the panel has moved. */
    if (src_dock != NULL && target_dock != NULL && src_dock != target_dock) {
        was_cross_dock = TRUE;
        src_root = gtk_widget_get_root (src_dock);
    }

    g_object_ref (panel);
    if (src_frame != NULL)
        panel_frame_remove (PANEL_FRAME (src_frame), PANEL_WIDGET (panel));

    panel_frame_add (target_frame, PANEL_WIDGET (panel));
    panel_widget_raise (PANEL_WIDGET (panel));

    /* Update the panel's home record so a later Close-all-pages +
     * toolbar-button re-show via hx_panel_ensure_attached lands
     * the panel where the user just dropped it, not where it
     * originally started.
     *
     * BUT only rehome to the main toolbar_dock. Dropping into an
     * undocked window must not change the panel's "home" — its
     * home is where it returns to when the undocked window
     * closes, which is necessarily in the main dock. (Otherwise
     * we'd loop: home is in the undocked window, on close the
     * panel goes "home" to itself.) */
    if (target_dock == toolbar_dock) {
        GtkWidget *tf = GTK_WIDGET (target_frame);
        PanelArea  new_area;
        /* Update home_frame unconditionally — the user's exact
         * destination leaf is what should come back on re-show.
         *
         * Only update home_area when the drop landed in one of the
         * four default leaves. A drop into a user-created split
         * leaf leaves the panel's original area intent intact: if
         * that custom leaf is later closed while the panel is
         * detached, hx_panel_ensure_attached's home_area fallback
         * should send the panel back to where it ORIGINALLY lived
         * (e.g. a sidebar panel returns to its sidebar default),
         * not to whatever area the user-created leaf might happen
         * to overlap. */
        if (panel_area_for_default_frame (tf, &new_area))
            panel->home_area = new_area;
        hx_panel_set_home_frame (panel, tf);
    }
    g_object_unref (panel);

    /* DnD between main-dock frames is a placement change; persist
     * the new layout. No-op on cross-dock drops since the panel
     * left the main dock — the destination is an undocked window
     * whose state isn't part of the saved layout (yet). */
    if (target_dock == toolbar_dock)
        dock_layout_request_save ();

    /* On a cross-dock drop, if the source undocked window is now
     * empty, destroy it. Earlier this assumed an undocked window
     * only ever held one panel, but the user can stack multiple
     * panels into an undocked window via subsequent drops; in that
     * case we leave the window open so the remaining panels stay
     * usable.
     *
     * Disconnect the close-request handler before destroying so it
     * doesn't try to redock the (already-moved) panel via
     * home_frame. */
    /* Close the source window only when the drag came from an
     * UNDOCKED window — never close the main toolbar window even
     * if it happens to be the source. */
    if (was_cross_dock && GTK_IS_WINDOW (src_root)
        && src_dock != toolbar_dock) {
        gboolean src_dock_empty = TRUE;
        GPtrArray *frames = g_ptr_array_new ();
        guint i;
        collect_frames (src_dock, frames);
        for (i = 0; i < frames->len; i++) {
            PanelFrame *f = PANEL_FRAME (g_ptr_array_index (frames, i));
            if (panel_frame_get_n_pages (f) > 0) {
                src_dock_empty = FALSE;
                break;
            }
        }
        g_ptr_array_unref (frames);

        if (src_dock_empty) {
            /* Disconnect by handler id (stashed at connect time)
             * rather than by (function + user_data). The original
             * connection was made for whichever panel created the
             * undocked window via hx_panel_undock — that may not
             * be the panel currently being moved (the user can
             * stack multiple panels into one undocked window).
             * disconnect-by-func with the wrong user_data is a
             * silent no-op, leaving on_undocked_close_request to
             * fire on destroy and redock the original panel
             * unexpectedly. */
            gulong handler_id = (gulong)GPOINTER_TO_SIZE (
                g_object_get_data (G_OBJECT (src_root),
                                   "hx-undocked-close-handler-id"));
            if (handler_id != 0) {
                g_signal_handler_disconnect (src_root, handler_id);
                g_object_set_data (G_OBJECT (src_root),
                                   "hx-undocked-close-handler-id",
                                   NULL);
            }
            gtk_window_destroy (GTK_WINDOW (src_root));
        }
    }

    return TRUE;
}

static void
defang_drop_controls (GtkWidget *root)
{
    GtkWidget *child;

    if (root == NULL)
        return;

    if (g_strcmp0 (G_OBJECT_TYPE_NAME (root), "PanelDropControls") == 0) {
        gtk_widget_set_can_target (root, FALSE);
        return;
    }

    for (child = gtk_widget_get_first_child (root);
         child != NULL;
         child = gtk_widget_get_next_sibling (child)) {
        defang_drop_controls (child);
    }
}

void
hx_panel_defang_drop_controls_on_frame (GtkWidget *frame)
{
    g_return_if_fail (frame == NULL || PANEL_IS_FRAME (frame));
    if (frame == NULL)
        return;

    /* Disable libpanel's invisible PanelDropControls so they don't
     * claim drop events. They become visible during a drag in
     * libpanel's normal flow but stay hidden / non-functional in
     * our setup; with can-target=FALSE they're transparent to
     * pointer events and the dock-level drop target can see the
     * drop. The actual drop handler lives on the PanelDock, not
     * here — this function is purely a libpanel workaround. */
    defang_drop_controls (frame);
}

/* --- Dock-level drop target ------------------------------------- */

/* Find the PanelFrame nearest to dock coordinates (x, y). We walk
 * every descendant frame of the dock and return the one whose
 * allocated bounds (translated into dock coordinates) contain the
 * point. Used by the dock-level drop target's drop handler to
 * route the drop to the right frame when per-frame drop targets
 * don't fire. */
static void
collect_frames (GtkWidget *root, GPtrArray *out)
{
    GtkWidget *child;
    if (root == NULL) return;
    if (PANEL_IS_FRAME (root))
        g_ptr_array_add (out, root);
    for (child = gtk_widget_get_first_child (root);
         child != NULL;
         child = gtk_widget_get_next_sibling (child))
        collect_frames (child, out);
}

static PanelFrame *
frame_at_dock_coords (GtkWidget *dock, double x, double y)
{
    GPtrArray *frames = g_ptr_array_new ();
    PanelFrame *result = NULL;
    guint i;

    collect_frames (dock, frames);
    for (i = 0; i < frames->len; i++) {
        GtkWidget *frame = g_ptr_array_index (frames, i);
        graphene_point_t pt = GRAPHENE_POINT_INIT ((float)x, (float)y);
        graphene_point_t out_pt;
        graphene_rect_t bounds;
        if (gtk_widget_compute_point (dock, frame, &pt, &out_pt) &&
            gtk_widget_compute_bounds (frame, frame, &bounds) &&
            graphene_rect_contains_point (&bounds, &out_pt)) {
            /* Prefer the most-nested matching frame (a center grid
             * is a descendant of its dock; both might match — the
             * frame is more specific). Since we walked the tree,
             * later results are deeper. */
            result = PANEL_FRAME (frame);
        }
    }
    g_ptr_array_unref (frames);
    return result;
}

static gboolean
on_dock_drop (GtkDropTarget *target,
              const GValue  *value,
              double         x,
              double         y,
              gpointer       user_data)
{
    GtkWidget *dock = GTK_WIDGET (user_data);
    PanelFrame *target_frame;
    GValue val_copy = G_VALUE_INIT;
    gboolean ret;

    debug_log ("dnd", "dock_drop: x=%g y=%g, value type=%s",
               x, y, G_VALUE_TYPE_NAME (value));

    if (!G_VALUE_HOLDS (value, PANEL_TYPE_WIDGET))
        return FALSE;

    target_frame = frame_at_dock_coords (dock, x, y);
    debug_log ("dnd", "dock_drop: target_frame=%p", target_frame);
    if (target_frame == NULL)
        return FALSE;

    /* Reuse on_frame_drop's logic by calling it directly with the
     * target frame as user_data. We have to copy the value because
     * GtkDropTarget gives us a borrowed const GValue. */
    g_value_init (&val_copy, G_VALUE_TYPE (value));
    g_value_copy (value, &val_copy);
    ret = on_frame_drop (target, &val_copy, x, y, target_frame);
    g_value_unset (&val_copy);
    return ret;
}

static GdkDragAction
on_dock_enter (GtkDropTarget *target, double x, double y, gpointer user_data)
{
    (void)target;
    (void)x;
    (void)y;
    (void)user_data;
    return GDK_ACTION_MOVE;
}

static GdkDragAction
on_dock_motion (GtkDropTarget *target, double x, double y, gpointer user_data)
{
    (void)target;
    (void)x;
    (void)y;
    (void)user_data;
    return GDK_ACTION_MOVE;
}

void
hx_panel_install_drop_target_on_dock (GtkWidget *dock)
{
    GtkDropTarget *target;
    GType types[] = { PANEL_TYPE_WIDGET, GTK_TYPE_WIDGET, G_TYPE_OBJECT };

    g_return_if_fail (dock == NULL || PANEL_IS_DOCK (dock));
    if (dock == NULL)
        return;

    /* Accept anything widget-typed and ALL drag actions. libpanel's
     * GtkDragSource doesn't set explicit actions (it relies on the
     * default), so we err on the side of accepting MOVE | COPY to
     * make the action negotiation succeed. The single-GType list
     * (PANEL_TYPE_WIDGET) wasn't matching at enter time during the
     * Phase 4a debugging, even though it should have — going broad
     * with the supertypes made the GdkContentFormats intersection
     * succeed.  Preload forces eager content fetch so type matching
     * happens at enter time, not just at drop. */
    target = gtk_drop_target_new (G_TYPE_INVALID,
                                  GDK_ACTION_MOVE | GDK_ACTION_COPY);
    gtk_drop_target_set_gtypes (target, types, G_N_ELEMENTS (types));
    gtk_drop_target_set_preload (target, TRUE);
    g_signal_connect (target, "drop",   G_CALLBACK (on_dock_drop),   dock);
    g_signal_connect (target, "enter",  G_CALLBACK (on_dock_enter),  dock);
    g_signal_connect (target, "motion", G_CALLBACK (on_dock_motion), dock);
    gtk_widget_add_controller (dock, GTK_EVENT_CONTROLLER (target));

    debug_log ("dnd", "installed dock drop target on %p", dock);
}

void
hx_panel_set_home_frame (HxPanel *self, GtkWidget *frame)
{
    g_return_if_fail (HX_IS_PANEL (self));
    g_return_if_fail (frame == NULL || PANEL_IS_FRAME (frame));
    g_weak_ref_set (&self->home_frame, frame);
}

void
hx_panel_ensure_attached (HxPanel *self)
{
    GtkWidget *target;
    GtkWidget *parent;
    GtkWidget *frame_anc;

    g_return_if_fail (HX_IS_PANEL (self));

    /* "Attached" means there's a PanelFrame ancestor — i.e., the
     * panel is hooked into the dock's widget tree, not just sitting
     * in a stale parent. A bare gtk_widget_get_parent check isn't
     * enough: libadwaita's AdwBin can survive briefly after the
     * AdwTabPage that wraps it is closed; that AdwBin still appears
     * as the panel's parent until its dispose runs. The
     * PanelFrame-ancestor test catches both the "really detached"
     * case (no ancestor) and the "stale AdwBin hanging on" case. */
    frame_anc = gtk_widget_get_ancestor (GTK_WIDGET (self),
                                         PANEL_TYPE_FRAME);
    if (frame_anc != NULL)
        return;

    /* No frame ancestor — we're going to re-attach. If there's a
     * dangling parent left over from a half-cleaned close (an AdwBin
     * is the case we've actually observed; gtk_widget_unparent for
     * anything else), shake it loose first. panel_frame_add /
     * panel_grid_add below both fail noisily if the widget already
     * has a parent. */
    parent = gtk_widget_get_parent (GTK_WIDGET (self));
    if (parent != NULL) {
        g_object_ref (self);
        if (ADW_IS_BIN (parent)) {
            adw_bin_set_child (ADW_BIN (parent), NULL);
        } else {
            gtk_widget_unparent (GTK_WIDGET (self));
        }
        /* g_object_ref guards against the unparent dropping the
         * last reference. The registry keeps a ref, but be
         * defensive. */
        g_object_unref (self);
    }

    /* Phase 5b / docking: prefer the panel's existing home_frame
     * weak ref. The user can move panels into user-created split
     * leaves (via Move-direction or DnD); home_frame records
     * which leaf they wanted, and the home_area is just a fallback
     * for the case where the home_frame's weak ref has expired
     * (e.g. close-frame destroyed the leaf the user moved into).
     *
     * Before Phase 5b this function ignored home_frame entirely
     * and re-attached on home_area alone, which clobbered the
     * user's actual placement on every Close-all-pages /
     * toolbar-button re-show round trip. */
    {
        GtkWidget *home = hx_panel_get_home_frame (self);
        gboolean home_usable = FALSE;
        if (home != NULL && PANEL_IS_FRAME (home)
            && gtk_widget_get_parent (home) != NULL) {
            /* Still parented in the dock tree — use it. */
            target = home;
            home_usable = TRUE;
        } else {
            target = NULL;
        }
        if (home != NULL)
            g_object_unref (home);  /* get_home_frame strong ref */

        if (!home_usable) {
            /* Fall back to home_area default. Only update the
             * stored home_frame in this branch — when home_frame
             * was already usable, leave it untouched so the user's
             * choice persists. */
            switch (self->home_area) {
            case PANEL_AREA_START:  target = toolbar_sidebar_frame; break;
            case PANEL_AREA_END:    target = toolbar_end_frame;     break;
            case PANEL_AREA_BOTTOM: target = toolbar_bottom_frame;  break;
            case PANEL_AREA_CENTER:
            default:                target = toolbar_center_frame;  break;
            }
            if (target == NULL)
                return;
            panel_frame_add (PANEL_FRAME (target), PANEL_WIDGET (self));
            hx_panel_set_home_frame (self, target);
            return;
        }
    }

    panel_frame_add (PANEL_FRAME (target), PANEL_WIDGET (self));
    /* home_frame already points here — don't reset. */
}

GtkWidget *
hx_panel_get_home_frame (HxPanel *self)
{
    GObject *obj;

    g_return_val_if_fail (HX_IS_PANEL (self), NULL);

    /* g_weak_ref_get returns a strong reference. Pass it through:
     * the caller is responsible for g_object_unref when done.
     * Earlier revisions unref'd here and returned a "borrowed"
     * pointer, but that's unsound — if the weak ref was the last
     * strong ref, the frame is destroyed inside the unref and the
     * returned pointer dangles. */
    obj = g_weak_ref_get (&self->home_frame);
    if (obj == NULL)
        return NULL;
    return GTK_WIDGET (obj);
}

/* close-request on the undocked window: pull the panel back to its
 * home frame, re-reveal a collapsed side area, then let the window
 * finish closing. Same pattern as the spike. */
static gboolean
on_undocked_close_request (GtkWindow *window, gpointer user_data)
{
    HxPanel   *self = HX_PANEL (user_data);
    GtkWidget *home;
    GtkWidget *parent_frame;

    g_object_ref (self);

    home = hx_panel_get_home_frame (self);
    parent_frame = gtk_widget_get_ancestor (GTK_WIDGET (self),
                                            PANEL_TYPE_FRAME);
    if (parent_frame != NULL)
        panel_frame_remove (PANEL_FRAME (parent_frame), PANEL_WIDGET (self));

    if (home != NULL) {
        panel_frame_add (PANEL_FRAME (home), PANEL_WIDGET (self));
        /* Phase 5b / docking: no more PanelDock revealers; the
         * tree just shows everything that's in it. No area
         * reveal flip needed on Redock. */
        g_object_unref (home);  /* hx_panel_get_home_frame strong ref */
    }

    /* Redock changes which main-dock leaf the panel lives in. */
    dock_layout_request_save ();

    g_object_unref (self);
    return FALSE;  /* let the window close */
}

/* notify::default-width / notify::default-height handler attached
 * to every undocked window. Resize fires this; we just request a
 * debounced save so the [Undocked] section captures the new size. */
static void
on_undocked_window_size_notify (GObject *object, GParamSpec *pspec, gpointer data)
{
    (void)object; (void)pspec; (void)data;
    dock_layout_request_save ();
}

void
hx_panel_undock (HxPanel *self)
{
    GtkWidget         *current_frame;
    GtkBuilder        *builder;
    GtkBuilderScope   *scope;
    GtkWindow         *window;
    PanelGrid         *grid;
    GApplication      *app;
    GError            *err = NULL;
    const char        *title;

    /* Same XML the spike uses — minimal AdwApplicationWindow with a
     * PanelDock whose center is a PanelGrid. Built via GtkBuilder so
     * libpanel's GtkBuildable add-child wraps the grid in the dock's
     * CENTER area correctly (gtk_widget_set_parent would skip that). */
    static const char *undocked_ui_xml =
        "<interface>"
        "  <object class='AdwApplicationWindow' id='window'>"
        "    <property name='default-width'>640</property>"
        "    <property name='default-height'>480</property>"
        "    <property name='content'>"
        "      <object class='AdwToolbarView'>"
        "        <child type='top'>"
        "          <object class='AdwHeaderBar'/>"
        "        </child>"
        "        <property name='content'>"
        "          <object class='PanelDock' id='dock'>"
        "            <child>"
        "              <object class='PanelGrid' id='grid'>"
        "                <signal name='create-frame' "
        "                        handler='hx_panel_undocked_create_frame'/>"
        "              </object>"
        "            </child>"
        "          </object>"
        "        </property>"
        "      </object>"
        "    </property>"
        "  </object>"
        "</interface>";

    g_return_if_fail (HX_IS_PANEL (self));

    g_object_ref (self);

    current_frame = gtk_widget_get_ancestor (GTK_WIDGET (self),
                                             PANEL_TYPE_FRAME);
    if (current_frame == NULL) {
        // g_warning ("Undock: panel %s has no ancestor frame", self->id);
        g_object_unref (self);
        return;
    }

    /* Phase 5b / docking: if the panel is already in an undocked
     * window (its widget root is NOT the main toolbar window),
     * "Undock" is meaningless — what the user wants is "Dock"
     * (redock to main). Close the source window; its
     * close-request handler (on_undocked_close_request) takes
     * care of moving the panel back to its home frame.
     *
     * The pre-Phase-5b version walked up to PANEL_TYPE_DOCK and
     * compared with toolbar_dock. With the main dock no longer a
     * PanelDock, the ancestor walk had to learn the new shape;
     * the simpler test of "is this panel's GtkRoot the toolbar
     * window?" works for the same purpose. Undocked windows are
     * still AdwApplicationWindows separate from toolbar_window. */
    {
        GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));
        if (root != NULL && GTK_WIDGET (root) != toolbar_window) {
            if (GTK_IS_WINDOW (root))
                gtk_window_close (GTK_WINDOW (root));
            g_object_unref (self);
            return;
        }
    }

    panel_frame_remove (PANEL_FRAME (current_frame), PANEL_WIDGET (self));

    builder = gtk_builder_new ();
    scope = gtk_builder_get_scope (builder);
    gtk_builder_cscope_add_callback_symbol (
        GTK_BUILDER_CSCOPE (scope), "hx_panel_undocked_create_frame",
        G_CALLBACK (hx_panel_undocked_create_frame));
    if (!gtk_builder_add_from_string (builder, undocked_ui_xml, -1, &err)) {
        g_critical ("HxPanel Undock: builder failed: %s",
                    err ? err->message : "?");
        g_clear_error (&err);
        g_object_unref (builder);
        g_object_unref (self);
        return;
    }

    window = GTK_WINDOW (gtk_builder_get_object (builder, "window"));
    grid   = PANEL_GRID (gtk_builder_get_object (builder, "grid"));

    app = g_application_get_default ();
    if (app != NULL)
        gtk_window_set_application (window, GTK_APPLICATION (app));

    title = panel_widget_get_title (PANEL_WIDGET (self));
    gtk_window_set_title (window, title ? title : _ ("Undocked panel"));

    panel_grid_add (grid, PANEL_WIDGET (self));

    {
        gulong handler_id = g_signal_connect (
            window, "close-request",
            G_CALLBACK (on_undocked_close_request), self);
        /* Stash the handler id so the cross-dock-drop path
         * (on_frame_drop) can disconnect it by id rather than by
         * (function + user_data). disconnect-by-func only matches
         * when the user_data the close-request was connected with
         * is the panel currently being moved — false whenever the
         * user has stacked multiple panels into the same undocked
         * window. */
        g_object_set_data (G_OBJECT (window),
                           "hx-undocked-close-handler-id",
                           GSIZE_TO_POINTER ((gsize)handler_id));
    }

    /* Standard Ctrl+W / Ctrl+Q / Ctrl+K / Ctrl+T shortcuts. Without
     * this an undocked panel loses the keyboard accelerators every
     * other window in the app has — the panel widget itself doesn't
     * carry them. */
    init_keyaccel (GTK_WIDGET (window));

    /* Persist user-resize. GTK 4's GtkWindow keeps default-width
     * and default-height in sync with the actual surface size on
     * user resize (that's the per-session-size persistence design
     * — gtk_window_get_default_size returns the live size for
     * exactly this reason). notify:: on either property fires on
     * every resize step; dock_layout_request_save debounces, so a
     * drag-resize collapses to one write. */
    g_signal_connect (window, "notify::default-width",
                      G_CALLBACK (on_undocked_window_size_notify), NULL);
    g_signal_connect (window, "notify::default-height",
                      G_CALLBACK (on_undocked_window_size_notify), NULL);

    gtk_window_present (window);

    /* Undocking removed the panel from the main dock; persist
     * the change so it doesn't re-appear after restart. */
    dock_layout_request_save ();

    g_object_unref (builder);
    g_object_unref (self);
}

/* GAction wrapper: chevron-menu Undock entry hands us
 * (action, parameter, panel) and we just forward to the public
 * undock function. The drag-out detector calls hx_panel_undock
 * directly without going through here. */
static void
on_undock_activate (GSimpleAction *action,
                    GVariant      *parameter,
                    gpointer       user_data)
{
    (void)action;
    (void)parameter;
    hx_panel_undock (HX_PANEL (user_data));
}

/* The undocked window's grid needs a create-frame handler. Uses
 * a plain PanelFrame — NOT HxPanelFrame — so libpanel's built-in
 * page.move-{left,right,up,down} class actions remain in force.
 * Those actions reflow pages between PanelGrid cells (auto-
 * creating a new column at the edge), which is the right
 * behaviour for an undocked-window layout. HxPanelFrame's
 * override targets the main dock's HxSplit tree and would
 * disable the chevron Move Page items here because there's no
 * HxSplit ancestor in an undocked window.
 *
 * The other HxPanel hookups still apply — close-dispatcher
 * (DYNAMIC panel teardown + registry unregister), drag-out
 * (drag onto desktop → spawn yet another window) and drop-
 * controls defang — they're frame-level concerns, independent
 * of the HxSplit world. */
static PanelFrame *
hx_panel_undocked_create_frame (PanelGrid *grid, gpointer user_data)
{
    GtkWidget        *frame  = panel_frame_new ();
    PanelFrameHeader *header = PANEL_FRAME_HEADER (panel_frame_header_bar_new ());
    (void)grid;
    (void)user_data;
    panel_frame_set_header (PANEL_FRAME (frame), header);
    hx_panel_install_close_dispatcher (frame);
    hx_panel_install_drag_out_on_frame (frame);
    hx_panel_defang_drop_controls_on_frame (frame);
    return PANEL_FRAME (frame);
}

/* on_move_area_activate retired — panel.move-area gave way to
 * the four relative-direction actions (panel.move-left / right /
 * up / down) plus split / close-frame. See on_move_direction_
 * activate below. */

/* ----------------------------------------------------------------- */
/* Phase 5a / docking — relative move + split + close-frame          */
/* ----------------------------------------------------------------- */

/* Phase 5b / docking: panel_neighbor_across_areas removed. The
 * dock is now ONE recursive HxSplit tree (no PanelDock; no fixed
 * areas), so hx_split_neighbor walks the entire dock and finds
 * any neighbour the user could navigate to. The cross-area
 * bridge that existed during Phase 5a's four-tree period is no
 * longer needed. */

/* Walk up from the panel to its HxSplit leaf. Returns NULL if the
 * panel isn't inside a split tree (e.g. it's in an undocked
 * window — in that case the relative-move / split / close-frame
 * actions are no-ops). */
static HxSplit *
panel_get_split_leaf (HxPanel *self)
{
    GtkWidget *anc = gtk_widget_get_ancestor (GTK_WIDGET (self),
                                              HX_TYPE_SPLIT);
    if (anc == NULL)
        return NULL;
    if (!hx_split_is_leaf (HX_SPLIT (anc))) {
        /* HxSplit ancestor is an internal split — find the leaf
         * by walking from the panel's PanelFrame ancestor to the
         * matching leaf within `anc`'s tree. */
        GtkWidget *frame_anc = gtk_widget_get_ancestor (GTK_WIDGET (self),
                                                        PANEL_TYPE_FRAME);
        if (frame_anc == NULL)
            return NULL;
        return hx_split_find_for_frame (HX_SPLIT (anc),
                                        PANEL_FRAME (frame_anc));
    }
    return HX_SPLIT (anc);
}

/* Cross-frame neighbor move. Used by HxPanelFrame's page.move-*
 * class-action handlers (hx_panel_frame.c) so that libpanel's
 * always-greyed "Move Page L/R/U/D" chevron items, which assume a
 * PanelGrid layout we don't have, become a working cross-frame
 * move across the HxSplit tree. */
void
hx_panel_do_move_in_direction (HxPanel *self, GtkDirectionType dir)
{
    HxSplit    *leaf;
    HxSplit    *target_leaf;
    GtkWidget  *current_frame;
    PanelFrame *target_frame;

    g_return_if_fail (HX_IS_PANEL (self));

    leaf = panel_get_split_leaf (self);
    if (leaf == NULL) {
        debug_log ("dock",
                   "hx_panel %s: page.move-* — no HxSplit ancestor",
                   self->id ? self->id : "(unset)");
        return;
    }
    target_leaf = hx_split_neighbor (leaf, dir);
    if (target_leaf == NULL) {
        debug_log ("dock",
                   "hx_panel %s: page.move-* — no neighbour leaf in that "
                   "direction (tree edge)",
                   self->id ? self->id : "(unset)");
        return;
    }
    target_frame = hx_split_get_frame (target_leaf);
    if (target_frame == NULL)
        return;

    current_frame = gtk_widget_get_ancestor (GTK_WIDGET (self),
                                             PANEL_TYPE_FRAME);
    if (current_frame == (GtkWidget *) target_frame)
        return;  /* shouldn't happen — neighbour by definition is elsewhere */

    g_object_ref (self);
    if (current_frame != NULL)
        panel_frame_remove (PANEL_FRAME (current_frame), PANEL_WIDGET (self));
    panel_frame_add (target_frame, PANEL_WIDGET (self));
    {
        GtkWidget *tf = GTK_WIDGET (target_frame);
        PanelArea  new_area;
        /* home_frame tracks the exact leaf the user chose.
         * home_area only follows along when the destination is one
         * of the four default leaves; a move into a user-created
         * split leaf preserves the panel's original area intent so
         * that — if that custom leaf is later closed while the
         * panel is detached — the home_area fallback in
         * hx_panel_ensure_attached returns the panel to its
         * original default frame rather than coercing it onto
         * whatever area sentinel the custom leaf might map to. */
        if (panel_area_for_default_frame (tf, &new_area))
            self->home_area = new_area;
        hx_panel_set_home_frame (self, tf);
    }
    panel_widget_raise (PANEL_WIDGET (self));
    dock_layout_request_save ();
    g_object_unref (self);
}

gboolean
hx_panel_can_move_in_direction (HxPanel *self, GtkDirectionType dir)
{
    HxSplit *leaf;

    g_return_val_if_fail (HX_IS_PANEL (self), FALSE);

    leaf = panel_get_split_leaf (self);
    if (leaf == NULL)
        return FALSE;
    return hx_split_neighbor (leaf, dir) != NULL;
}

/* Per-panel split / close-frame handlers retired — those
 * operations now live on each PanelFrame's header via
 * hx_split_install_frame_ui. Per-panel chevron stays focused on
 * undock; libpanel's chevron items Move Page L/R/U/D are routed
 * to hx_panel_do_move_in_direction by HxPanelFrame's overriding
 * class actions (src/hx_panel_frame.c). */
