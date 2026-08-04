/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * test_frame_close_button.c — the close (X) button on a dock frame's
 * header closes the visible panel, and is greyed when there isn't one.
 *
 * This exists because that button was dead for the entire life of the
 * HxSplit dock. libpanel wires it to frame.close-page-or-frame, whose
 * handler and enabled state both begin with
 *
 *     gtk_widget_get_ancestor (frame, PANEL_TYPE_GRID)
 *
 * and there is no PanelGrid in the main dock — so it was permanently
 * insensitive and closing one panel meant Close all pages on the whole
 * frame followed by reopening the rest.
 *
 * Two things need pinning, and neither is visible from the C API alone:
 *
 *   - The button is reachable and gets adopted. It's a private template
 *     child found by CSS class, so a libpanel restructure would break
 *     the search silently; asserting on its action-name catches that.
 *   - libpanel doesn't win the enabled-state fight. frame-ops is an
 *     inserted action group precisely so panel_frame_update_actions —
 *     which runs on the way OUT of panel_frame_add and _remove, after
 *     every signal a class-action override could hook — can't reach
 *     it. An override was tried first and lost exactly there.
 *
 * Needs a display, like test_dock_pages: GTK 4 has no headless backend,
 * so the suite runs under xvfb. A missing one is a failure, never a
 * skip.
 */

#include "config.h"

#include <adwaita.h>
#include <gtk/gtk.h>
#include <libpanel.h>

#include "hx_panel.h"
#include "hx_panel_frame.h"
#include "hx_split.h"

/* The dock globals hx_panel.c and hx_split.c reach for. A single-leaf
 * fixture points all four at the one frame. */
GtkWidget *toolbar_window;
GtkWidget *toolbar_dock;
GtkWidget *toolbar_sidebar_frame;
GtkWidget *toolbar_end_frame;
GtkWidget *toolbar_bottom_frame;
GtkWidget *toolbar_center_frame;

/* Stubs for symbols the linked dock sources reach for, in the parts of
 * the app this fixture deliberately doesn't build. Prototypes so the
 * tree's -Wmissing-prototypes stays clean; the real declarations live
 * in headers that would drag in the session struct. */
void toolbar_install_panel_hooks_on_frame (GtkWidget *frame);
void init_keyaccel (GtkWidget *widget);
const char *gtkhx_config_dir (void);

void
toolbar_install_panel_hooks_on_frame (GtkWidget *frame)
{
    (void)frame;
}

void
init_keyaccel (GtkWidget *widget)
{
    (void)widget;
}

const char *
gtkhx_config_dir (void)
{
    return g_get_tmp_dir ();
}

typedef struct {
    GtkWidget *window;
    PanelFrame *frame;
    GtkWidget *close_button;
} Fixture;

/* Settle pending main-loop work. Non-blocking, so nothing here may
 * depend on a timeout elapsing — see click() for the one place that
 * bites. */
static void
pump (void)
{
    for (int i = 0; i < 200; i++) {
        g_main_context_iteration (NULL, FALSE);
    }
}

/* What a real mouse press ends in. NOT gtk_widget_activate: that runs
 * GtkButton's press animation on a ~250 ms timeout and only then emits
 * clicked, which a non-blocking pump never reaches — the test passes
 * the assertion and silently proves nothing. */
static void
click (GtkWidget *button)
{
    g_signal_emit_by_name (button, "clicked");
    pump ();
}

static GtkWidget *
find_close_button (GtkWidget *root)
{
    GtkWidget *child;

    if (GTK_IS_BUTTON (root)
        && g_strcmp0 (gtk_button_get_icon_name (GTK_BUTTON (root)),
                      "window-close-symbolic")
               == 0) {
        return root;
    }
    for (child = gtk_widget_get_first_child (root); child != NULL;
         child = gtk_widget_get_next_sibling (child)) {
        GtkWidget *hit = find_close_button (child);
        if (hit != NULL) {
            return hit;
        }
    }
    return NULL;
}

static HxPanel *
add_panel (PanelFrame *frame, const char *id)
{
    HxPanel *panel = hx_panel_new (id, HX_PANEL_KIND_CENTER, PANEL_AREA_CENTER);

    panel_widget_set_title (PANEL_WIDGET (panel), id);
    panel_widget_set_child (PANEL_WIDGET (panel), gtk_label_new (id));
    panel_frame_add (frame, PANEL_WIDGET (panel));
    pump ();
    return panel;
}

