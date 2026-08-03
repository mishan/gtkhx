/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * dock_pages.c — a dock panel's content, as a set of named pages.
 *
 * A panel used to hold exactly one child, set once at embed time and never
 * replaceable: `panel_widget_set_child` had a single call site in the whole
 * tree and there was no getter, no swap, no remove. That is fine for one
 * connection and is the thing the tab-switched layout cannot do, because
 * switching connections means swapping the entire set of per-connection
 * panels' contents.
 *
 * So a panel's child becomes a GtkStack and its content becomes a page in
 * that stack, named for the connection it belongs to. Switching connections
 * is `gtk_stack_set_visible_child_name` on each per-connection panel — the
 * inactive trees stay parented and alive, which matters more than it sounds:
 * every content module's destroy handler doubles as its model-side teardown,
 * so a scheme that unparented and dropped the outgoing tree would silently
 * null the state of the connection just switched away from.
 *
 * This file is deliberately free of libpanel and of the dock: it is GtkStack
 * manipulation and nothing else, so `dock_bridge.c` can resolve a panel id to
 * a stack and delegate, and a unit test can exercise the page logic without
 * standing up a dock. Everything here tolerates NULL and a non-stack widget,
 * because the panel's child is only a stack for panels this module created.
 */

#include "config.h"

#include "dock_pages.h"

GtkWidget *
hx_dock_pages_new (const char *page, GtkWidget *content)
{
    GtkWidget *stack;

    g_return_val_if_fail (page != NULL, NULL);
    g_return_val_if_fail (GTK_IS_WIDGET (content), NULL);

    stack = gtk_stack_new ();
    /* No transition: a connection switch should be instant. A crossfade
     * between two servers' chat logs reads as a rendering glitch rather than
     * as navigation. */
    gtk_stack_set_transition_type (GTK_STACK (stack),
                                   GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_stack_add_named (GTK_STACK (stack), content, page);
    return stack;
}

/* The stack behind a panel's child, or NULL if this widget isn't one. */
static GtkStack *
as_stack (GtkWidget *child)
{
    if (child == NULL || !GTK_IS_STACK (child)) {
        return NULL;
    }
    return GTK_STACK (child);
}

gboolean
hx_dock_pages_add (GtkWidget *child, const char *page, GtkWidget *content)
{
    GtkStack *stack = as_stack (child);

    g_return_val_if_fail (page != NULL, FALSE);
    g_return_val_if_fail (GTK_IS_WIDGET (content), FALSE);

    if (stack == NULL) {
        return FALSE;
    }
    /* Adding a name the stack already has would leave two children answering
     * to it and `gtk_stack_get_child_by_name` returning the first — so the
     * caller's new content would be silently unreachable. Refuse instead;
     * every caller has a has-page test available and this is a bug if it
     * fires. */
    if (gtk_stack_get_child_by_name (stack, page) != NULL) {
        g_critical ("hx_dock_pages_add: page \"%s\" already exists", page);
        return FALSE;
    }
    gtk_stack_add_named (stack, content, page);
    return TRUE;
}

gboolean
hx_dock_pages_has (GtkWidget *child, const char *page)
{
    GtkStack *stack = as_stack (child);

    if (stack == NULL || page == NULL) {
        return FALSE;
    }
    return gtk_stack_get_child_by_name (stack, page) != NULL;
}

gboolean
hx_dock_pages_show (GtkWidget *child, const char *page)
{
    GtkStack *stack = as_stack (child);

    if (stack == NULL || page == NULL) {
        return FALSE;
    }
    /* Switching to a page that isn't there is a no-op rather than a blank
     * panel: a connection can legitimately have no content for a given role
     * yet (its panel factory hasn't run, or the role doesn't apply), and the
     * caller switching every panel at once shouldn't have to know which. */
    if (gtk_stack_get_child_by_name (stack, page) == NULL) {
        return FALSE;
    }
    gtk_stack_set_visible_child_name (stack, page);
    return TRUE;
}

const char *
hx_dock_pages_visible (GtkWidget *child)
{
    GtkStack *stack = as_stack (child);

    if (stack == NULL) {
        return NULL;
    }
    return gtk_stack_get_visible_child_name (stack);
}

gboolean
hx_dock_pages_remove (GtkWidget *child, const char *page)
{
    GtkStack *stack = as_stack (child);
    GtkWidget *content;

    if (stack == NULL || page == NULL) {
        return FALSE;
    }
    content = gtk_stack_get_child_by_name (stack, page);
    if (content == NULL) {
        return FALSE;
    }
    /* This is what runs the content tree's destroy handlers, which for every
     * content module in the tree *are* its model-side teardown. So removing a
     * page is a real teardown of that connection's view for this role, not a
     * detach — which is why nothing switches connections by removing pages. */
    gtk_stack_remove (stack, content);
    return TRUE;
}

guint
hx_dock_pages_count (GtkWidget *child)
{
    GtkStack *stack = as_stack (child);
    GtkSelectionModel *pages;
    guint n;

    if (stack == NULL) {
        return 0;
    }
    pages = gtk_stack_get_pages (stack);
    n = g_list_model_get_n_items (G_LIST_MODEL (pages));
    g_object_unref (pages);
    return n;
}
