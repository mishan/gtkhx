/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * dock_layout.c — save / restore the main dock's HxSplit shape
 * and per-leaf panel placement. See dock_layout.h for the file
 * format and contract.
 */

#include "config.h"

#include "dock_layout.h"
#include "dock_layout_parse.h"

#include "hx.h" /* session typedef — toolbar.h needs it */
#include "hx_panel.h"
#include "hx_panel_frame.h"
#include "hx_split.h"
#include "panel_registry.h"
#include "toolbar.h" /* DEFAULT_LEAF_MIN_WIDTH */
#include "debug.h"

#include <errno.h>
#include <stdio.h> /* sscanf — used by the [Undocked] parser */
#include <string.h>

#include <glib/gstdio.h>

extern const char *gtkhx_config_dir (void);

/* ----------------------------------------------------------------- */
/* Module state                                                      */
/* ----------------------------------------------------------------- */

typedef struct {
    int w;
    int h;
} UndockedSize;

static struct {
    gboolean loaded;               /* TRUE iff dock_layout_load
                                       * found and parsed a file. */
    GHashTable *id_to_frame;       /* char* (panel id) → GtkWidget*
                                       * (PanelFrame*, borrowed). */
    GHashTable *id_to_undock_size; /* char* (panel id) → UndockedSize*;
                                       * panels saved as living in their
                                       * own undocked window. Consumed by
                                       * dock_layout_place_panel — each
                                       * matching id triggers a one-time
                                       * hx_panel_undock + size apply. */
    GtkPaned **paned_order;        /* depth-first order, set by load
                                       * + apply_geometry, used by save */
    guint n_paned;
    guint save_idle_id;
    gboolean save_disabled; /* TRUE after dock_layout_reset until
                                       * the next process — every save
                                       * request is dropped on the floor.
                                       * Without this, paned-position
                                       * notify::s during the rest of the
                                       * session re-create the file we
                                       * just deleted. */
    HxSplit *dock_root;     /* set by dock_layout_set_dock_root;
                                       * walked at save time */
} dock = { 0 };

void
dock_layout_set_dock_root (HxSplit *root)
{
    dock.dock_root = root;
}

static const char *LAYOUT_FILE = "dock-layout.ini";
static const guint SAVE_DEBOUNCE_MS = 200;

static char *
layout_file_path (void)
{
    return g_build_filename (gtkhx_config_dir (), LAYOUT_FILE, NULL);
}

/* ----------------------------------------------------------------- */
/* Serialise: live HxSplit tree → string                             */
/* ----------------------------------------------------------------- */

/* Map a frame back to its role tag (start / center / bottom / end),
 * or NULL if it's a user-created leaf. The four toolbar_*_frame
 * globals carry the roles. */
extern GtkWidget *toolbar_sidebar_frame;
extern GtkWidget *toolbar_end_frame;
extern GtkWidget *toolbar_bottom_frame;
extern GtkWidget *toolbar_center_frame;
extern GtkWidget *toolbar_window;

static const char *
role_for_frame (GtkWidget *frame)
{
    if (frame == toolbar_sidebar_frame) {
        return "start";
    }
    if (frame == toolbar_end_frame) {
        return "end";
    }
    if (frame == toolbar_bottom_frame) {
        return "bottom";
    }
    if (frame == toolbar_center_frame) {
        return "center";
    }
    return NULL;
}

static void
serialize_leaf (GString *out, PanelFrame *frame)
{
    guint n = panel_frame_get_n_pages (frame);
    const char *role = role_for_frame (GTK_WIDGET (frame));
    gboolean first = TRUE;

    g_string_append (out, "L[");
    for (guint i = 0; i < n; i++) {
        PanelWidget *p = panel_frame_get_page (frame, i);
        if (p == NULL || !HX_IS_PANEL (p)) {
            continue;
        }
        if (!first) {
            g_string_append_c (out, ',');
        }
        g_string_append (out, hx_panel_get_id (HX_PANEL (p)));
        first = FALSE;
    }
    if (role != NULL) {
        g_string_append_c (out, ':');
        g_string_append (out, role);
    }
    g_string_append_c (out, ']');
}

