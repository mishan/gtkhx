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
 * dock_bridge.c — see dock_bridge.h. The dock is one mullion-gtk MlnPanes;
 * this makes it, keeps its layout through dock_layout.c, and puts the Rust
 * ports' content into its panes.
 */

#include "config.h"

#include <glib.h>
#include <mullion-gtk.h>

#include "hx.h"      /* _() */
#include "gtkutil.h" /* init_keyaccel, for the undocked windows */
#include "debug.h"
#include "dock_bridge.h"
#include "dock_layout.h"
#include "dock_pages.h"
#include "panel_registry.h"
#include "toolbar.h" /* DEFAULT_LEAF_MIN_WIDTH */

/* A string xgettext picks up, translated where it is used. */
#ifndef N_
#define N_(s) (s)
#endif

static MlnPanes *dock;

/* Until gtkhx_dock_settled: startup is still opening panels. */
static gboolean settling = TRUE;

/* Every static panel: its title until the content names it, the toolbar
 * pixmap its tab shows, and the slot it goes to where a layout has no
 * place for it (a panel new since the layout was kept, or after a
 * reset). */
static const struct {
    const char *id;
    const char *title;
    const char *pixmap;
    const char *slot;
} PANES[] = {
    { HX_PANEL_ID_USERS, N_ ("Users"), "users.png", "end" },
    { HX_PANEL_ID_TASKS, N_ ("Tasks"), "tasks.png", "bottom" },
    { HX_PANEL_ID_NEWS, N_ ("News"), "news.png", "start" },
    { HX_PANEL_ID_CHAT, N_ ("Chat"), "chat.png", "center" },
    { HX_PANEL_ID_NEWS15, N_ ("News (1.5+)"), "news_folder.png", "center" },
#ifdef HAVE_VOICE
    { HX_PANEL_ID_VIDEO, N_ ("Video"), NULL, "end" },
#endif
};

/* The first run's layout, and what Reset Layout puts back: News and Tasks
 * in a column on the left (the transfer queue is empty most of the time,
 * and a transfer raises its tab), Chat and News 1.5 in the middle, Users on
 * the right. The slots are where each area's panels go when nothing
 * remembers where they were. */
static const char *DEFAULT_LAYOUT
    = "{\"dir\":\"row\",\"size\":[0.24,0.58,0.18],\"kids\":["
      "{\"tabs\":[\"news\",\"tasks\"],\"slots\":[\"start\",\"bottom\"]},"
      "{\"tabs\":[\"chat\",\"news15\"],\"slots\":[\"center\"]},"
      "{\"tabs\":[\"users\"],\"slots\":[\"end\"]}]}";

/* id -> GPtrArray of void (*) (void): what gtkhx_dock_connect_shown
 * asked for. */
static GHashTable *shown_hooks;

static GtkWidget *
pane_content (const char *id)
{
    if (dock == NULL || id == NULL) {
        return NULL;
    }
    return mln_panes_get_content (dock, id);
}

/* ---- Action rows and the corner ------------------------------------- */

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

/* The widgets that may share the corner's controls: an action row, or a
 * widget tagged to make room (the chat's subject line and its tab
 * strip). */
static gboolean
is_corner_widget (GtkWidget *w)
{
    return gtk_widget_has_css_class (w, "gtkhx-panel-actions")
           || gtk_widget_has_css_class (w, "gtkhx-pane-reserve");
}

/* The widget that sits under the corner on one content page: the first
 * *visible* corner widget in tree order. First, because that is the one at
 * the top; visible, because a hidden action row isn't there to make room
 * in. */
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

/* Room for the corner's controls on every one of a panel's pages -- each
 * connection has its own, with its own corner widget -- or, with the tabs
 * in a strip, none. And the corner kept in sight where the page on screen
 * has made room for it, since it covers nothing there: an action row, or
 * the chat's subject line, with the controls at its end. Elsewhere they
 * would sit over content, and show only on hover and focus. */
