/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * test_dock_pages.c — a dock panel's content as a set of named pages.
 *
 * This exists because a panel used to hold one child, set once, with no way
 * to replace it — and the tab-switched layout has to swap every
 * per-connection panel's content when the user changes connection. The page
 * layer is what makes that expressible.
 *
 * It is testable at all because it was split out of `dock_bridge.c`: no
 * libpanel, no dock, no registry, just a GtkStack. The dock bridge's job is
 * to resolve a panel id to that stack and delegate, and that half needs a
 * built dock and so is not covered here.
 *
 * Needs a display — GTK 4 has no headless backend — so the suite runs under
 * xvfb. A missing one is a failure, never a skip.
 */

#include "config.h"

#include <glib.h>
#include <gtk/gtk.h>

#include "dock_pages.h"

/* A distinguishable throwaway content widget. */
static GtkWidget *
content (const char *label)
{
    return gtk_label_new (label);
}

/* A stack owned by this test: `hx_dock_pages_new` hands back a floating ref
 * that production sinks into a panel, so a test has to sink it itself or the
 * widget leaks. */
static GtkWidget *
new_pages (const char *page, GtkWidget *first)
{
    GtkWidget *stack = hx_dock_pages_new (page, first);
    g_assert_nonnull (stack);
    return g_object_ref_sink (stack);
}

static void
test_new_holds_its_first_page (void)
{
    GtkWidget *stack = new_pages ("a", content ("A"));

    g_assert_true (hx_dock_pages_has (stack, "a"));
    g_assert_cmpuint (hx_dock_pages_count (stack), ==, 1);
    /* The one page is visible without anyone selecting it — which is what
     * makes a single-connection panel behave exactly as the old single child
     * did. */
    g_assert_cmpstr (hx_dock_pages_visible (stack), ==, "a");

    g_object_unref (stack);
}

static void
test_pages_are_independent (void)
{
    GtkWidget *stack = new_pages ("a", content ("A"));

    g_assert_true (hx_dock_pages_add (stack, "b", content ("B")));
    g_assert_cmpuint (hx_dock_pages_count (stack), ==, 2);
    g_assert_true (hx_dock_pages_has (stack, "a"));
    g_assert_true (hx_dock_pages_has (stack, "b"));
    g_assert_false (hx_dock_pages_has (stack, "c"));

    g_object_unref (stack);
}

/* Switching is what a connection change does to every per-connection panel.
 * The page switched away from has to survive it: its content tree carries the
 * connection's view state, and every content module's destroy handler is also
 * its model-side teardown. */
static void
test_switching_keeps_the_other_page_alive (void)
{
    GtkWidget *stack = new_pages ("a", content ("A"));
    GtkWidget *b = content ("B");

    g_assert_true (hx_dock_pages_add (stack, "b", b));
    g_object_add_weak_pointer (G_OBJECT (b), (gpointer *)&b);

    g_assert_true (hx_dock_pages_show (stack, "b"));
    g_assert_cmpstr (hx_dock_pages_visible (stack), ==, "b");

    g_assert_true (hx_dock_pages_show (stack, "a"));
    g_assert_cmpstr (hx_dock_pages_visible (stack), ==, "a");

    /* Switched away from, still there. */
    g_assert_nonnull (b);
    g_assert_true (hx_dock_pages_has (stack, "b"));
    g_assert_cmpuint (hx_dock_pages_count (stack), ==, 2);

    g_object_unref (stack);
}

/* Removing *is* a teardown, and the distinction from switching is the whole
 * reason both exist. */
static void
test_removing_destroys_the_content (void)
{
    GtkWidget *stack = new_pages ("a", content ("A"));
    GtkWidget *b = content ("B");

    g_assert_true (hx_dock_pages_add (stack, "b", b));
    g_object_add_weak_pointer (G_OBJECT (b), (gpointer *)&b);

    g_assert_true (hx_dock_pages_remove (stack, "b"));
    g_assert_null (b);
    g_assert_false (hx_dock_pages_has (stack, "b"));
    g_assert_cmpuint (hx_dock_pages_count (stack), ==, 1);

    /* Removing what isn't there is a no-op, not a crash: a connection can
     * close without having had content in every panel. */
    g_assert_false (hx_dock_pages_remove (stack, "b"));

    g_object_unref (stack);
}