/* Walks live tree from root and appends to `out`. Returns the
 * number of internal splits encountered, in depth-first order
 * (matches the order in which paned positions get collected). */
static void
serialize_node (GString *out, HxSplit *node, GArray *paned_positions)
{
    PanelFrame *frame = hx_split_get_frame (node);

    if (frame != NULL) {
        serialize_leaf (out, frame);
        return;
    }

    GtkOrientation o = hx_split_get_orientation (node);
    GtkPaned *paned = hx_split_get_paned (node);

    g_string_append (out, o == GTK_ORIENTATION_HORIZONTAL ? "h(" : "v(");
    serialize_node (out, hx_split_get_child_a (node), paned_positions);
    g_string_append_c (out, ',');
    serialize_node (out, hx_split_get_child_b (node), paned_positions);
    g_string_append_c (out, ')');

    if (paned_positions != NULL && paned != NULL) {
        int pos = gtk_paned_get_position (paned);
        g_array_append_val (paned_positions, pos);
    }
}

/* Save / serialise use dock.dock_root directly — toolbar.c calls
 * dock_layout_set_dock_root once the tree is mounted in the dock,
 * and the root pointer is stable for the lifetime of the toolbar
 * window (hx_split_close_leaf refuses to close the root). */

/* ----------------------------------------------------------------- */
/* Build: DLParsedNode → live HxSplit tree                           */
/* ----------------------------------------------------------------- */

/* Builds the HxSplit subtree for `node`. Records the four
 * role-tagged leaves into out_sidebar/center/bottom/end and seeds
 * dock.id_to_frame with each leaf's panel-id list. */
static HxSplit *
build_node (DLParsedNode *node, GtkWidget **out_sidebar, GtkWidget **out_center,
            GtkWidget **out_bottom, GtkWidget **out_end,
            GPtrArray *paned_collect)
{
    if (node->is_leaf) {
        PanelFrame *frame = hx_panel_frame_new ();
        panel_frame_set_header (
            frame, PANEL_FRAME_HEADER (panel_frame_header_bar_new ()));
        gtk_widget_set_size_request (GTK_WIDGET (frame), DEFAULT_LEAF_MIN_WIDTH,
                                     -1);

        HxSplit *leaf = hx_split_new_with_frame (frame);

        /* Wire role pointers. */
        if (node->role != NULL) {
            GtkWidget *fw = GTK_WIDGET (frame);
            if (g_strcmp0 (node->role, "start") == 0) {
                *out_sidebar = fw;
            } else if (g_strcmp0 (node->role, "end") == 0) {
                *out_end = fw;
            } else if (g_strcmp0 (node->role, "bottom") == 0) {
                *out_bottom = fw;
            } else if (g_strcmp0 (node->role, "center") == 0) {
                *out_center = fw;
            }
        }

        /* Seed id_to_frame. */
        for (guint i = 0; i < node->panel_ids->len; i++) {
            const char *id = g_ptr_array_index (node->panel_ids, i);
            g_hash_table_replace (dock.id_to_frame, g_strdup (id), frame);
        }
        return leaf;
    }

    HxSplit *a = build_node (node->child_a, out_sidebar, out_center, out_bottom,
                             out_end, paned_collect);
    HxSplit *b = build_node (node->child_b, out_sidebar, out_center, out_bottom,
                             out_end, paned_collect);
    GtkOrientation o = (node->orientation == DL_ORIENT_HORIZONTAL)
                           ? GTK_ORIENTATION_HORIZONTAL
                           : GTK_ORIENTATION_VERTICAL;
    HxSplit *split = hx_split_new_internal (a, b, o);
    if (paned_collect != NULL) {
        g_ptr_array_add (paned_collect, hx_split_get_paned (split));
    }
    return split;
}