static void
reserve_corner (const char *id)
{
    GtkWidget *stack = pane_content (id);
    int margin = dock != NULL ? mln_panes_get_corner_width (dock, id) : 0;
    GtkWidget *front;

    if (stack == NULL) {
        return;
    }
    front = gtk_stack_get_visible_child (GTK_STACK (stack));
    mln_panes_set_corner_pinned (dock, id,
                                 front != NULL
                                     && find_corner_widget (front) != NULL);
    clear_corner_margins (stack);
    if (margin <= 0) {
        return;
    }
    for (GtkWidget *page = gtk_widget_get_first_child (stack); page != NULL;
         page = gtk_widget_get_next_sibling (page)) {
        GtkWidget *corner = find_corner_widget (page);
        if (corner != NULL) {
            gtk_widget_set_margin_end (corner, margin);
        }
    }
}

static void
reserve_all (void)
{
    for (gsize i = 0; i < G_N_ELEMENTS (PANES); i++) {
        reserve_corner (PANES[i].id);
    }
}

static void
on_corner_changed (MlnPanes *panes, gpointer data)
{
    (void)panes;
    (void)data;
    reserve_all ();
}

/* The Show Action Bar item's action, for one panel. */
static GSimpleAction *
actions_action (const char *id)
{
    GApplication *app = g_application_get_default ();
    g_autofree char *name = g_strdup_printf ("pane-actions-%s", id);
    GAction *a = app != NULL
                     ? g_action_map_lookup_action (G_ACTION_MAP (app), name)
                     : NULL;

    return a != NULL ? G_SIMPLE_ACTION (a) : NULL;
}

/* The panel's action rows as the setting says, and its Show Action Bar
 * item greyed where there is none (Chat). Again whenever a connection
 * adds a page, so a new page arrives matching. */
static void
sync_actions (const char *id)
{
    GtkWidget *stack = pane_content (id);
    GSimpleAction *action = actions_action (id);
    gboolean found;

    if (stack == NULL) {
        return;
    }
    found = set_action_rows_visible (stack,
                                     !dock_layout_panel_actions_hidden (id));
    if (action != NULL) {
        g_simple_action_set_enabled (action, found);
    }
    reserve_corner (id);
}

static void
on_pane_actions_change (GSimpleAction *action, GVariant *value, gpointer data)
{
    const char *id = data;

    g_simple_action_set_state (action, value);
    dock_layout_set_panel_actions_hidden (id, !g_variant_get_boolean (value));
    sync_actions (id);
}

void
gtkhx_dock_add_actions (GActionMap *map)
{
    for (gsize i = 0; i < G_N_ELEMENTS (PANES); i++) {
        g_autofree char *name
            = g_strdup_printf ("pane-actions-%s", PANES[i].id);
        GSimpleAction *a = g_simple_action_new_stateful (
            name, NULL,
            g_variant_new_boolean (
                !dock_layout_panel_actions_hidden (PANES[i].id)));

        g_signal_connect (a, "change-state",
                          G_CALLBACK (on_pane_actions_change),
                          (gpointer)PANES[i].id);
        g_action_map_add_action (map, G_ACTION (a));
        g_object_unref (a);
        sync_actions (PANES[i].id);
    }
}

/* ---- The dock's signals ---------------------------------------------- */

static void
on_layout_kept (MlnPanes *panes, const char *mode, const char *layout,
                gpointer data)
{
    (void)panes;
    (void)mode;
    (void)data;
    dock_layout_keep (layout);
}

static void
on_pane_shown (MlnPanes *panes, const char *id, gboolean on, gpointer data)
{
    GPtrArray *hooks;

    (void)panes;
    (void)data;

    if (!on || shown_hooks == NULL) {
        return;
    }
    hooks = g_hash_table_lookup (shown_hooks, id);
    for (guint i = 0; hooks != NULL && i < hooks->len; i++) {
        ((void (*) (void))hooks->pdata[i]) ();
    }
}

/* A window for panels moved out of the main one, as the old undocked
 * windows were: the application's, with its keys. mullion-gtk titles it
 * and keeps its size with the layout. */
static GtkWindow *
make_window (MlnPanes *panes, gpointer data)
{
    GtkWidget *win = gtk_window_new ();
    GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (panes));
    GApplication *app = g_application_get_default ();

    (void)data;

    if (GTK_IS_WINDOW (root)) {
        gtk_window_set_transient_for (GTK_WINDOW (win), GTK_WINDOW (root));
    }
    if (app != NULL) {
        gtk_window_set_application (GTK_WINDOW (win), GTK_APPLICATION (app));
    }
    init_keyaccel (win);

    return GTK_WINDOW (win);
}

