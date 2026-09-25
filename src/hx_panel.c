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
#include "compat.h"  /* _() gettext macro for menu labels */
#include "hx.h"      /* session typedef, required by gtkutil.h and toolbar.h */
#include "gtkutil.h" /* init_keyaccel for undocked windows */
#include "toolbar.h" /* toolbar_window — main-dock root comparison */
#include "gtkhx_theme.h" /* GTKHX_SCALE_TOOLBAR for the pane switcher */

#include <adwaita.h>

struct _HxPanel {
    PanelWidget parent_instance;

    char *id;
    HxPanelKind kind;
    PanelArea home_area;
    GWeakRef home_frame; /* PanelFrame the Undock action returns to */

    /* DYNAMIC panel close callback. NULL on static
     * panels; for pchat / msg panels the factory installs a
     * tear-down function that runs from the frame dispatcher
     * (page-closed signal) before the registry unregisters the
     * panel. */
    HxPanelCloseFunc close_func;
    gpointer close_data;

    /* panel.show-actions: whether the content's action row (the widget
     * carrying .gtkhx-panel-actions) is shown. Owned by the panel's
     * action group. */
    GSimpleAction *show_actions;
    gboolean has_actions; /* the content has an action row at all */

    /* The panel's own action group ("panel" to libpanel's muxer), kept
     * so the pane controls can reach it under "pane". */
    GSimpleActionGroup *actions;

    /* Content wrapper: an overlay whose child is the dock page stack and
     * whose one overlay is the pane controls — switcher, drag handle, menu
     * and close — which stand in for the frame's header when it is
     * hidden. NULL until content is set. */
    GtkWidget *overlay;
    GtkWidget *controls;
    GtkWidget *switcher; /* one button per panel sharing the frame */
    gboolean hovering;
    gboolean focus_within; /* keyboard focus is somewhere in the pane */
    gboolean dragging;     /* keeps the controls up while their handle drags */
    gboolean menu_open;    /* ...and while their menu is open */
};

G_DEFINE_FINAL_TYPE (HxPanel, hx_panel, PANEL_TYPE_WIDGET)

/* Forward decls */
static void hx_panel_sync_chrome (HxPanel *self);
static void on_panel_map (GtkWidget *widget, gpointer user_data);
static gboolean on_libpanel_drag_cancel (GtkDragSource *src, GdkDrag *drag,
                                         GdkDragCancelReason reason,
                                         gpointer user_data);
static void on_show_actions_change (GSimpleAction *action, GVariant *value,
                                    gpointer user_data);
static void on_undock_activate (GSimpleAction *action, GVariant *parameter,
                                gpointer user_data);
static gboolean on_undocked_close_request (GtkWindow *window,
                                           gpointer user_data);
static PanelFrame *hx_panel_undocked_create_frame (PanelGrid *grid,
                                                   gpointer user_data);

/* Toolbar globals exposed by toolbar.c — the home frames for each
 * of the four sidebar areas. NULL until create_toolbar_window has
 * run, but Phase 2 panels are constructed inside its tail so the
 * pointers are live by the time any move action fires. */
extern GtkWidget *toolbar_sidebar_frame; /* PANEL_AREA_START  */
extern GtkWidget *toolbar_end_frame;     /* PANEL_AREA_END    */
extern GtkWidget *toolbar_bottom_frame;  /* PANEL_AREA_BOTTOM */
extern GtkWidget *toolbar_center_frame;  /* PANEL_AREA_CENTER */
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
    if (frame == toolbar_sidebar_frame) {
        *out = PANEL_AREA_START;
        return TRUE;
    }
    if (frame == toolbar_end_frame) {
        *out = PANEL_AREA_END;
        return TRUE;
    }
    if (frame == toolbar_bottom_frame) {
        *out = PANEL_AREA_BOTTOM;
        return TRUE;
    }
    if (frame == toolbar_center_frame) {
        *out = PANEL_AREA_CENTER;
        return TRUE;
    }
    return FALSE;
}

static void
hx_panel_finalize (GObject *object)
{
    HxPanel *self = HX_PANEL (object);

    g_clear_pointer (&self->id, g_free);
    g_weak_ref_clear (&self->home_frame);
    g_clear_object (&self->actions);

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
    GSimpleAction *undock;
    GMenu *menu;

    /* Sensible defaults: every panel starts assuming it's a center
     * document. SIDEBAR / DYNAMIC panels override via hx_panel_new. */
    self->kind = HX_PANEL_KIND_CENTER;
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
    group = g_simple_action_group_new ();
    undock = g_simple_action_new ("undock", NULL);
    g_signal_connect (undock, "activate", G_CALLBACK (on_undock_activate),
                      self);
    g_action_map_add_action (G_ACTION_MAP (group), G_ACTION (undock));
    g_object_unref (undock);

    self->show_actions = g_simple_action_new_stateful (
        "show-actions", NULL, g_variant_new_boolean (TRUE));
    g_signal_connect (self->show_actions, "change-state",
                      G_CALLBACK (on_show_actions_change), self);
    g_action_map_add_action (G_ACTION_MAP (group),
                             G_ACTION (self->show_actions));
    g_object_unref (self->show_actions); /* the group holds the ref */

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
    self->actions = group; /* keep our ref for the pane controls */

    /* A panel that moves frames lands under a different header, among
     * different neighbors. */
    g_signal_connect (self, "map", G_CALLBACK (on_panel_map), NULL);

    /* Per-panel chevron menu: this panel's own items, then the frame's.
     * The Move Page items in the chevron come from libpanel's
     * frame_menu template and are rerouted to our cross-frame
     * neighbour move by the per-frame "page" action group install
     * in hx_panel_install_page_move_actions.
     *
     * Split + close-frame live here too, resolved against the frame's
     * "frame-ops" group up the widget tree, so a frame header carries
     * one menu instead of two. The separate split button
     * (hx_split_install_frame_ui) only shows on an empty frame, which
     * has no panel and so no chevron menu. */
    menu = g_menu_new ();
    {
        GMenu *panel_section = g_menu_new ();
        GMenu *frame_section = g_menu_new ();

        g_menu_append (panel_section, _ ("Show Toolbar"),
                       "page.panel.show-actions");
        g_menu_append (panel_section, _ ("Undock"), "page.panel.undock");
        g_menu_append (frame_section, _ ("Split Horizontally"),
                       "frame-ops.split-h");
        g_menu_append (frame_section, _ ("Split Vertically"),
                       "frame-ops.split-v");
        g_menu_append (frame_section, _ ("Close Frame"),
                       "frame-ops.close-frame");
        g_menu_append_section (menu, NULL, G_MENU_MODEL (panel_section));
        g_menu_append_section (menu, NULL, G_MENU_MODEL (frame_section));
        g_object_unref (panel_section);
        g_object_unref (frame_section);
    }
    panel_widget_set_menu_model (PANEL_WIDGET (self), G_MENU_MODEL (menu));
    g_object_unref (menu);
}