/* ----------------------------------------------------------------- */
/* Save: undocked-window walk                                        */
/* ----------------------------------------------------------------- */

/* hx_panel_registry_foreach callback. For each panel whose root
 * window is NOT the main toolbar window, write a [Undocked] key
 * to the GKeyFile carrying its current width / height. */
static void
visit_undocked_panel (HxPanel *panel, gpointer user_data)
{
    GKeyFile *kf;
    GtkRoot *root;
    int w = 0;
    int h = 0;
    const char *id;
    char buf[32];

    kf = (GKeyFile *)user_data;
    root = gtk_widget_get_root (GTK_WIDGET (panel));
    if (root == NULL || GTK_WIDGET (root) == toolbar_window) {
        return;
    }
    if (!GTK_IS_WINDOW (root)) {
        return;
    }

    gtk_window_get_default_size (GTK_WINDOW (root), &w, &h);
    if (w <= 0 || h <= 0) {
        return;
    }

    id = hx_panel_get_id (panel);
    if (id == NULL || id[0] == '\0') {
        return;
    }

    g_snprintf (buf, sizeof buf, "%d,%d", w, h);
    g_key_file_set_string (kf, "Undocked", id, buf);
}

/* ----------------------------------------------------------------- */
/* Save (coalesced)                                                  */
/* ----------------------------------------------------------------- */

static gboolean
on_save_idle (gpointer user_data)
{
    (void)user_data;
    dock.save_idle_id = 0;

    HxSplit *root = dock.dock_root;
    if (root == NULL) {
        debug_log ("layout", "save: no dock root, skipping");
        return G_SOURCE_REMOVE;
    }

    GString *tree = g_string_new (NULL);
    GArray *sizes = g_array_new (FALSE, FALSE, sizeof (int));
    serialize_node (tree, root, sizes);

    GKeyFile *kf = g_key_file_new ();

    /* Window size lives in gtkhxrc via gtkhx_save_window_positions
     * and Window_Geo — don't duplicate it here. dock-layout.ini
     * stays focused on the tree shape and paned positions. */

    g_key_file_set_string (kf, "Dock", "tree", tree->str);
    /* Walk every registered panel and serialise the ones whose
     * root is an undocked window. The [Undocked] section ends up
     * with one key per panel, value "W,H". Empty section when
     * nothing is undocked — GKeyFile drops empty groups so the
     * file just doesn't gain an [Undocked] header in that case. */
    hx_panel_registry_foreach (visit_undocked_panel, kf);
    if (sizes->len > 0) {
        GString *sz = g_string_new (NULL);
        for (guint i = 0; i < sizes->len; i++) {
            if (i > 0) {
                g_string_append_c (sz, ';');
            }
            g_string_append_printf (sz, "%d", g_array_index (sizes, int, i));
        }
        g_key_file_set_string (kf, "Dock", "sizes", sz->str);
        g_string_free (sz, TRUE);
    }

    gsize len = 0;
    char *data = g_key_file_to_data (kf, &len, NULL);
    char *path = layout_file_path ();
    GError *err = NULL;
    if (!g_file_set_contents (path, data, (gssize)len, &err)) {
        g_warning ("dock_layout: write %s: %s", path, err ? err->message : "?");
        g_clear_error (&err);
    } else {
        debug_log ("layout", "saved: %s", path);
    }
    g_free (data);
    g_free (path);
    g_key_file_unref (kf);
    g_array_unref (sizes);
    g_string_free (tree, TRUE);

    return G_SOURCE_REMOVE;
}

void
dock_layout_request_save (void)
{
    if (dock.save_disabled) {
        return;
    }
    /* Debounce, not throttle: every request resets the timer so a
     * burst of notify::position during a divider drag (or rapid
     * undock/redock) collapses to one write 200 ms after the last
     * request, not a write every 200 ms across the burst. */
    if (dock.save_idle_id != 0) {
        g_source_remove (dock.save_idle_id);
    }
    dock.save_idle_id = g_timeout_add (SAVE_DEBOUNCE_MS, on_save_idle, NULL);
}