/* ---- The dock -------------------------------------------------------- */

GtkWidget *
gtkhx_dock_new (void)
{
    g_autofree char *kept = NULL;

    g_return_val_if_fail (dock == NULL, GTK_WIDGET (dock));

    dock = MLN_PANES (mln_panes_new ());

    /* Closed panels come back from the main menu's Panels section, as they
     * always have; a row of them over the layout would be a second way,
     * taking a row of height to say so. */
    g_object_set (dock, "show-drawer", FALSE, NULL);

    for (gsize i = 0; i < G_N_ELEMENTS (PANES); i++) {
        GtkWidget *stack = gtk_stack_new ();

        /* No transition: a connection switch should be instant. */
        gtk_stack_set_transition_type (GTK_STACK (stack),
                                       GTK_STACK_TRANSITION_TYPE_NONE);
        mln_panes_register (dock, PANES[i].id, _ (PANES[i].title), stack,
                            DEFAULT_LEAF_MIN_WIDTH);
        mln_panes_set_placement (dock, PANES[i].id, PANES[i].slot, TRUE);

        if (PANES[i].pixmap != NULL) {
            g_autofree char *uri
                = g_strconcat ("resource:///com/nasledov/gtkhx/pixmaps/",
                               PANES[i].pixmap, NULL);
            GFile *file = g_file_new_for_uri (uri);
            GIcon *icon = g_file_icon_new (file);

            mln_panes_set_icon (dock, PANES[i].id, icon);
            g_object_unref (icon);
            g_object_unref (file);
        }

        {
            GMenu *items = g_menu_new ();
            g_autofree char *action
                = g_strdup_printf ("app.pane-actions-%s", PANES[i].id);

            g_menu_append (items, _ ("Show _Action Bar"), action);
            mln_panes_set_pane_menu (dock, PANES[i].id, G_MENU_MODEL (items));
            g_object_unref (items);
        }
    }

    mln_panes_set_window_func (dock, make_window, NULL, NULL);
    g_signal_connect (dock, "layout-kept", G_CALLBACK (on_layout_kept), NULL);
    g_signal_connect (dock, "pane-shown", G_CALLBACK (on_pane_shown), NULL);
    g_signal_connect (dock, "corner-changed", G_CALLBACK (on_corner_changed),
                      NULL);

    mln_panes_set_default (dock, "main", DEFAULT_LAYOUT);
    mln_panes_set_mode (dock, "main");

    /* After the file is read: it says which headers. */
    kept = dock_layout_load ();
    mln_panes_set_header (dock, dock_layout_pane_titles_visible ()
                                    ? MLN_HEADER_STRIP
                                    : MLN_HEADER_CORNER);
    if (!mln_panes_load (dock, kept) && kept != NULL) {
        g_warning ("dock: the saved layout does not read; "
                   "the default comes up");
    }

    gtk_widget_set_hexpand (GTK_WIDGET (dock), TRUE);
    gtk_widget_set_vexpand (GTK_WIDGET (dock), TRUE);

    return GTK_WIDGET (dock);
}

void
gtkhx_dock_settled (void)
{
    settling = FALSE;
}

gboolean
gtkhx_dock_is_open (const char *id)
{
    g_auto (GStrv) closed = NULL;

    if (dock == NULL || mln_panes_get_content (dock, id) == NULL) {
        return FALSE;
    }
    closed = mln_panes_get_closed (dock);
    return !g_strv_contains ((const char *const *)closed, id);
}

void
gtkhx_dock_present (const char *id)
{
    GtkWindow *win;

    if (dock == NULL) {
        return;
    }
    mln_panes_present (dock, id, TRUE);
    win = mln_panes_get_window (dock, id);
    if (win != NULL) {
        gtk_window_present (win);
    }
}

void
gtkhx_dock_show_if_open (const char *id)
{
    if (gtkhx_dock_is_open (id)) {
        mln_panes_present (dock, id, FALSE);
    }
}