HxPanel *
hx_panel_new (const char *id, HxPanelKind kind, PanelArea home_area)
{
    HxPanel *self;

    g_return_val_if_fail (id != NULL && id[0] != '\0', NULL);

    self = g_object_new (HX_TYPE_PANEL, NULL);
    self->id = g_strdup (id);
    self->kind = kind;
    self->home_area = home_area;

    /* The base class already exposes "id" as a property
     * (panel_widget_set_id) — populate it from our copy so the rest
     * of libpanel sees the same string. We keep our own copy too
     * because panel_widget_get_id can return NULL transiently
     * (e.g. during dispose). */
    panel_widget_set_id (PANEL_WIDGET (self), id);

    g_simple_action_set_state (
        self->show_actions,
        g_variant_new_boolean (!dock_layout_panel_actions_hidden (id)));

    return self;
}

/* Show or hide every action row under `root`. Doesn't descend into a
 * match — a row's own children are buttons, not more rows. Returns
 * whether it found one. */
static gboolean
set_action_rows_visible (GtkWidget *root, gboolean visible)
{
    gboolean found = FALSE;

    if (gtk_widget_has_css_class (root, "gtkhx-panel-actions")) {
        gtk_widget_set_visible (root, visible);
        return TRUE;
    }
    for (GtkWidget *c = gtk_widget_get_first_child (root); c != NULL;
         c = gtk_widget_get_next_sibling (c)) {
        found |= set_action_rows_visible (c, visible);
    }
    return found;
}

void
hx_panel_sync_actions (HxPanel *self)
{
    GtkWidget *child;
    g_autoptr (GVariant) state = NULL;

    g_return_if_fail (HX_IS_PANEL (self));

    child = hx_panel_get_content (self);
    if (child == NULL) {
        return;
    }
    state = g_action_get_state (G_ACTION (self->show_actions));
    /* A panel with no action row (Chat) greys the item out rather than
     * offering a switch that does nothing. */
    self->has_actions
        = set_action_rows_visible (child, g_variant_get_boolean (state));
    g_simple_action_set_enabled (self->show_actions, self->has_actions);
    hx_panel_sync_chrome (self);
}

/* ------------------------------------------------------------------ */
/* Pane titles                                                         */
/* ------------------------------------------------------------------ */

/* A frame's header names what is plainly on screen, so it shows only
 * where it has a job the pane controls can't do: on an empty frame, which
 * has no panel to carry them and needs the header's split button. Pane
 * Titles in the main menu puts every header back. Switching between the
 * panels sharing a frame is the pane controls' switcher. */
static gboolean
frame_needs_header (PanelFrame *frame)
{
    return dock_layout_pane_titles_visible ()
           || panel_frame_get_n_pages (frame) == 0;
}

/* The pixmap a panel goes by on the toolbar, so the switcher reads the
 * same as the buttons that open the panels. NULL for a panel with no
 * toolbar button; the switcher falls back to its title. */
static const char *
panel_pixmap (const char *id)
{
    static const struct {
        const char *id;
        const char *resource;
    } map[] = {
        { HX_PANEL_ID_CHAT, "/com/nasledov/gtkhx/pixmaps/chat.png" },
        { HX_PANEL_ID_USERS, "/com/nasledov/gtkhx/pixmaps/users.png" },
        { HX_PANEL_ID_NEWS, "/com/nasledov/gtkhx/pixmaps/news.png" },
        { HX_PANEL_ID_NEWS15, "/com/nasledov/gtkhx/pixmaps/news_folder.png" },
        { HX_PANEL_ID_TASKS, "/com/nasledov/gtkhx/pixmaps/tasks.png" },
    };

    for (gsize i = 0; i < G_N_ELEMENTS (map); i++) {
        if (g_strcmp0 (id, map[i].id) == 0) {
            return map[i].resource;
        }
    }
    return NULL;
}

static void
on_switch_clicked (GtkButton *button, gpointer user_data)
{
    (void)button;
    panel_widget_raise (PANEL_WIDGET (user_data));
}

/* Rebuild the switcher from the frame's page list: one button per
 * panel, this one marked. Hidden for a panel alone in its frame, where
 * there is nothing to switch to. */
static void
hx_panel_rebuild_switcher (HxPanel *self)
{
    GtkWidget *frame, *child;
    guint n;

    if (self->switcher == NULL) {
        return;
    }
    while ((child = gtk_widget_get_first_child (self->switcher)) != NULL) {
        gtk_box_remove (GTK_BOX (self->switcher), child);
    }
    frame = gtk_widget_get_ancestor (GTK_WIDGET (self), PANEL_TYPE_FRAME);
    n = frame != NULL ? panel_frame_get_n_pages (PANEL_FRAME (frame)) : 0;
    gtk_widget_set_visible (self->switcher, n > 1);
    if (n < 2) {
        return;
    }
    for (guint i = 0; i < n; i++) {
        PanelWidget *page = panel_frame_get_page (PANEL_FRAME (frame), i);
        const char *title;
        const char *pixmap;
        GtkWidget *btn;

        if (page == NULL || !HX_IS_PANEL (page)) {
            continue;
        }
        title = panel_widget_get_title (page);
        pixmap = panel_pixmap (HX_PANEL (page)->id);
        if (pixmap != NULL) {
            btn = gtkhx_pixmap_button (pixmap, title, GTKHX_SCALE_TOOLBAR,
                                       G_CALLBACK (on_switch_clicked), page);
        } else {
            btn = gtk_button_new_with_label (title != NULL ? title : "?");
            gtk_widget_add_css_class (btn, "flat");
            g_signal_connect (btn, "clicked", G_CALLBACK (on_switch_clicked),
                              page);
        }
        /* Named for screen readers (the pixmap says nothing to them),
         * and the current one marked as such rather than only by color. */
        gtk_accessible_update_property (GTK_ACCESSIBLE (btn),
                                        GTK_ACCESSIBLE_PROPERTY_LABEL,
                                        title != NULL ? title : "", -1);
        if (page == PANEL_WIDGET (self)) {
            gtk_widget_add_css_class (btn, "gtkhx-switch-current");
            gtk_accessible_update_state (GTK_ACCESSIBLE (btn),
                                         GTK_ACCESSIBLE_STATE_PRESSED,
                                         GTK_ACCESSIBLE_TRISTATE_TRUE, -1);
        }
        gtk_box_append (GTK_BOX (self->switcher), btn);
    }
}

/* The widgets that may share the pane controls' corner: an action row,
 * or a widget tagged to make room (the chat's subject line and its tab
 * strip). */
static gboolean
is_corner_widget (GtkWidget *w)
{
    return gtk_widget_has_css_class (w, "gtkhx-panel-actions")
           || gtk_widget_has_css_class (w, "gtkhx-pane-reserve");
}

/* The widget that sits in the controls' corner on one content page: the
 * first *visible* corner widget in tree order. First, because that is the
 * one at the top — Files has more rows below its own. Visible, because a
 * hidden action row or an autohidden tab strip isn't there to make room
 * in. NULL when there is none. */
static GtkWidget *
find_corner_widget (GtkWidget *root)
{
    if (!gtk_widget_get_visible (root)) {
        return NULL;
    }
    if (is_corner_widget (root)) {
        return root;
    }
    for (GtkWidget *c = gtk_widget_get_first_child (root); c != NULL;
         c = gtk_widget_get_next_sibling (c)) {
        GtkWidget *found = find_corner_widget (c);
        if (found != NULL) {
            return found;
        }
    }
    return NULL;
}

static void
clear_corner_margins (GtkWidget *root)
{
    if (is_corner_widget (root)) {
        gtk_widget_set_margin_end (root, 0);
    }
    for (GtkWidget *c = gtk_widget_get_first_child (root); c != NULL;
         c = gtk_widget_get_next_sibling (c)) {
        clear_corner_margins (c);
    }
}