/* ----------------------------------------------------------------- */
/* Load                                                              */
/* ----------------------------------------------------------------- */

/* hx_split_foreach_leaf callback. Latches the first leaf's frame
 * pointer through user_data; subsequent leaves are ignored. */
static void
find_first_leaf_frame_cb (HxSplit *leaf, gpointer user_data)
{
    GtkWidget **out = user_data;
    PanelFrame *frame;

    if (*out != NULL) {
        return;
    }
    frame = hx_split_get_frame (leaf);
    if (frame != NULL) {
        *out = GTK_WIDGET (frame);
    }
}

gboolean
dock_layout_load (HxSplit **out_root, GtkWidget **out_sidebar_frame,
                  GtkWidget **out_center_frame, GtkWidget **out_bottom_frame,
                  GtkWidget **out_end_frame)
{
    char *path = layout_file_path ();
    GKeyFile *kf = g_key_file_new ();
    GError *err = NULL;
    gboolean ok = FALSE;

    if (!g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, &err)) {
        if (!g_error_matches (err, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
            g_warning ("dock_layout: load %s: %s", path,
                       err ? err->message : "?");
        }
        g_clear_error (&err);
        goto out;
    }

    char *tree_str = g_key_file_get_string (kf, "Dock", "tree", NULL);
    if (tree_str == NULL) {
        goto out;
    }

    DLParsedNode *parsed = dl_parse_tree (tree_str);
    g_free (tree_str);
    if (parsed == NULL) {
        g_warning ("dock_layout: malformed tree in %s; "
                   "falling back to defaults",
                   path);
        goto out;
    }

    /* Prime / reset module state. */
    if (dock.id_to_frame != NULL) {
        g_hash_table_remove_all (dock.id_to_frame);
    } else {
        dock.id_to_frame
            = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    }

    GPtrArray *paneds = g_ptr_array_new ();
    *out_sidebar_frame = NULL;
    *out_center_frame = NULL;
    *out_bottom_frame = NULL;
    *out_end_frame = NULL;
    HxSplit *root = build_node (parsed, out_sidebar_frame, out_center_frame,
                                out_bottom_frame, out_end_frame, paneds);
    dl_parsed_node_free (parsed);

    /* Default any missing roles to the first leaf. The toolbar_*_frame
     * globals MUST be non-NULL — static-panel factories dereference
     * them as their panel_frame_add target. But a saved tree can
     * legitimately lack some role tags:
     *
     *   - Closing a default leaf in hx_split.c's on_frame_close
     *     reseats the matching toolbar_*_frame global onto the
     *     surviving sibling. After closing N default leaves, all
     *     N+1 of the still-distinct globals collapse to the same
     *     pointer.
     *   - role_for_frame walks the four toolbar_*_frame globals in
     *     a fixed if-else chain and returns the first match. So
     *     when several globals point at the same leaf, the leaf
     *     only gets the topmost role tag (start) serialised; the
     *     others appear missing on load.
     *
     * Falling back to defaults in that case would erase the user's
     * arrangement (the "I undocked everything and now main dock is
     * just an empty leaf" case Misha demoed). Point the missing
     * globals at the first leaf instead; new panels added by
     * factories land there briefly before dock_layout_place_panel
     * moves them to their saved location or undocks them. */
    {
        GtkWidget *first_leaf_frame = NULL;
        hx_split_foreach_leaf (root, find_first_leaf_frame_cb,
                               &first_leaf_frame);
        if (first_leaf_frame == NULL) {
            g_warning ("dock_layout: saved tree has no leaves; "
                       "falling back to defaults");
            g_ptr_array_unref (paneds);
            g_hash_table_remove_all (dock.id_to_frame);
            if (root != NULL) {
                g_object_unref (g_object_ref_sink (root));
            }
            goto out;
        }
        if (*out_sidebar_frame == NULL) {
            *out_sidebar_frame = first_leaf_frame;
        }
        if (*out_center_frame == NULL) {
            *out_center_frame = first_leaf_frame;
        }
        if (*out_bottom_frame == NULL) {
            *out_bottom_frame = first_leaf_frame;
        }
        if (*out_end_frame == NULL) {
            *out_end_frame = first_leaf_frame;
        }
    }

    *out_root = root;

    /* Stash paneds for apply_geometry. */
    g_free (dock.paned_order);
    dock.n_paned = paneds->len;
    dock.paned_order = (GtkPaned **)g_ptr_array_free (paneds, FALSE);

    /* Stash sizes for apply_geometry. */
    /* Stored as a static array of ints alongside the paned order;
     * we re-read the key file in apply_geometry rather than copy
     * here to keep this function focused. */

    /* Parse [Undocked] into the pending-undock map. Consumed by
     * dock_layout_place_panel — when each panel's factory runs and
     * registers, the placement hook checks this map and calls
     * hx_panel_undock with the saved size. */
    if (dock.id_to_undock_size != NULL) {
        g_hash_table_remove_all (dock.id_to_undock_size);
    } else {
        dock.id_to_undock_size
            = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
    }

    {
        gsize n_keys = 0;
        char **keys = g_key_file_get_keys (kf, "Undocked", &n_keys, NULL);
        for (gsize i = 0; keys != NULL && i < n_keys; i++) {
            char *val = g_key_file_get_string (kf, "Undocked", keys[i], NULL);
            int w = 0;
            int h = 0;
            if (val != NULL && sscanf (val, "%d,%d", &w, &h) == 2 && w > 0
                && h > 0) {
                UndockedSize *sz = g_new (UndockedSize, 1);
                sz->w = w;
                sz->h = h;
                g_hash_table_replace (dock.id_to_undock_size,
                                      g_strdup (keys[i]), sz);
            }
            g_free (val);
        }
        g_strfreev (keys);
    }

    dock.loaded = TRUE;
    ok = TRUE;
    debug_log ("layout", "loaded: %s", path);

out:
    g_key_file_unref (kf);
    g_free (path);
    return ok;
}