void
gtkhx_dock_set_pane_titles (gboolean on)
{
    if (dock != NULL) {
        mln_panes_set_header (dock, on ? MLN_HEADER_STRIP : MLN_HEADER_CORNER);
    }
}

void
gtkhx_dock_reset (void)
{
    if (dock != NULL) {
        mln_panes_reset (dock);
    }
}

void
gtkhx_dock_connect_shown (const char *id, void (*func) (void))
{
    GPtrArray *hooks;

    g_return_if_fail (id != NULL && func != NULL);

    if (shown_hooks == NULL) {
        shown_hooks = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                             (GDestroyNotify)g_ptr_array_unref);
    }
    hooks = g_hash_table_lookup (shown_hooks, id);
    if (hooks == NULL) {
        hooks = g_ptr_array_new ();
        g_hash_table_insert (shown_hooks, g_strdup (id), hooks);
    }
    g_ptr_array_add (hooks, (gpointer)func);
}

/* ---- The Rust ports' ABI --------------------------------------------- */

gboolean
gtkhx_dock_raise_if_open (const char *id)
{
    g_return_val_if_fail (id != NULL, FALSE);

    if (!gtkhx_dock_is_embedded (id)) {
        return FALSE;
    }
    if (!settling) {
        mln_panes_present (dock, id, TRUE);
    }
    return TRUE;
}

gboolean
gtkhx_dock_is_embedded (const char *id)
{
    g_return_val_if_fail (id != NULL, FALSE);

    return hx_dock_pages_count (pane_content (id)) > 0;
}

void
gtkhx_dock_set_needs_attention (const char *id, gboolean state)
{
    g_return_if_fail (id != NULL);

    if (dock != NULL) {
        mln_panes_set_attention (dock, id, state);
    }
}

/* Consume `content` on a failure path, so the caller never has to reason
 * about a still-floating widget it handed us. */
static void
drop_content (GtkWidget *content)
{
    if (content != NULL) {
        g_object_ref_sink (content);
        g_object_unref (content);
    }
}

gboolean
gtkhx_dock_embed (const char *id, GtkhxDockKind kind, GtkhxDockArea area,
                  const char *title, const char *icon_name, const char *page,
                  GtkWidget *content)
{
    g_return_val_if_fail (id != NULL, FALSE);
    g_return_val_if_fail (page != NULL, FALSE);
    g_return_val_if_fail (GTK_IS_WIDGET (content), FALSE);

    (void)kind;
    (void)area;
    (void)icon_name; /* the tab shows the toolbar's pixmap (PANES) */

    if (pane_content (id) == NULL) {
        g_critical ("gtkhx_dock_embed(%s): no such pane in the dock", id);
        drop_content (content);
        return FALSE;
    }
    if (!hx_dock_pages_add (pane_content (id), page, content)) {
        drop_content (content);
        return FALSE;
    }
    if (title != NULL) {
        mln_panes_set_title (dock, id, title);
    }
    sync_actions (id);
    return TRUE;
}

/* ---- Per-connection content pages ------------------------------------ *
 *
 * The panel-level API above answers "does this role have a panel?". These
 * answer "does this connection have content in it?", which is the question
 * the tab-switched layout actually asks. See dock_pages.h. */

gboolean
gtkhx_dock_add_page (const char *id, const char *page, GtkWidget *content)
{
    g_return_val_if_fail (GTK_IS_WIDGET (content), FALSE);

    if (!gtkhx_dock_is_embedded (id)
        || !hx_dock_pages_add (pane_content (id), page, content)) {
        drop_content (content);
        return FALSE;
    }
    sync_actions (id);
    return TRUE;
}

gboolean
gtkhx_dock_has_page (const char *id, const char *page)
{
    return hx_dock_pages_has (pane_content (id), page);
}

gboolean
gtkhx_dock_show_page (const char *id, const char *page)
{
    gboolean shown = hx_dock_pages_show (pane_content (id), page);

    /* Another connection's page, with its own corner widget or none. */
    reserve_corner (id);
    return shown;
}

gboolean
gtkhx_dock_remove_page (const char *id, const char *page)
{
    return hx_dock_pages_remove (pane_content (id), page);
}

guint
gtkhx_dock_page_count (const char *id)
{
    return hx_dock_pages_count (pane_content (id));
}