/* Make room for the controls on every content page — each connection has
 * its own page in the stack, and each its own corner widget — or, with
 * margin 0, give it all back. */
static void
reserve_for_controls (GtkWidget *content, int margin)
{
    clear_corner_margins (content);
    if (margin <= 0) {
        return;
    }
    if (GTK_IS_STACK (content)) {
        for (GtkWidget *page = gtk_widget_get_first_child (content);
             page != NULL; page = gtk_widget_get_next_sibling (page)) {
            GtkWidget *corner = find_corner_widget (page);
            if (corner != NULL) {
                gtk_widget_set_margin_end (corner, margin);
            }
        }
    } else {
        GtkWidget *corner = find_corner_widget (content);
        if (corner != NULL) {
            gtk_widget_set_margin_end (corner, margin);
        }
    }
}

/* The page on screen: the stack's visible child, or the content itself. */
static GtkWidget *
visible_page (GtkWidget *content)
{
    if (GTK_IS_STACK (content)) {
        return gtk_stack_get_visible_child (GTK_STACK (content));
    }
    return content;
}

/* Whether the frame's own chrome is out of the way: its header hidden
 * (pane titles off), or — in an undocked window — the header's controls
 * hidden, which takes its menu with them. Either way the pane controls
 * are the only route to the panel's menu. */
static gboolean
frame_chrome_hidden (GtkWidget *frame)
{
    PanelFrameHeader *header;
    GtkWidget *controls;

    if (frame == NULL) {
        return FALSE;
    }
    header = panel_frame_get_header (PANEL_FRAME (frame));
    if (header == NULL) {
        return FALSE;
    }
    if (!gtk_widget_get_visible (GTK_WIDGET (header))) {
        return TRUE;
    }
    controls
        = hx_panel_find_css_class_descendant (GTK_WIDGET (header), "controls");
    return controls != NULL && !gtk_widget_get_visible (controls);
}

/* Show, hide and place the pane controls. They are up for good where
 * there is a corner widget on the page for them to sit at the end of
 * (which makes room for them) and something worth keeping in view — an
 * action row, or a switcher, which would hide the very panels it exists
 * to reveal if it only appeared on hover. Otherwise they would sit over
 * content, so they appear on hover, or while keyboard focus is in the
 * pane so they can be reached without a pointer. */
static void
hx_panel_sync_chrome (HxPanel *self)
{
    GtkWidget *frame, *content, *corner;
    gboolean compact, row, pinned;
    g_autoptr (GVariant) state = NULL;

    if (self->overlay == NULL) {
        return;
    }
    frame = gtk_widget_get_ancestor (GTK_WIDGET (self), PANEL_TYPE_FRAME);
    compact = frame_chrome_hidden (frame);
    state = g_action_get_state (G_ACTION (self->show_actions));
    row = self->has_actions && g_variant_get_boolean (state);
    content = gtk_overlay_get_child (GTK_OVERLAY (self->overlay));
    corner
        = content != NULL ? find_corner_widget (visible_page (content)) : NULL;
    pinned = compact && corner != NULL
             && (row || gtk_widget_get_visible (self->switcher));

    gtk_widget_set_visible (self->controls,
                            pinned
                                || (compact
                                    && (self->hovering || self->focus_within
                                        || self->dragging || self->menu_open)));
    if (pinned) {
        int width = 0;

        gtk_widget_add_css_class (self->overlay, "gtkhx-pane-inline");
        gtk_widget_measure (self->controls, GTK_ORIENTATION_HORIZONTAL, -1,
                            NULL, &width, NULL, NULL);
        reserve_for_controls (content, width);
    } else {
        gtk_widget_remove_css_class (self->overlay, "gtkhx-pane-inline");
        if (content != NULL) {
            reserve_for_controls (content, 0);
        }
    }
}

static void
on_pane_enter (GtkEventControllerMotion *motion, double x, double y,
               gpointer user_data)
{
    HxPanel *self = HX_PANEL (user_data);

    (void)motion;
    (void)x;
    (void)y;
    self->hovering = TRUE;
    hx_panel_sync_chrome (self);
}

static void
on_pane_focus_enter (GtkEventControllerFocus *focus, gpointer user_data)
{
    HxPanel *self = HX_PANEL (user_data);

    (void)focus;
    self->focus_within = TRUE;
    hx_panel_sync_chrome (self);
}

static void
on_pane_focus_leave (GtkEventControllerFocus *focus, gpointer user_data)
{
    HxPanel *self = HX_PANEL (user_data);

    (void)focus;
    self->focus_within = FALSE;
    hx_panel_sync_chrome (self);
}

/* A connection switch shows another page, with its own corner widget. */
static void
on_page_switched (GObject *stack, GParamSpec *pspec, gpointer user_data)
{
    (void)stack;
    (void)pspec;
    hx_panel_sync_chrome (HX_PANEL (user_data));
}

static void
on_pane_leave (GtkEventControllerMotion *motion, gpointer user_data)
{
    HxPanel *self = HX_PANEL (user_data);

    (void)motion;
    self->hovering = FALSE;
    hx_panel_sync_chrome (self);
}

/* Opening the menu moves the pointer into its popover, which is a
 * leave as far as the pane is concerned. Controls shown on hover would
 * hide then, taking the menu with them before it could be used. */
static void
on_pane_menu_active (GObject *button, GParamSpec *pspec, gpointer user_data)
{
    HxPanel *self = HX_PANEL (user_data);

    (void)pspec;
    self->menu_open = gtk_menu_button_get_active (GTK_MENU_BUTTON (button));
    hx_panel_sync_chrome (self);
}

/* The pane's drag handle. It stands in for the one on the header this
 * pane no longer shows, and speaks the same language: the drag carries
 * the HxPanel as a PANEL_TYPE_WIDGET value, which is all the dock's drop
 * target (hx_panel_install_drop_target_on_dock) asks of a drag, and a
 * release over nothing undocks exactly as the header's handle does. */
static GdkContentProvider *
on_handle_prepare (GtkDragSource *src, double x, double y, gpointer user_data)
{
    (void)src;
    (void)x;
    (void)y;
    return gdk_content_provider_new_typed (PANEL_TYPE_WIDGET, user_data);
}

static void
on_handle_drag_begin (GtkDragSource *src, GdkDrag *drag, gpointer user_data)
{
    HxPanel *self = HX_PANEL (user_data);
    GtkWidget *chip;
    const char *title = panel_widget_get_title (PANEL_WIDGET (self));

    (void)src;
    self->dragging = TRUE;

    /* A chip naming the pane, rather than a snapshot of it: the drop
     * highlight already shows where it will land. */
    chip = gtk_label_new (title != NULL ? title : "");
    gtk_widget_add_css_class (chip, "gtkhx-pane-drag-chip");
    gtk_drag_icon_set_child (GTK_DRAG_ICON (gtk_drag_icon_get_for_drag (drag)),
                             chip);
}

static void
on_handle_drag_end (GtkDragSource *src, GdkDrag *drag, gboolean delete_data,
                    gpointer user_data)
{
    HxPanel *self = HX_PANEL (user_data);

    (void)src;
    (void)drag;
    (void)delete_data;
    self->dragging = FALSE;
    hx_panel_sync_chrome (self);
}

