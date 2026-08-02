/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * dock_pages.h — a dock panel's content, as a set of named pages.
 *
 * See dock_pages.c for why a panel's single child became a stack. In short:
 * the tab-switched layout swaps every per-connection panel's content when the
 * user changes connection, and a panel that can only ever be given one child
 * cannot express that.
 *
 * Page names are opaque strings. The dock bridge names them for the
 * connection the content belongs to; this layer only requires that they are
 * distinct within a panel.
 *
 * Every function tolerates a NULL or non-stack `child` and returns the
 * do-nothing answer, because a panel's child is only a stack for panels the
 * dock bridge built.
 */

#ifndef HX_DOCK_PAGES_H
#define HX_DOCK_PAGES_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* A fresh stack holding `content` as its only page, named `page`. This is
 * what becomes a panel's child. Returns a floating ref, for the caller to
 * hand straight to `panel_widget_set_child`. */
GtkWidget *hx_dock_pages_new (const char *page, GtkWidget *content);

/* Add another page. FALSE (with a g_critical) if the name is already taken —
 * a duplicate would leave the new content unreachable rather than replacing
 * the old, so it is refused rather than silently accepted. */
gboolean hx_dock_pages_add (GtkWidget *child, const char *page,
                            GtkWidget *content);

/* Whether `page` exists. This is the per-connection form of the "is it
 * already built?" test the panel entry points do today against the panel as
 * a whole. */
gboolean hx_dock_pages_has (GtkWidget *child, const char *page);

/* Switch to `page`. FALSE if it doesn't exist, which is a no-op rather than
 * an error: a caller switching every panel at once shouldn't have to know
 * which roles a given connection has content for. */
gboolean hx_dock_pages_show (GtkWidget *child, const char *page);

/* The visible page's name, or NULL. */
const char *hx_dock_pages_visible (GtkWidget *child);

/* Remove `page` and destroy its content tree. FALSE if it wasn't there.
 *
 * This is a teardown, not a detach: every content module's destroy handler
 * doubles as its model-side teardown, so removing a page unwinds that
 * connection's state for the role. Switching connections must not use it. */
gboolean hx_dock_pages_remove (GtkWidget *child, const char *page);

/* How many pages the panel holds. 0 for a NULL or non-stack child. */
guint hx_dock_pages_count (GtkWidget *child);

G_END_DECLS

#endif /* ndef HX_DOCK_PAGES_H */