/* notify::max-position one-shot: GtkPaned starts with
 * max_position=0 before allocation, and gtk_paned_set_position
 * clamps to [min, max]. Setting a non-zero position before the
 * first allocation collapses to 0. Defer set_position until
 * max-position notifies a real value, then disconnect.
 *
 * pos is passed via the data pointer (GPOINTER_TO_INT). */
static void
on_paned_apply_saved_position (GObject *object, GParamSpec *pspec,
                               gpointer data)
{
    int max_position = 0;
    int pos = GPOINTER_TO_INT (data);

    (void)pspec;
    g_object_get (object, "max-position", &max_position, NULL);
    if (max_position <= 0) {
        return;
    }

    gtk_paned_set_position (GTK_PANED (object), pos);
    g_signal_handlers_disconnect_by_func (object, on_paned_apply_saved_position,
                                          data);
}

void
dock_layout_apply_geometry (GtkWindow *window)
{
    (void)window; /* Window size lives in gtkhxrc; we only
                    * restore paned positions here. */

    if (!dock.loaded) {
        return; /* No saved tree — nothing to restore. */
    }

    char *path = layout_file_path ();
    GKeyFile *kf = g_key_file_new ();
    GError *err = NULL;

    if (!g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, &err)) {
        g_clear_error (&err);
        goto out;
    }

    char *sizes_str = g_key_file_get_string (kf, "Dock", "sizes", NULL);
    if (sizes_str != NULL) {
        char **parts = g_strsplit (sizes_str, ";", -1);
        for (guint i = 0; parts[i] != NULL && i < dock.n_paned; i++) {
            int pos = (int)g_ascii_strtoll (parts[i], NULL, 10);
            if (pos > 0 && dock.paned_order[i] != NULL) {
                g_signal_connect (dock.paned_order[i], "notify::max-position",
                                  G_CALLBACK (on_paned_apply_saved_position),
                                  GINT_TO_POINTER (pos));
            }
        }
        g_strfreev (parts);
        g_free (sizes_str);
    }