static GtkWidget *
build_drag_handle (HxPanel *self)
{
    GtkWidget *handle
        = gtk_image_new_from_icon_name ("list-drag-handle-symbolic");
    GtkDragSource *src = gtk_drag_source_new ();

    gtk_widget_add_css_class (handle, "gtkhx-pane-handle");
    gtk_widget_add_css_class (handle, "dim-label");
    gtk_widget_set_cursor_from_name (handle, "grab");
    gtk_widget_set_tooltip_text (handle, _ ("Drag to move this pane"));

    gtk_drag_source_set_actions (src, GDK_ACTION_MOVE | GDK_ACTION_COPY);
    g_signal_connect (src, "prepare", G_CALLBACK (on_handle_prepare), self);
    g_signal_connect (src, "drag-begin", G_CALLBACK (on_handle_drag_begin),
                      self);
    g_signal_connect (src, "drag-end", G_CALLBACK (on_handle_drag_end), self);
    g_signal_connect (src, "drag-cancel", G_CALLBACK (on_libpanel_drag_cancel),
                      NULL);
    gtk_widget_add_controller (handle, GTK_EVENT_CONTROLLER (src));
    return handle;
}

static GtkWidget *
build_pane_controls (HxPanel *self)
{
    GtkWidget *box, *menu_btn, *close_btn;
    GMenu *menu, *panel_section, *frame_section;

    /* The chevron menu again, addressed from inside the panel: "pane"
     * is the panel's own group (inserted on the overlay), frame-ops.*
     * the frame's group, found up the widget tree. No Move items —
     * the handle drags, and Alt+Shift+arrows move (hx_panel_frame.c). */
    panel_section = g_menu_new ();
    /* "Action Bar", not "Toolbar": the main menu's Show Toolbar is the
     * window's pixmap row, and one string for both reads as one switch. */
    g_menu_append (panel_section, _ ("Show Action Bar"), "pane.show-actions");
    g_menu_append (panel_section, _ ("Undock"), "pane.undock");
    frame_section = g_menu_new ();
    g_menu_append (frame_section, _ ("Split Horizontally"),
                   "frame-ops.split-h");
    g_menu_append (frame_section, _ ("Split Vertically"), "frame-ops.split-v");
    g_menu_append (frame_section, _ ("Close Frame"), "frame-ops.close-frame");
    menu = g_menu_new ();
    g_menu_append_section (menu, NULL, G_MENU_MODEL (panel_section));
    g_menu_append_section (menu, NULL, G_MENU_MODEL (frame_section));
    g_object_unref (panel_section);
    g_object_unref (frame_section);

    menu_btn = gtk_menu_button_new ();
    gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (menu_btn),
                                   "pan-down-symbolic");
    gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (menu_btn),
                                    G_MENU_MODEL (menu));
    gtk_widget_set_tooltip_text (menu_btn, _ ("Pane options"));
    gtk_accessible_update_property (GTK_ACCESSIBLE (menu_btn),
                                    GTK_ACCESSIBLE_PROPERTY_LABEL,
                                    _ ("Pane options"), -1);
    g_signal_connect (menu_btn, "notify::active",
                      G_CALLBACK (on_pane_menu_active), self);
    g_object_unref (menu);

    close_btn = gtk_button_new_from_icon_name ("window-close-symbolic");
    gtk_actionable_set_action_name (GTK_ACTIONABLE (close_btn),
                                    "frame-ops.close-page");
    gtk_widget_set_tooltip_text (close_btn, _ ("Close pane"));
    gtk_accessible_update_property (GTK_ACCESSIBLE (close_btn),
                                    GTK_ACCESSIBLE_PROPERTY_LABEL,
                                    _ ("Close pane"), -1);

    box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class (box, "gtkhx-pane-controls");
    gtk_widget_set_halign (box, GTK_ALIGN_END);
    gtk_widget_set_valign (box, GTK_ALIGN_START);
    gtk_box_append (GTK_BOX (box), menu_btn);
    gtk_box_append (GTK_BOX (box), close_btn);
    gtk_box_prepend (GTK_BOX (box), build_drag_handle (self));
    self->switcher = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class (self->switcher, "gtkhx-pane-switcher");
    gtk_widget_set_visible (self->switcher, FALSE);
    gtk_box_prepend (GTK_BOX (box), self->switcher);
    gtk_widget_set_visible (box, FALSE);
    return box;
}

static void
on_panel_map (GtkWidget *widget, gpointer user_data)
{
    (void)user_data;
    hx_panel_rebuild_switcher (HX_PANEL (widget));
    hx_panel_sync_chrome (HX_PANEL (widget));
}

void
hx_panel_set_content (HxPanel *self, GtkWidget *content)
{
    GtkEventController *motion, *focus;

    g_return_if_fail (HX_IS_PANEL (self));
    g_return_if_fail (self->overlay == NULL);

    self->overlay = gtk_overlay_new ();
    gtk_overlay_set_child (GTK_OVERLAY (self->overlay), content);
    self->controls = build_pane_controls (self);
    gtk_overlay_add_overlay (GTK_OVERLAY (self->overlay), self->controls);
    gtk_widget_insert_action_group (self->overlay, "pane",
                                    G_ACTION_GROUP (self->actions));

    motion = gtk_event_controller_motion_new ();
    g_signal_connect (motion, "enter", G_CALLBACK (on_pane_enter), self);
    g_signal_connect (motion, "leave", G_CALLBACK (on_pane_leave), self);
    gtk_widget_add_controller (self->overlay, motion);

    focus = gtk_event_controller_focus_new ();
    g_signal_connect (focus, "enter", G_CALLBACK (on_pane_focus_enter), self);
    g_signal_connect (focus, "leave", G_CALLBACK (on_pane_focus_leave), self);
    gtk_widget_add_controller (self->overlay, focus);

    if (GTK_IS_STACK (content)) {
        g_signal_connect_object (content, "notify::visible-child",
                                 G_CALLBACK (on_page_switched), self,
                                 G_CONNECT_DEFAULT);
    }

    panel_widget_set_child (PANEL_WIDGET (self), self->overlay);
}

GtkWidget *
hx_panel_get_content (HxPanel *self)
{
    g_return_val_if_fail (HX_IS_PANEL (self), NULL);

    if (self->overlay != NULL) {
        return gtk_overlay_get_child (GTK_OVERLAY (self->overlay));
    }
    return panel_widget_get_child (PANEL_WIDGET (self));
}

static void
frame_sync_header (PanelFrame *frame)
{
    PanelFrameHeader *header = panel_frame_get_header (frame);
    guint n = panel_frame_get_n_pages (frame);

    if (header != NULL) {
        gtk_widget_set_visible (GTK_WIDGET (header),
                                frame_needs_header (frame));
    }
    for (guint i = 0; i < n; i++) {
        PanelWidget *page = panel_frame_get_page (frame, i);
        if (page != NULL && HX_IS_PANEL (page)) {
            hx_panel_rebuild_switcher (HX_PANEL (page));
            hx_panel_sync_chrome (HX_PANEL (page));
        }
    }
}

static void
on_frame_pages_changed (GListModel *pages, guint position, guint removed,
                        guint added, gpointer user_data)
{
    (void)pages;
    (void)position;
    (void)removed;
    (void)added;
    frame_sync_header (PANEL_FRAME (user_data));
}