/* One leaf in a PanelDock, with the frame-ops UI installed — the same
 * shape toolbar_install_panel_hooks_on_frame gives every dock leaf. */
static void
fixture_set_up (Fixture *fx, gconstpointer user_data)
{
    HxSplit *root;

    (void)user_data;

    root = hx_split_new ();
    fx->frame = hx_split_get_frame (root);
    panel_frame_set_header (fx->frame,
                            PANEL_FRAME_HEADER (panel_frame_header_bar_new ()));

    toolbar_dock = panel_dock_new ();
    {
        GtkBuilder *builder = gtk_builder_new ();
        GtkBuildable *buildable = GTK_BUILDABLE (toolbar_dock);
        GTK_BUILDABLE_GET_IFACE (buildable)->add_child (buildable, builder,
                                                        G_OBJECT (root), NULL);
        g_object_unref (builder);
    }

    fx->window = gtk_window_new ();
    gtk_window_set_default_size (GTK_WINDOW (fx->window), 800, 600);
    gtk_window_set_child (GTK_WINDOW (fx->window), toolbar_dock);

    toolbar_window = fx->window;
    toolbar_center_frame = GTK_WIDGET (fx->frame);
    toolbar_sidebar_frame = GTK_WIDGET (fx->frame);
    toolbar_end_frame = GTK_WIDGET (fx->frame);
    toolbar_bottom_frame = GTK_WIDGET (fx->frame);

    hx_split_install_frame_ui (GTK_WIDGET (fx->frame));
    pump ();

    /* Deliberately NOT presented. A widget is rooted the moment it is
     * inside a GtkWindow's child tree, which is all the action muxer
     * chain needs to resolve frame-ops.close-page and all a
     * GtkActionable needs to track its enabled state — mapping adds
     * nothing this test asks about. Presenting creates a GdkSurface
     * and with it a GSK renderer, which on a CI container with no DRM
     * device warns about Vulkan and (because g_test_init makes
     * warnings fatal) kills the run. test_dock_pages gets this right
     * by never making a window at all. */

    fx->close_button = find_close_button (GTK_WIDGET (fx->frame));
}

static void
fixture_tear_down (Fixture *fx, gconstpointer user_data)
{
    (void)user_data;
    gtk_window_destroy (GTK_WINDOW (fx->window));
    pump ();
    toolbar_window = NULL;
    toolbar_dock = NULL;
    toolbar_sidebar_frame = NULL;
    toolbar_end_frame = NULL;
    toolbar_bottom_frame = NULL;
    toolbar_center_frame = NULL;
}

/* The search for the button is by CSS class + icon name, both private
 * template details. If libpanel ever restructures its header this is
 * the assertion that says so, rather than the button quietly going
 * back to doing nothing. */
static void
test_button_is_adopted (Fixture *fx, gconstpointer user_data)
{
    (void)user_data;

    g_assert_nonnull (fx->close_button);
    g_assert_cmpstr (
        gtk_actionable_get_action_name (GTK_ACTIONABLE (fx->close_button)), ==,
        "frame-ops.close-page");
}

static void
test_greyed_when_empty (Fixture *fx, gconstpointer user_data)
{
    (void)user_data;

    g_assert_nonnull (fx->close_button);
    g_assert_cmpuint (panel_frame_get_n_pages (fx->frame), ==, 0);
    g_assert_false (gtk_widget_get_sensitive (fx->close_button));
}

/* The regression proper: before frame-ops.close-page this stayed
 * FALSE forever, because panel_frame_update_actions saw no PanelGrid. */
static void
test_enabled_with_a_page (Fixture *fx, gconstpointer user_data)
{
    (void)user_data;

    add_panel (fx->frame, "a");
    g_assert_true (gtk_widget_get_sensitive (fx->close_button));
}