/* A duplicate name would leave the new content unreachable — the lookup
 * returns the first match — rather than replacing the old. Refused loudly. */
static void
test_a_duplicate_page_is_refused (void)
{
    GtkWidget *stack = new_pages ("a", content ("A"));
    GtkWidget *dup = content ("also A");

    g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL,
                           "*already exists*");
    g_assert_false (hx_dock_pages_add (stack, "a", dup));
    g_test_assert_expected_messages ();

    g_assert_cmpuint (hx_dock_pages_count (stack), ==, 1);

    /* Refused, so ownership never transferred — this test still holds the
     * only (floating) reference. */
    g_object_ref_sink (dup);
    g_object_unref (dup);
    g_object_unref (stack);
}

/* Showing a page a connection doesn't have is a no-op rather than a blank
 * panel: whoever switches every panel at once shouldn't have to know which
 * roles a given connection has content for. */
static void
test_showing_an_absent_page_changes_nothing (void)
{
    GtkWidget *stack = new_pages ("a", content ("A"));

    g_assert_false (hx_dock_pages_show (stack, "nope"));
    g_assert_cmpstr (hx_dock_pages_visible (stack), ==, "a");

    g_object_unref (stack);
}

/* A panel's child is only a stack for panels the dock bridge built, so every
 * entry point has to tolerate one that isn't — and a NULL, which is what an
 * unknown panel id resolves to. */
static void
test_a_non_stack_child_is_inert (void)
{
    GtkWidget *plain = g_object_ref_sink (gtk_label_new ("not a stack"));

    g_assert_false (hx_dock_pages_has (plain, "a"));
    g_assert_false (hx_dock_pages_show (plain, "a"));
    g_assert_false (hx_dock_pages_remove (plain, "a"));
    g_assert_null (hx_dock_pages_visible (plain));
    g_assert_cmpuint (hx_dock_pages_count (plain), ==, 0);

    g_assert_false (hx_dock_pages_has (NULL, "a"));
    g_assert_false (hx_dock_pages_show (NULL, "a"));
    g_assert_false (hx_dock_pages_remove (NULL, "a"));
    g_assert_null (hx_dock_pages_visible (NULL));
    g_assert_cmpuint (hx_dock_pages_count (NULL), ==, 0);

    g_object_unref (plain);
}

int
main (int argc, char *argv[])
{
    /* GTK builds an AT-SPI accessibility context for the first widget
     * created, which needs a session bus; without one it emits a Gtk-WARNING,
     * and g_test_init has made warnings fatal. A headless CI container has no
     * session bus, so turn accessibility off rather than teach the test to
     * tolerate a fatal warning — the warning is worth keeping fatal, and
     * nothing here is testing a11y. Set before gtk_init, and here rather than
     * in meson so running the binary by hand behaves the same. */
    g_setenv ("GTK_A11Y", "none", TRUE);

    g_test_init (&argc, &argv, NULL);
    if (!gtk_init_check ()) {
        g_error ("no display: run under xvfb-run with GDK_BACKEND=x11");
    }

    g_test_add_func ("/dock_pages/new_holds_its_first_page",
                     test_new_holds_its_first_page);
    g_test_add_func ("/dock_pages/pages_are_independent",
                     test_pages_are_independent);
    g_test_add_func ("/dock_pages/switching_keeps_the_other_page_alive",
                     test_switching_keeps_the_other_page_alive);
    g_test_add_func ("/dock_pages/removing_destroys_the_content",
                     test_removing_destroys_the_content);
    g_test_add_func ("/dock_pages/a_duplicate_page_is_refused",
                     test_a_duplicate_page_is_refused);
    g_test_add_func ("/dock_pages/showing_an_absent_page_changes_nothing",
                     test_showing_an_absent_page_changes_nothing);
    g_test_add_func ("/dock_pages/a_non_stack_child_is_inert",
                     test_a_non_stack_child_is_inert);
    return g_test_run ();
}