/* Drop the page-model connection before the frame tears its pages down:
 * disposal removes them, each removal emits items-changed, and the
 * handler would run on a frame half taken apart. */
static void
on_titled_frame_destroy (GtkWidget *frame, gpointer user_data)
{
    GObject *pages = g_object_get_data (G_OBJECT (frame), "hx-pane-titles");

    (void)user_data;
    if (pages != NULL) {
        g_signal_handlers_disconnect_by_data (pages, frame);
    }
}

void
hx_panel_install_pane_titles_on_frame (GtkWidget *frame)
{
    GtkSelectionModel *pages;

    g_return_if_fail (PANEL_IS_FRAME (frame));

    if (g_object_get_data (G_OBJECT (frame), "hx-pane-titles") != NULL) {
        return;
    }
    pages = panel_frame_get_pages (PANEL_FRAME (frame));
    g_signal_connect_object (pages, "items-changed",
                             G_CALLBACK (on_frame_pages_changed), frame,
                             G_CONNECT_DEFAULT);
    /* Holds the model — and so the connection — for the frame's life. */
    g_object_set_data_full (G_OBJECT (frame), "hx-pane-titles", pages,
                            g_object_unref);
    g_signal_connect (frame, "destroy", G_CALLBACK (on_titled_frame_destroy),
                      NULL);
    frame_sync_header (PANEL_FRAME (frame));
}

static void
resync_leaf_cb (HxSplit *leaf, gpointer user_data)
{
    PanelFrame *frame = hx_split_get_frame (leaf);

    (void)user_data;
    if (frame != NULL) {
        frame_sync_header (frame);
    }
}

void
hx_panel_resync_pane_titles (void)
{
    HxSplit *root = dock_layout_get_dock_root ();

    if (root != NULL) {
        hx_split_foreach_leaf (root, resync_leaf_cb, NULL);
    }
}