out:
    g_key_file_unref (kf);
    g_free (path);
}

/* ----------------------------------------------------------------- */
/* Per-panel placement on registry register                          */
/* ----------------------------------------------------------------- */

void
dock_layout_place_panel (HxPanel *panel)
{
    const char *id;

    if (!dock.loaded) {
        return;
    }

    id = hx_panel_get_id (panel);
    if (id == NULL) {
        return;
    }

    /* Main-dock reseat. The factory just placed the panel in some
     * default frame; if the saved layout puts it elsewhere, move
     * it now. */
    if (dock.id_to_frame != NULL) {
        GtkWidget *target = g_hash_table_lookup (dock.id_to_frame, id);
        if (target != NULL) {
            GtkWidget *current = gtk_widget_get_ancestor (GTK_WIDGET (panel),
                                                          PANEL_TYPE_FRAME);
            if (current != target) {
                g_object_ref (panel);
                if (current != NULL) {
                    panel_frame_remove (PANEL_FRAME (current),
                                        PANEL_WIDGET (panel));
                }
                panel_frame_add (PANEL_FRAME (target), PANEL_WIDGET (panel));
                hx_panel_set_home_frame (panel, target);
                g_object_unref (panel);
            }
        }
    }

    /* Undock if the saved layout had this panel living in its own
     * window. Consume the map entry so a subsequent re-register
     * (after a redock + close) doesn't re-undock. */
    if (dock.id_to_undock_size != NULL) {
        UndockedSize *sz = g_hash_table_lookup (dock.id_to_undock_size, id);
        if (sz != NULL) {
            int w = sz->w;
            int h = sz->h;
            g_hash_table_remove (dock.id_to_undock_size, id);

            hx_panel_undock (panel);

            /* hx_panel_undock has called gtk_window_present on the
             * new top-level by now. set_default_size on a mapped
             * window resizes the surface on the next reconfigure
             * — works on both X11 and Wayland in our experience.
             * If the size doesn't take on some compositor, that's
             * a follow-up. */
            GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (panel));
            if (GTK_IS_WINDOW (root)) {
                gtk_window_set_default_size (GTK_WINDOW (root), w, h);
            }
        }
    }
}

/* ----------------------------------------------------------------- */
/* Reset                                                             */
/* ----------------------------------------------------------------- */

void
dock_layout_reset (void)
{
    char *path = layout_file_path ();
    if (g_unlink (path) != 0 && errno != ENOENT) {
        g_warning ("dock_layout: unlink %s: %s", path, g_strerror (errno));
    }
    g_free (path);

    if (dock.id_to_frame != NULL) {
        g_hash_table_remove_all (dock.id_to_frame);
    }
    if (dock.id_to_undock_size != NULL) {
        g_hash_table_remove_all (dock.id_to_undock_size);
    }
    dock.loaded = FALSE;

    if (dock.save_idle_id != 0) {
        g_source_remove (dock.save_idle_id);
        dock.save_idle_id = 0;
    }

    /* Suppress all further saves this session. Without this, any
     * subsequent notify::position from a divider drag (or DnD,
     * resize, etc.) would re-create the file we just deleted —
     * and on next launch the user would get whatever was in flight
     * at the moment of reset, not the defaults. Matches the toast
     * the action posts: "Layout will reset on next launch." */
    dock.save_disabled = TRUE;
}

/* ----------------------------------------------------------------- */
/* Lifecycle                                                         */
/* ----------------------------------------------------------------- */

void
dock_layout_init (void)
{
    if (dock.id_to_frame == NULL) {
        dock.id_to_frame
            = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    }
}

void
dock_layout_shutdown (void)
{
    if (dock.save_idle_id != 0) {
        /* Flush pending save synchronously before quit. */
        g_source_remove (dock.save_idle_id);
        dock.save_idle_id = 0;
        on_save_idle (NULL);
    }
}
