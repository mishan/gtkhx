/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
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
 * dock_bridge.c — see dock_bridge.h. Wraps the libpanel plumbing every
 * old create_X_window did by hand (hx_panel_new / set title+icon+child /
 * panel_frame_add / set_home_frame / registry register) behind three
 * type-free entry points so the gtk4-rs window ports never touch a
 * libpanel type.
 */

#include "config.h"

#include <glib.h>
#include <libpanel.h>

#include "hx.h"            /* session (toolbar.h's prototypes reference it) */
#include "hx_panel.h"
#include "panel_registry.h"
#include "toolbar.h"        /* toolbar_*_frame globals */
#include "dock_bridge.h"

/* Map a bridge area to its libpanel PanelArea + home PanelFrame. The
 * area→frame pairing lived, duplicated, in every create_X_window; it now
 * lives here exactly once. Returns NULL for `*frame_out` (and logs) when
 * the dock hasn't been built, which the callers treat as fatal-ish. */
static PanelArea
dock_area_to_panel_area (GtkhxDockArea area, GtkWidget **frame_out)
{
    switch (area) {
    case GTKHX_DOCK_AREA_START:
        *frame_out = toolbar_sidebar_frame;
        return PANEL_AREA_START;
    case GTKHX_DOCK_AREA_END:
        *frame_out = toolbar_end_frame;
        return PANEL_AREA_END;
    case GTKHX_DOCK_AREA_BOTTOM:
        *frame_out = toolbar_bottom_frame;
        return PANEL_AREA_BOTTOM;
    case GTKHX_DOCK_AREA_CENTER:
    default:
        *frame_out = toolbar_center_frame;
        return PANEL_AREA_CENTER;
    }
}

static HxPanelKind
dock_kind_to_panel_kind (GtkhxDockKind kind)
{
    switch (kind) {
    case GTKHX_DOCK_KIND_CENTER:  return HX_PANEL_KIND_CENTER;
    case GTKHX_DOCK_KIND_SIDEBAR: return HX_PANEL_KIND_SIDEBAR;
    case GTKHX_DOCK_KIND_DYNAMIC:
    default:                      return HX_PANEL_KIND_DYNAMIC;
    }
}

gboolean
gtkhx_dock_raise_if_open (const char *id)
{
    HxPanel *panel;

    g_return_val_if_fail (id != NULL, FALSE);

    panel = hx_panel_registry_lookup (id);
    if (panel == NULL) {
        return FALSE;
    }

    /* The panel may have been detached by a Close-all-pages on its
     * frame; the registry kept it alive. Splice it back into its home
     * area (no-op if still attached) and raise it to focus. */
    hx_panel_ensure_attached (panel);
    panel_widget_raise (PANEL_WIDGET (panel));
    return TRUE;
}

/* Shared body for the static + dynamic embeds. Builds the panel, homes
 * it, registers it. Returns the panel (still owned by the registry's
 * strong ref) or NULL if the dock wasn't built. */
static HxPanel *
dock_embed_common (const char   *id,
                   GtkhxDockKind kind,
                   GtkhxDockArea area,
                   const char   *title,
                   const char   *icon_name,
                   GtkWidget    *content)
{
    HxPanel   *panel;
    GtkWidget *home_frame = NULL;
    PanelArea  panel_area;

    panel_area = dock_area_to_panel_area (area, &home_frame);
    if (home_frame == NULL) {
        g_critical ("gtkhx_dock_embed(%s): toolbar dock not built yet", id);
        return NULL;
    }

    panel = hx_panel_new (id, dock_kind_to_panel_kind (kind), panel_area);
    if (title != NULL) {
        panel_widget_set_title (PANEL_WIDGET (panel), title);
    }
    if (icon_name != NULL) {
        panel_widget_set_icon_name (PANEL_WIDGET (panel), icon_name);
    }
    panel_widget_set_child (PANEL_WIDGET (panel), content);

    panel_frame_add (PANEL_FRAME (home_frame), PANEL_WIDGET (panel));
    hx_panel_set_home_frame (panel, home_frame);

    /* Registry strong-refs the panel; do NOT unref here. hx_panel_new's
     * initial ref is the GTK4 floating ref, already claimed by
     * panel_frame_add's g_object_ref_sink (clears floating, no new ref).
     * Unrefing would drop the registry's owning ref. */
    hx_panel_registry_register (panel);
    return panel;
}

void
gtkhx_dock_embed (const char   *id,
                  GtkhxDockKind kind,
                  GtkhxDockArea area,
                  const char   *title,
                  const char   *icon_name,
                  GtkWidget    *content)
{
    g_return_if_fail (id != NULL);
    g_return_if_fail (GTK_IS_WIDGET (content));

    dock_embed_common (id, kind, area, title, icon_name, content);
}

/* Close-trampoline payload. Lives on the panel via g_object_set_data_full
 * so it's freed when the panel finalizes; hx_panel_set_close_handler
 * gets it as user_data. */
typedef struct {
    void        (*on_close) (gpointer user_data);
    gpointer      user_data;
    GDestroyNotify destroy;
} DockDynClose;

static void
dock_dyn_close_free (gpointer data)
{
    DockDynClose *c = data;
    if (c == NULL) {
        return;
    }
    if (c->destroy != NULL && c->user_data != NULL) {
        c->destroy (c->user_data);
    }
    g_free (c);
}

static void
dock_dyn_close_trampoline (HxPanel *panel, gpointer user_data)
{
    DockDynClose *c = user_data;
    (void) panel;
    if (c != NULL && c->on_close != NULL) {
        c->on_close (c->user_data);
    }
}

void
gtkhx_dock_embed_dynamic (const char   *id,
                          GtkhxDockArea area,
                          const char   *title,
                          const char   *icon_name,
                          GtkWidget    *content,
                          void        (*on_close) (gpointer user_data),
                          gpointer      user_data,
                          GDestroyNotify destroy)
{
    HxPanel      *panel;
    DockDynClose *c;

    g_return_if_fail (id != NULL);
    g_return_if_fail (GTK_IS_WIDGET (content));

    panel = dock_embed_common (id, GTKHX_DOCK_KIND_DYNAMIC, area,
                               title, icon_name, content);
    if (panel == NULL) {
        return;
    }

    c = g_new0 (DockDynClose, 1);
    c->on_close  = on_close;
    c->user_data = user_data;
    c->destroy   = destroy;

    /* Keep the payload alive for the panel's lifetime and free it (which
     * runs `destroy` on user_data) on finalize. */
    g_object_set_data_full (G_OBJECT (panel), "gtkhx-dock-dyn-close",
                            c, dock_dyn_close_free);
    hx_panel_set_close_handler (panel, dock_dyn_close_trampoline, c);
}
