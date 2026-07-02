/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * hx_panel.h — HxPanel : PanelWidget subclass.
 *
 * Phase 1 / docking. The base unit of GtkHx's dockable UI. Each of
 * the dockable tool windows (chat / news / news 1.5 / users /
 * tasks / files) becomes one HxPanel in Phase 2; the dynamic
 * per-chat / per-PM windows become HxPanels in Phase 3. (The
 * Tracker is deliberately NOT docked — it is a standalone top-level
 * window and has no HxPanel.)
 *
 * Compared to a raw PanelWidget, HxPanel adds:
 *
 *   - A stable string id ("chat", "users", "tracker", "msg-<uid>" …)
 *     so the panel registry (panel_registry.h) can index it and so
 *     that future per-connection layout persistence in Phase 4 can
 *     refer to panels by name, not by widget pointer.
 *
 *   - A kind tag (sidebar / center / dynamic) so the registry knows
 *     which dock area to home the panel to on Redock and which
 *     PanelToggleButton should track its visibility.
 *
 *   - A "home area" hint that survives detach-to-window — the
 *     spike's spike-home GObject-data pointer graduates here as a
 *     real instance field.
 *
 * Content is set with panel_widget_set_child() inherited from the
 * base class; HxPanel does not own the relocated headerbar widgets,
 * the per-window Phase 2 migrations do that.
 */

#ifndef GTKHX_HX_PANEL_H
#define GTKHX_HX_PANEL_H 1

#include <gtk/gtk.h>
#include <libpanel.h>

G_BEGIN_DECLS

#define HX_TYPE_PANEL (hx_panel_get_type ())
G_DECLARE_FINAL_TYPE (HxPanel, hx_panel, HX, PANEL, PanelWidget)

/* Where the panel lives by default in the dock. The HxPanelKind
 * is used by the panel registry on Redock (and Phase 4 layout
 * restore) to pick the correct PanelDock area. CENTER panels go in
 * the central PanelGrid; SIDEBAR panels go in the start/end/etc
 * PanelFrame matching home_area. DYNAMIC panels (per-pchat, per-PM)
 * default to a dedicated "conversations" frame the registry sets
 * up in Phase 3.
 *
 * The split was driven by the Phase 0 finding that the libpanel
 * center area is a PanelGrid (not a PanelFrame) and is not wrapped
 * in a GtkRevealer — but each side area is, and an empty side area
 * auto-collapses (see panel_dock_notify_empty_cb). The registry
 * needs to know which case it is dealing with to flip the
 * revealer back on after Redock. */
typedef enum {
    HX_PANEL_KIND_CENTER,    /* chat, news 1.5 (news browser), files */
    HX_PANEL_KIND_SIDEBAR,   /* users, tasks, news */
    HX_PANEL_KIND_DYNAMIC,   /* private chats, private messages */
} HxPanelKind;

HxPanel    *hx_panel_new           (const char  *id,
                                    HxPanelKind  kind,
                                    PanelArea    home_area);

const char *hx_panel_get_id        (HxPanel     *self);
HxPanelKind hx_panel_get_kind      (HxPanel     *self);
PanelArea   hx_panel_get_home_area (HxPanel     *self);

/* Set / get the PanelFrame the panel should return to on Redock
 * (i.e., when the undocked window closes). Called by the per-panel
 * factory right after panel_frame_add. Stored as a GWeakRef so a
 * disposed home frame doesn't dangle.
 *
 * hx_panel_get_home_frame returns a STRONG REFERENCE — the caller
 * must g_object_unref the returned widget when done. (The underlying
 * GWeakRef must promote to a strong ref to safely hand the pointer
 * out; returning a "borrowed" pointer would dangle if the weak ref
 * was the last live reference.) Returns NULL if the home frame has
 * been disposed. */
void        hx_panel_set_home_frame (HxPanel    *self,
                                     GtkWidget  *frame);
GtkWidget  *hx_panel_get_home_frame (HxPanel    *self);

/* Re-attach a panel that's been detached from the dock (e.g. via
 * the frame chevron menu's "Close all pages" or a per-tab close).
 * The panel registry keeps a strong ref on every registered HxPanel
 * for exactly this reason — the widget survives the close even when
 * the dock thinks it's gone, and a subsequent toolbar-button click
 * (or open_* entry point) calls this to splice it back into its
 * home area before raising. No-op when the panel is already in a
 * frame.
 *
 * The destination is the panel's recorded home_frame when its
 * weak ref still resolves to a live PanelFrame in the dock tree —
 * which is the common case once the user has moved the panel into
 * a custom split leaf via Move-direction or DnD. Phase 5b /
 * docking deliberately preserves that placement across Close all
 * pages → re-show so that user-arranged layouts survive the round
 * trip. Only when home_frame has expired (e.g. the leaf was
 * collapsed) does this fall back to the home_area default — the
 * matching toolbar_*_frame for that area (Phase 5b dropped the
 * separate PanelGrid in favor of a single HxSplit root, so even
 * CENTER resolves to a PanelFrame — toolbar_center_frame —
 * rather than a grid). The stored home_frame is updated to that
 * fallback target. */