static void
on_show_actions_change (GSimpleAction *action, GVariant *value,
                        gpointer user_data)
{
    HxPanel *self = HX_PANEL (user_data);

    g_simple_action_set_state (action, value);
    hx_panel_sync_actions (self);
    dock_layout_set_panel_actions_hidden (self->id,
                                          !g_variant_get_boolean (value));
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
hx_panel_set_close_handler (HxPanel *self, HxPanelCloseFunc func,
                            gpointer user_data)
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
on_frame_page_closed (PanelFrame *frame, PanelWidget *page, gpointer user_data)
{
    HxPanel *self;
    const char *id;
    HxPanelCloseFunc cb;
    gpointer cb_data;

    (void)frame;
    (void)user_data;

    if (!HX_IS_PANEL (page)) {
        return;
    }

    /* Closing a panel changes which panels are in the dock, and that
     * is now persisted (the [Dock] closed= key). Requested for every
     * kind, before the dynamic-panel early return below, so a closed
     * static panel is still closed after a restart. */
    dock_layout_request_save ();

    self = HX_PANEL (page);
    if (self->kind != HX_PANEL_KIND_DYNAMIC) {
        return;
    }

    /* Snapshot the callback fields before the teardown runs — the
     * callback is allowed to set them to NULL (and may even
     * destroy the gchat/msgwin struct that close_data points at). */
    cb = self->close_func;
    cb_data = self->close_data;
    self->close_func = NULL;
    self->close_data = NULL;

    if (cb) {
        cb (self, cb_data);
    }

    /* Take the registry's last strong ref off. The panel widget
     * is already orphaned (page-closed runs post-detach); dropping
     * the registry ref hits refcount 0 → finalize. */
    id = self->id;
    if (id != NULL) {
        hx_panel_registry_unregister (id);
    }
}

void
hx_panel_install_close_dispatcher (GtkWidget *frame)
{
    g_return_if_fail (frame == NULL || PANEL_IS_FRAME (frame));
    if (frame == NULL) {
        return;
    }

    /* g_signal_connect is fine — frames live for the application's
     * lifetime (the toolbar dock owns them) so there's no
     * disconnect-on-frame-death case to worry about. */
    g_signal_connect (frame, "page-closed", G_CALLBACK (on_frame_page_closed),
                      NULL);
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

    if (root == NULL) {
        return NULL;
    }

    if (GTK_IS_BUTTON (root)) {
        const char *icon = gtk_button_get_icon_name (GTK_BUTTON (root));
        if (g_strcmp0 (icon, "list-drag-handle-symbolic") == 0) {
            return GTK_BUTTON (root);
        }
    }

    for (child = gtk_widget_get_first_child (root); child != NULL;
         child = gtk_widget_get_next_sibling (child)) {
        GtkButton *found = find_drag_button (child);
        if (found != NULL) {
            return found;
        }
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
    if (controllers == NULL) {
        return NULL;
    }

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
on_libpanel_drag_cancel (GtkDragSource *src, GdkDrag *drag,
                         GdkDragCancelReason reason, gpointer user_data)
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
    if (reason == GDK_DRAG_CANCEL_USER_CANCELLED) {
        return FALSE;
    }

    /* libpanel installs the content via the GtkDragSource::prepare
     * signal's return value (not gtk_drag_source_set_content), so
     * gtk_drag_source_get_content returns NULL. The actual content
     * lives on the GdkDrag for the duration of the operation. */
    cp = gdk_drag_get_content (drag);
    if (cp == NULL) {
        return FALSE;
    }

    g_value_init (&value, PANEL_TYPE_WIDGET);
    if (gdk_content_provider_get_value (cp, &value, NULL)) {
        GObject *obj = g_value_get_object (&value);
        if (obj != NULL && HX_IS_PANEL (obj)) {
            hx_panel_undock (HX_PANEL (obj));
        }
    }
    g_value_unset (&value);
    return FALSE;
}

void
hx_panel_install_drag_out_on_frame (GtkWidget *frame)
{
    GtkButton *btn;
    GtkDragSource *src;

    g_return_if_fail (frame == NULL || PANEL_IS_FRAME (frame));
    if (frame == NULL) {
        return;
    }

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

    g_signal_connect (src, "drag-cancel", G_CALLBACK (on_libpanel_drag_cancel),
                      NULL);

    /* libpanel doesn't set actions on its drag source, leaving it at
     * the default (0). With actions=0 the drag-drop action
     * negotiation fails everywhere — no drop target can ever accept,
     * which is what was making in-dock drag-between-frames look
     * impossible. Set MOVE | COPY explicitly so drop targets along
     * the path have something to negotiate against. */
    gtk_drag_source_set_actions (src, GDK_ACTION_MOVE | GDK_ACTION_COPY);

    debug_log ("dnd", "installed drag-out on frame=%p (drag_source=%p)", frame,
               src);
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
                                           gpointer user_data);

static gboolean
on_frame_drop (GtkDropTarget *target, const GValue *value, double x, double y,
               gpointer user_data)
{
    PanelFrame *target_frame = PANEL_FRAME (user_data);
    GObject *obj;
    HxPanel *panel;
    GtkWidget *src_frame;
    GtkWidget *src_dock;
    GtkWidget *target_dock;
    GtkRoot *src_root = NULL;
    gboolean was_cross_dock = FALSE;

    (void)target;
    (void)x;
    (void)y;

    debug_log ("dnd", "frame_drop: value type=%s, target_frame=%p",
               G_VALUE_TYPE_NAME (value), target_frame);

    if (!G_VALUE_HOLDS (value, PANEL_TYPE_WIDGET)) {
        return FALSE;
    }
    obj = g_value_get_object (value);
    if (obj == NULL || !HX_IS_PANEL (obj)) {
        return FALSE;
    }
    panel = HX_PANEL (obj);

    src_frame = gtk_widget_get_ancestor (GTK_WIDGET (panel), PANEL_TYPE_FRAME);
    src_dock = gtk_widget_get_ancestor (GTK_WIDGET (panel), PANEL_TYPE_DOCK);
    target_dock
        = gtk_widget_get_ancestor (GTK_WIDGET (target_frame), PANEL_TYPE_DOCK);

    /* No-op when dropping onto the panel's current frame. */
    if (src_frame == (GtkWidget *)target_frame) {
        return FALSE;
    }

    /* Was this a cross-dock drag? If so we'll close the source
     * window once the panel has moved. */
    if (src_dock != NULL && target_dock != NULL && src_dock != target_dock) {
        was_cross_dock = TRUE;
        src_root = gtk_widget_get_root (src_dock);
    }

    g_object_ref (panel);
    if (src_frame != NULL) {
        panel_frame_remove (PANEL_FRAME (src_frame), PANEL_WIDGET (panel));
    }

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
        PanelArea new_area;
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
        if (panel_area_for_default_frame (tf, &new_area)) {
            panel->home_area = new_area;
        }
        hx_panel_set_home_frame (panel, tf);
    }
    g_object_unref (panel);

    /* DnD between main-dock frames is a placement change; persist
     * the new layout. No-op on cross-dock drops since the panel
     * left the main dock — the destination is an undocked window
     * whose state isn't part of the saved layout (yet). */
    if (target_dock == toolbar_dock) {
        dock_layout_request_save ();
    }

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
            gulong handler_id = (gulong)GPOINTER_TO_SIZE (g_object_get_data (
                G_OBJECT (src_root), "hx-undocked-close-handler-id"));
            if (handler_id != 0) {
                g_signal_handler_disconnect (src_root, handler_id);
                g_object_set_data (G_OBJECT (src_root),
                                   "hx-undocked-close-handler-id", NULL);
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

    if (root == NULL) {
        return;
    }

    if (g_strcmp0 (G_OBJECT_TYPE_NAME (root), "PanelDropControls") == 0) {
        gtk_widget_set_can_target (root, FALSE);
        return;
    }

    for (child = gtk_widget_get_first_child (root); child != NULL;
         child = gtk_widget_get_next_sibling (child)) {
        defang_drop_controls (child);
    }
}

void
hx_panel_defang_drop_controls_on_frame (GtkWidget *frame)
{
    g_return_if_fail (frame == NULL || PANEL_IS_FRAME (frame));
    if (frame == NULL) {
        return;
    }

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
    if (root == NULL) {
        return;
    }
    if (PANEL_IS_FRAME (root)) {
        g_ptr_array_add (out, root);
    }
    for (child = gtk_widget_get_first_child (root); child != NULL;
         child = gtk_widget_get_next_sibling (child)) {
        collect_frames (child, out);
    }
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
        if (gtk_widget_compute_point (dock, frame, &pt, &out_pt)
            && gtk_widget_compute_bounds (frame, frame, &bounds)
            && graphene_rect_contains_point (&bounds, &out_pt)) {
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

/* --- Drop highlight ---------------------------------------------- */

/* Class on the PanelFrame the pointer is currently over. */
#define HX_DROP_TARGET_CLASS "hx-drop-target"
/* Class on the dock hosting our drop target, so the CSS below can
 * turn off the outline GTK would otherwise draw around it. */
#define HX_DOCK_DROP_HOST_CLASS "hx-dock-drop-host"

/* Weak pointer: the highlighted frame can be destroyed mid-drag (a
 * cross-dock drag that empties an undocked window, say), and a raw
 * pointer would dangle until the next motion event cleared it. */
static GtkWidget *drop_highlight_frame;

static void
set_drop_highlight (GtkWidget *frame)
{
    if (drop_highlight_frame == frame) {
        return;
    }
    if (drop_highlight_frame != NULL) {
        gtk_widget_remove_css_class (drop_highlight_frame,
                                     HX_DROP_TARGET_CLASS);
        g_clear_weak_pointer (&drop_highlight_frame);
    }
    if (frame != NULL) {
        gtk_widget_add_css_class (frame, HX_DROP_TARGET_CLASS);
        g_set_weak_pointer (&drop_highlight_frame, frame);
    }
}

/* Install the drop-feedback stylesheet once, at
 * GTK_STYLE_PROVIDER_PRIORITY_APPLICATION so both rules beat the
 * theme's.
 *
 * `outline` rather than `box-shadow: inset` for the frame, and the
 * distinction matters: GtkWidget snapshots background and border
 * BEFORE its children and the outline AFTER, so an inset box-shadow
 * is painted over by whatever content fills the pane, while the
 * outline lands on top. A negative outline-offset keeps it inside
 * the frame's own allocation instead of bleeding onto the
 * neighbouring pane.
 *
 * @accent_bg_color rather than var(--accent-bg-color): the named
 * color works across the whole supported libadwaita range, and CSS
 * custom properties do not reach back to our floor. */
static void
ensure_drop_css (void)
{
    static GtkCssProvider *provider = NULL;
    GdkDisplay *display;

    if (provider != NULL) {
        return;
    }
    display = gdk_display_get_default ();
    if (display == NULL) {
        return;
    }

    provider = gtk_css_provider_new ();
    gtk_css_provider_load_from_string (
        provider,
        /* GTK's default stylesheet outlines ANY widget that has an
         * active drop target under the pointer
         * (":not(window):drop(active)"). Ours is a single target on
         * the dock, which fills the window — so the whole window lit
         * up during a drag and gave the user no clue which pane the
         * panel would land in. Suppress it there; the hit-tested
         * frame gets the highlight instead.
         *
         * All three properties that rule sets, not just box-shadow.
         * The other two are inert on a PanelDock today (no border
         * width, no caret) but would resurface the moment libpanel
         * gave the dock a border, and a suppression rule that only
         * half-suppresses is a trap. */
        "paneldock." HX_DOCK_DROP_HOST_CLASS ":drop(active) {"
        "  box-shadow: none;"
        "  border-color: inherit;"
        "  caret-color: inherit;"
        "}"
        "panelframe." HX_DROP_TARGET_CLASS " {"
        "  outline: 3px solid @accent_bg_color;"
        "  outline-offset: -3px;"
        "}");
    gtk_style_context_add_provider_for_display (
        display, GTK_STYLE_PROVIDER (provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

static gboolean
on_dock_drop (GtkDropTarget *target, const GValue *value, double x, double y,
              gpointer user_data)
{
    GtkWidget *dock = GTK_WIDGET (user_data);
    PanelFrame *target_frame;
    GValue val_copy = G_VALUE_INIT;
    gboolean ret;

    debug_log ("dnd", "dock_drop: x=%g y=%g, value type=%s", x, y,
               G_VALUE_TYPE_NAME (value));

    /* The drag is over either way — clear the highlight before any
     * of the early returns below can skip it. */
    set_drop_highlight (NULL);

    if (!G_VALUE_HOLDS (value, PANEL_TYPE_WIDGET)) {
        return FALSE;
    }

    target_frame = frame_at_dock_coords (dock, x, y);
    debug_log ("dnd", "dock_drop: target_frame=%p", target_frame);
    if (target_frame == NULL) {
        return FALSE;
    }

    /* Reuse on_frame_drop's logic by calling it directly with the
     * target frame as user_data. We have to copy the value because
     * GtkDropTarget gives us a borrowed const GValue. */
    g_value_init (&val_copy, G_VALUE_TYPE (value));
    g_value_copy (value, &val_copy);
    ret = on_frame_drop (target, &val_copy, x, y, target_frame);
    g_value_unset (&val_copy);
    return ret;
}

/* enter and motion share a body: light up whichever frame the
 * pointer is over, using the same hit-test the drop itself uses so
 * the highlight can never point at a pane other than the one that
 * will receive the panel. */
static GdkDragAction
on_dock_enter (GtkDropTarget *target, double x, double y, gpointer user_data)
{
    GtkWidget *dock = GTK_WIDGET (user_data);
    PanelFrame *frame = frame_at_dock_coords (dock, x, y);

    (void)target;
    set_drop_highlight (frame != NULL ? GTK_WIDGET (frame) : NULL);
    return GDK_ACTION_MOVE;
}

static GdkDragAction
on_dock_motion (GtkDropTarget *target, double x, double y, gpointer user_data)
{
    GtkWidget *dock = GTK_WIDGET (user_data);
    PanelFrame *frame = frame_at_dock_coords (dock, x, y);

    (void)target;
    set_drop_highlight (frame != NULL ? GTK_WIDGET (frame) : NULL);
    return GDK_ACTION_MOVE;
}

/* Leave fires when the pointer exits the dock and when the drag ends
 * anywhere else — including a cancelled drag, which is the path that
 * would otherwise leave a pane outlined with nothing in flight. */
static void
on_dock_leave (GtkDropTarget *target, gpointer user_data)
{
    (void)target;
    (void)user_data;
    set_drop_highlight (NULL);
}

void
hx_panel_install_drop_target_on_dock (GtkWidget *dock)
{
    GtkDropTarget *target;
    GType types[] = { PANEL_TYPE_WIDGET, GTK_TYPE_WIDGET, G_TYPE_OBJECT };

    g_return_if_fail (dock == NULL || PANEL_IS_DOCK (dock));
    if (dock == NULL) {
        return;
    }

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
    g_signal_connect (target, "drop", G_CALLBACK (on_dock_drop), dock);
    g_signal_connect (target, "enter", G_CALLBACK (on_dock_enter), dock);
    g_signal_connect (target, "motion", G_CALLBACK (on_dock_motion), dock);
    g_signal_connect (target, "leave", G_CALLBACK (on_dock_leave), dock);
    gtk_widget_add_controller (dock, GTK_EVENT_CONTROLLER (target));

    /* Per-pane drop feedback: the class marks this dock as the one
     * whose whole-widget :drop(active) outline should be suppressed,
     * and the stylesheet paints the hit-tested frame instead. */
    gtk_widget_add_css_class (dock, HX_DOCK_DROP_HOST_CLASS);
    ensure_drop_css ();

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
    frame_anc = gtk_widget_get_ancestor (GTK_WIDGET (self), PANEL_TYPE_FRAME);
    if (frame_anc != NULL) {
        return;
    }

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

    /* prefer the panel's existing home_frame
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
        if (home != NULL) {
            g_object_unref (home); /* get_home_frame strong ref */
        }

        if (!home_usable) {
            /* Fall back to home_area default. Only update the
             * stored home_frame in this branch — when home_frame
             * was already usable, leave it untouched so the user's
             * choice persists. */
            switch (self->home_area) {
            case PANEL_AREA_START:
                target = toolbar_sidebar_frame;
                break;
            case PANEL_AREA_END:
                target = toolbar_end_frame;
                break;
            case PANEL_AREA_BOTTOM:
                target = toolbar_bottom_frame;
                break;
            case PANEL_AREA_CENTER:
            default:
                target = toolbar_center_frame;
                break;
            }
            if (target == NULL) {
                return;
            }
            panel_frame_add (PANEL_FRAME (target), PANEL_WIDGET (self));
            hx_panel_set_home_frame (self, target);
            /* Re-opening a closed panel puts it back in the dock —
             * the mirror of the page-closed save above. */
            dock_layout_request_save ();
            return;
        }
    }

    panel_frame_add (PANEL_FRAME (target), PANEL_WIDGET (self));
    /* home_frame already points here — don't reset. */
    dock_layout_request_save ();
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
    if (obj == NULL) {
        return NULL;
    }
    return GTK_WIDGET (obj);
}

/* close-request on the undocked window: pull the panel back to its
 * home frame, re-reveal a collapsed side area, then let the window
 * finish closing. Same pattern as the spike. */
static gboolean
on_undocked_close_request (GtkWindow *window, gpointer user_data)
{
    HxPanel *self = HX_PANEL (user_data);
    GtkWidget *home;
    GtkWidget *parent_frame;

    g_object_ref (self);

    home = hx_panel_get_home_frame (self);
    parent_frame
        = gtk_widget_get_ancestor (GTK_WIDGET (self), PANEL_TYPE_FRAME);
    if (parent_frame != NULL) {
        panel_frame_remove (PANEL_FRAME (parent_frame), PANEL_WIDGET (self));
    }

    if (home != NULL) {
        panel_frame_add (PANEL_FRAME (home), PANEL_WIDGET (self));
        /* no more PanelDock revealers; the
         * tree just shows everything that's in it. No area
         * reveal flip needed on Redock. */
        g_object_unref (home); /* hx_panel_get_home_frame strong ref */
    }

    /* Redock changes which main-dock leaf the panel lives in. */
    dock_layout_request_save ();

    g_object_unref (self);
    return FALSE; /* let the window close */
}

/* notify::default-width / notify::default-height handler attached
 * to every undocked window. Resize fires this; we just request a
 * debounced save so the [Undocked] section captures the new size. */
static void
on_undocked_window_size_notify (GObject *object, GParamSpec *pspec,
                                gpointer data)
{
    (void)object;
    (void)pspec;
    (void)data;
    dock_layout_request_save ();
}

void
hx_panel_undock (HxPanel *self)
{
    GtkWidget *current_frame;
    GtkBuilder *builder;
    GtkBuilderScope *scope;
    GtkWindow *window;
    PanelGrid *grid;
    GApplication *app;
    GError *err = NULL;
    const char *title;

    /* Same XML the spike uses — minimal AdwApplicationWindow with a
     * PanelDock whose center is a PanelGrid. Built via GtkBuilder so
     * libpanel's GtkBuildable add-child wraps the grid in the dock's
     * CENTER area correctly (gtk_widget_set_parent would skip that). */
    static const char *undocked_ui_xml
        = "<interface>"
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

    current_frame
        = gtk_widget_get_ancestor (GTK_WIDGET (self), PANEL_TYPE_FRAME);
    if (current_frame == NULL) {
        // g_warning ("Undock: panel %s has no ancestor frame", self->id);
        g_object_unref (self);
        return;
    }

    /* if the panel is already in an undocked
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
            if (GTK_IS_WINDOW (root)) {
                gtk_window_close (GTK_WINDOW (root));
            }
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
    grid = PANEL_GRID (gtk_builder_get_object (builder, "grid"));

    app = g_application_get_default ();
    if (app != NULL) {
        gtk_window_set_application (window, GTK_APPLICATION (app));
    }

    title = panel_widget_get_title (PANEL_WIDGET (self));
    gtk_window_set_title (window, title ? title : _ ("Undocked panel"));

    panel_grid_add (grid, PANEL_WIDGET (self));

    {
        gulong handler_id
            = g_signal_connect (window, "close-request",
                                G_CALLBACK (on_undocked_close_request), self);
        /* Stash the handler id so the cross-dock-drop path
         * (on_frame_drop) can disconnect it by id rather than by
         * (function + user_data). disconnect-by-func only matches
         * when the user_data the close-request was connected with
         * is the panel currently being moved — false whenever the
         * user has stacked multiple panels into the same undocked
         * window. */
        g_object_set_data (G_OBJECT (window), "hx-undocked-close-handler-id",
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
on_undock_activate (GSimpleAction *action, GVariant *parameter,
                    gpointer user_data)
{
    (void)action;
    (void)parameter;
    hx_panel_undock (HX_PANEL (user_data));
}

GtkWidget *
hx_panel_find_css_class_descendant (GtkWidget *root, const char *css_class)
{
    GtkWidget *child;

    if (root == NULL) {
        return NULL;
    }
    if (gtk_widget_has_css_class (root, css_class)) {
        return root;
    }
    for (child = gtk_widget_get_first_child (root); child != NULL;
         child = gtk_widget_get_next_sibling (child)) {
        GtkWidget *hit = hx_panel_find_css_class_descendant (child, css_class);
        if (hit != NULL) {
            return hit;
        }
    }
    return NULL;
}

/* The undocked window's grid needs a create-frame handler. Uses a
 * plain PanelFrame — NOT HxPanelFrame. HxPanelFrame exists to
 * repurpose the chevron's page.move-{left,right,up,down} onto the
 * main dock's HxSplit tree, and an undocked window has no HxSplit
 * ancestor, so subclassing here would buy nothing but four actions
 * that can never be enabled.
 *
 * (This used to be argued the other way round — that libpanel's own
 * handlers reflow pages between PanelGrid cells and are worth
 * keeping. The chevron they live behind is hidden below, so nothing
 * reaches them either way; the reason above is the one that still
 * holds.)
 *
 * The other HxPanel hookups still apply — close-dispatcher
 * (DYNAMIC panel teardown + registry unregister), drag-out
 * (drag onto desktop → spawn yet another window) and drop-
 * controls defang — they're frame-level concerns, independent
 * of the HxSplit world. */
static PanelFrame *
hx_panel_undocked_create_frame (PanelGrid *grid, gpointer user_data)
{
    GtkWidget *frame = panel_frame_new ();
    PanelFrameHeader *header
        = PANEL_FRAME_HEADER (panel_frame_header_bar_new ());
    GtkWidget *controls;

    (void)grid;
    (void)user_data;
    panel_frame_set_header (PANEL_FRAME (frame), header);
    hx_panel_install_close_dispatcher (frame);
    hx_panel_install_drag_out_on_frame (frame);
    hx_panel_defang_drop_controls_on_frame (frame);

    /* An undocked window is a LEAF: it holds the one panel that was
     * dragged or undocked into it and nothing docks into it. The
     * dock-level GtkDropTarget lives on toolbar_dock alone, so a
     * drop here has never been accepted — but libpanel's header
     * still advertises the machinery for it, which reads as "this
     * should work" and then doesn't.
     *
     * Hide the ".controls" box, which is the pan-down chevron (Move
     * Page L/R/U/D + our Undock item) and the window-close X
     * together. Both are dock operations with no meaning in a
     * one-panel window: Move Page has nowhere to move to, and the X
     * would close the page and leave an empty window behind rather
     * than redocking.
     *
     * The drag handle and the title button stay, so the way back
     * into the dock — drag the panel over a pane, or just close the
     * window, which redocks via on_undocked_close_request — is
     * unchanged. */
    controls
        = hx_panel_find_css_class_descendant (GTK_WIDGET (header), "controls");
    if (controls != NULL) {
        gtk_widget_set_visible (controls, FALSE);
    } else {
        g_warning ("HxPanel: no .controls box on the undocked frame's "
                   "header — libpanel's template changed; the chevron "
                   "and close button will still be shown");
    }

    return PANEL_FRAME (frame);
}

/* on_move_area_activate retired — panel.move-area gave way to
 * the four relative-direction actions (panel.move-left / right /
 * up / down) plus split / close-frame. See on_move_direction_
 * activate below. */

/* ----------------------------------------------------------------- */
/* relative move + split + close-frame          */
/* ----------------------------------------------------------------- */

/* panel_neighbor_across_areas removed. The
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
    GtkWidget *anc = gtk_widget_get_ancestor (GTK_WIDGET (self), HX_TYPE_SPLIT);
    if (anc == NULL) {
        return NULL;
    }
    if (!hx_split_is_leaf (HX_SPLIT (anc))) {
        /* HxSplit ancestor is an internal split — find the leaf
         * by walking from the panel's PanelFrame ancestor to the
         * matching leaf within `anc`'s tree. */
        GtkWidget *frame_anc
            = gtk_widget_get_ancestor (GTK_WIDGET (self), PANEL_TYPE_FRAME);
        if (frame_anc == NULL) {
            return NULL;
        }
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
    HxSplit *leaf;
    HxSplit *target_leaf;
    GtkWidget *current_frame;
    PanelFrame *target_frame;

    g_return_if_fail (HX_IS_PANEL (self));

    leaf = panel_get_split_leaf (self);
    if (leaf == NULL) {
        debug_log ("dock", "hx_panel %s: page.move-* — no HxSplit ancestor",
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
    if (target_frame == NULL) {
        return;
    }

    current_frame
        = gtk_widget_get_ancestor (GTK_WIDGET (self), PANEL_TYPE_FRAME);
    if (current_frame == (GtkWidget *)target_frame) {
        return; /* shouldn't happen — neighbour by definition is elsewhere */
    }

    g_object_ref (self);
    if (current_frame != NULL) {
        panel_frame_remove (PANEL_FRAME (current_frame), PANEL_WIDGET (self));
    }
    panel_frame_add (target_frame, PANEL_WIDGET (self));
    {
        GtkWidget *tf = GTK_WIDGET (target_frame);
        PanelArea new_area;
        /* home_frame tracks the exact leaf the user chose.
         * home_area only follows along when the destination is one
         * of the four default leaves; a move into a user-created
         * split leaf preserves the panel's original area intent so
         * that — if that custom leaf is later closed while the
         * panel is detached — the home_area fallback in
         * hx_panel_ensure_attached returns the panel to its
         * original default frame rather than coercing it onto
         * whatever area sentinel the custom leaf might map to. */
        if (panel_area_for_default_frame (tf, &new_area)) {
            self->home_area = new_area;
        }
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
    if (leaf == NULL) {
        return FALSE;
    }
    return hx_split_neighbor (leaf, dir) != NULL;
}

/* Per-panel split / close-frame handlers retired — those
 * operations now live on each PanelFrame's header via
 * hx_split_install_frame_ui. Per-panel chevron stays focused on
 * undock; libpanel's chevron items Move Page L/R/U/D are routed
 * to hx_panel_do_move_in_direction by HxPanelFrame's overriding
 * class actions (src/hx_panel_frame.c). */