static void
test_closes_the_visible_page (Fixture *fx, gconstpointer user_data)
{
    HxPanel *a, *b;

    (void)user_data;

    a = add_panel (fx->frame, "a");
    b = add_panel (fx->frame, "b");
    g_assert_cmpuint (panel_frame_get_n_pages (fx->frame), ==, 2);

    /* Raise so the test knows which page the click should take —
     * panel_frame_add doesn't move the selection off the first page. */
    panel_widget_raise (PANEL_WIDGET (b));
    pump ();
    g_assert_true (panel_frame_get_visible_child (fx->frame)
                   == PANEL_WIDGET (b));

    click (fx->close_button);

    g_assert_cmpuint (panel_frame_get_n_pages (fx->frame), ==, 1);
    g_assert_true (panel_frame_get_visible_child (fx->frame)
                   == PANEL_WIDGET (a));
}

/* Closing every page has to leave the frame itself standing — the
 * "or frame" half of libpanel's action is deliberately not
 * reimplemented, so an emptied frame stays put and greys its X. */
static void
test_closing_the_last_page_empties_the_frame (Fixture *fx,
                                              gconstpointer user_data)
{
    (void)user_data;

    add_panel (fx->frame, "a");
    click (fx->close_button);

    g_assert_cmpuint (panel_frame_get_n_pages (fx->frame), ==, 0);
    g_assert_null (panel_frame_get_visible_child (fx->frame));
    g_assert_false (gtk_widget_get_sensitive (fx->close_button));
    /* Frame survived. */
    g_assert_true (PANEL_IS_FRAME (fx->frame));
    g_assert_nonnull (gtk_widget_get_parent (GTK_WIDGET (fx->frame)));
}

/* panel_frame_add and panel_frame_remove both call
 * panel_frame_update_actions on the way out. That is what defeated the
 * class-action override, and it's the reason this test adds and removes
 * repeatedly rather than checking the state once. */
static void
test_state_survives_repeated_add_remove (Fixture *fx, gconstpointer user_data)
{
    (void)user_data;

    for (int i = 0; i < 3; i++) {
        add_panel (fx->frame, "a");
        g_assert_true (gtk_widget_get_sensitive (fx->close_button));

        click (fx->close_button);
        g_assert_cmpuint (panel_frame_get_n_pages (fx->frame), ==, 0);
        g_assert_false (gtk_widget_get_sensitive (fx->close_button));
    }
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    /* Headless CI is a minefield of environmental GTK warnings, and
     * g_test_init makes warnings fatal, so each one is an abort with a
     * SIGTRAP and no useful message. Two defences, in that order:
     *
     * No AT-SPI. GTK builds an accessibility context per widget and
     * warns when it can't reach the session bus. Nothing here asserts
     * on accessibility, so removing the cause is better than muting
     * the symptom. (The other big source, the GSK renderer's Vulkan /
     * EGL complaints on a container with no DRM device, is handled by
     * the fixture never presenting its window — see fixture_set_up.)
     *
     * Then narrow the fatal mask to CRITICAL and ERROR. The warnings
     * this test can still meet are all about the box it runs in, not
     * about GtkHx, and having them abort the run cost two CI rounds
     * already. Loudness is preserved where it means something:
     * g_return_if_fail and friends — the way GTK reports actual
     * misuse — are CRITICALs, and every g_assert in this file is an
     * ERROR. Both still stop the test dead. */
    g_setenv ("GTK_A11Y", "none", TRUE);
    g_log_set_always_fatal (G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_ERROR);

    gtk_init ();
    adw_init ();
    panel_init (); /* registers PanelPosition & co; mandatory */

#define ADD(path, func)                                                        \
    g_test_add ((path), Fixture, NULL, fixture_set_up, (func),                 \
                fixture_tear_down)

    ADD ("/frame_close_button/adopted", test_button_is_adopted);
    ADD ("/frame_close_button/greyed_when_empty", test_greyed_when_empty);
    ADD ("/frame_close_button/enabled_with_a_page", test_enabled_with_a_page);
    ADD ("/frame_close_button/closes_the_visible_page",
         test_closes_the_visible_page);
    ADD ("/frame_close_button/last_page_empties_the_frame",
         test_closing_the_last_page_empties_the_frame);
    ADD ("/frame_close_button/survives_add_remove_churn",
         test_state_survives_repeated_add_remove);

#undef ADD

    return g_test_run ();
}