void        hx_panel_ensure_attached (HxPanel   *self);

/* Per-panel close callback for DYNAMIC kind panels. Phase 3.
 *
 * Static panels (CENTER / SIDEBAR) survive Close all pages by
 * design — the registry's strong ref keeps them alive and a
 * subsequent toolbar click re-attaches them. Dynamic panels
 * (private chats, private messages) should instead tear down
 * their backing state (gchat / msgwin struct, model-side
 * hashtable entry, server-side hx_part_chat) and unregister so
 * the panel finalizes.
 *
 * The frame-level dispatcher (hx_panel_dispatch_frame_close,
 * below) calls this callback when it sees a DYNAMIC panel closing.
 * It then calls hx_panel_registry_unregister on the panel's id.
 * The callback runs before the unregister, so it can safely
 * read the panel's id and any data set via g_object_set_data. */
typedef void (*HxPanelCloseFunc) (HxPanel *panel, gpointer user_data);

void        hx_panel_set_close_handler (HxPanel          *self,
                                        HxPanelCloseFunc  func,
                                        gpointer          user_data);

/* Move the panel into the leaf adjacent to its current frame in
 * the given direction (across the HxSplit tree, found via
 * hx_split_neighbor). No-op when there's no neighbour. Used by
 * HxPanelFrame's page.move-* class-action handlers so the chevron
 * "Move Page L/R/U/D" items perform a cross-frame move. */
void        hx_panel_do_move_in_direction (HxPanel          *self,
                                           GtkDirectionType  dir);

/* Returns TRUE iff hx_panel_do_move_in_direction(self, dir) would
 * actually move the panel — i.e. there is a neighbour leaf in
 * that direction in the dock's HxSplit tree. Used to gate the
 * enabled state of the chevron's per-direction Move Page items so
 * a no-op direction renders greyed instead of clickable-but-inert. */
gboolean    hx_panel_can_move_in_direction (HxPanel          *self,
                                            GtkDirectionType  dir);

/* Hook the dispatcher onto a PanelFrame's page-closed signal.
 * Called once per frame at toolbar build time and from the
 * grid's create-frame handler. The dispatcher runs the close
 * callback + unregisters dynamic panels; static panels pass
 * through untouched (their close is just a detach — the
 * registry keeps them alive for re-attach). */
void        hx_panel_install_close_dispatcher (GtkWidget *frame);

/* Pop the panel out of its current frame into a fresh top-level
 * AdwApplicationWindow + PanelDock + PanelGrid. The window's
 * close-request handler routes the panel back to its home frame
 * (Redock). Phase 4 / docking — used by both the panel.undock
 * GAction (chevron menu) and the drag-out detector. No-op if the
 * panel isn't currently in a frame ancestor. */
void        hx_panel_undock (HxPanel *self);

/* drag-out detection. libpanel's drag system
 * lives on a "grab" button inside each PanelFrame's header bar
 * (PanelFrameHeaderBar). The drag produces a PANEL_TYPE_WIDGET
 * content; libpanel's drop targets accept drops only inside an
 * existing PanelDock. To support drag-out-spawns-a-new-window,
 * we connect to the drag source's "drag-cancel" signal: when the
 * cancellation reason is GDK_DRAG_CANCEL_NO_TARGET (user
 * released the drag outside any drop target), we run hx_panel_undock
 * on the dragged panel. Call once per frame at toolbar-build /
 * frame-create time. */
void        hx_panel_install_drag_out_on_frame (GtkWidget *frame);

/* disable libpanel's PanelDropControls overlay
 * inside the frame so it doesn't claim drop events. The overlay is
 * meant to handle in-dock drops but it stays hidden / non-functional
 * in our setup, and while it's the deepest-hit drop target it
 * silently consumes events that should reach the dock-level drop
 * target (hx_panel_install_drop_target_on_dock).
 *
 * Call once per frame at toolbar-build / frame-create time. */
void        hx_panel_defang_drop_controls_on_frame (GtkWidget *frame);

/* dock-level drop target. When per-frame drop
 * targets don't receive events (libpanel internals consume them
 * before they bubble up), install one at the dock level. The
 * handler picks the target frame by hit-testing the drop
 * coordinates against the dock's child frames. */
void        hx_panel_install_drop_target_on_dock (GtkWidget *dock);

G_END_DECLS

#endif /* GTKHX_HX_PANEL_H */
