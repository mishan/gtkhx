/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * dock_layout.c — the main window's layout file. See dock_layout.h for
 * the format and contract.
 */

#include "config.h"

#include "dock_layout.h"
#include "dock_layout_parse.h"

#include "hx.h" /* gtkhx_prefs, for the window size an import measures by */
#include "debug.h"

#include <stdio.h> /* sscanf */
#include <string.h>

extern const char *gtkhx_config_dir (void);

static struct {
    char *layout;             /* the dock's JSON, as it last asked to keep */
    GHashTable *bare_ids;     /* char* set of panel ids whose action row
                               * the user has hidden */
    gboolean toolbar_shown;   /* the main window's pixmap toolbar */
    gboolean pane_titles;     /* tab strips rather than corner controls */
    GHashTable *window_sizes; /* char* name → "W,H"; windows that
                               * aren't panels (Files) */
    guint save_id;
} dock = { 0 };

static const char *LAYOUT_FILE = "dock-layout.ini";
static const guint SAVE_DEBOUNCE_MS = 200;

static char *
layout_file_path (void)
{
    return g_build_filename (gtkhx_config_dir (), LAYOUT_FILE, NULL);
}

static void
ensure_tables (void)
{
    if (dock.bare_ids == NULL) {
        dock.bare_ids
            = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    }
    if (dock.window_sizes == NULL) {
        dock.window_sizes
            = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
    }
}

/* ----------------------------------------------------------------- */
/* Save (debounced)                                                  */
/* ----------------------------------------------------------------- */

static void
serialize_chrome (GKeyFile *kf)
{
    /* These two are written when *on*: both are off by default. */
    if (dock.toolbar_shown) {
        g_key_file_set_boolean (kf, "Chrome", "toolbar", TRUE);
    }
    if (dock.pane_titles) {
        g_key_file_set_boolean (kf, "Chrome", "pane-titles", TRUE);
    }
    if (dock.window_sizes != NULL) {
        GHashTableIter it;
        gpointer k, v;

        g_hash_table_iter_init (&it, dock.window_sizes);
        while (g_hash_table_iter_next (&it, &k, &v)) {
            g_key_file_set_string (kf, "Windows", k, v);
        }
    }
    if (dock.bare_ids != NULL && g_hash_table_size (dock.bare_ids) > 0) {
        /* Sorted, so the file doesn't churn with hash order. */
        GList *ids = g_list_sort (g_hash_table_get_keys (dock.bare_ids),
                                  (GCompareFunc)g_strcmp0);
        GString *joined = g_string_new (NULL);

        for (GList *l = ids; l != NULL; l = l->next) {
            if (joined->len > 0) {
                g_string_append_c (joined, ';');
            }
            g_string_append (joined, l->data);
        }
        g_key_file_set_string (kf, "Chrome", "hidden-actions", joined->str);
        g_string_free (joined, TRUE);
        g_list_free (ids);
    }
}

static gboolean
save_now (gpointer user_data)
{
    GKeyFile *kf = g_key_file_new ();
    char *path = layout_file_path ();
    char *data;
    gsize len = 0;
    GError *err = NULL;

    (void)user_data;
    dock.save_id = 0;

    if (dock.layout != NULL) {
        g_key_file_set_string (kf, "Dock", "layout", dock.layout);
    }
    serialize_chrome (kf);

    data = g_key_file_to_data (kf, &len, NULL);
    if (!g_file_set_contents (path, data, (gssize)len, &err)) {
        g_warning ("dock_layout: write %s: %s", path, err ? err->message : "?");
        g_clear_error (&err);
    } else {
        debug_log ("layout", "saved: %s", path);
    }

    g_free (data);
    g_free (path);
    g_key_file_unref (kf);

    return G_SOURCE_REMOVE;
}

void
dock_layout_request_save (void)
{
    /* Debounce, not throttle: every request resets the timer, so a burst
     * collapses to one write 200 ms after the last request. */
    if (dock.save_id != 0) {
        g_source_remove (dock.save_id);
    }
    dock.save_id = g_timeout_add (SAVE_DEBOUNCE_MS, save_now, NULL);
}

void
dock_layout_keep (const char *json)
{
    g_free (dock.layout);
    dock.layout = g_strdup (json);
    dock_layout_request_save ();
}

/* ----------------------------------------------------------------- */
/* Load                                                              */
/* ----------------------------------------------------------------- */

/* Read [Chrome] and [Windows]. Independent of the layout: a file whose
 * layout is missing or malformed still carries the user's choice of
 * bars. */
static void
load_chrome (GKeyFile *kf)
{
    g_autofree char *bare = NULL;
    g_auto (GStrv) keys = NULL;

    ensure_tables ();
    dock.toolbar_shown = g_key_file_get_boolean (kf, "Chrome", "toolbar", NULL);
    dock.pane_titles
        = g_key_file_get_boolean (kf, "Chrome", "pane-titles", NULL);

    g_hash_table_remove_all (dock.window_sizes);
    keys = g_key_file_get_keys (kf, "Windows", NULL, NULL);
    for (char **k = keys; k != NULL && *k != NULL; k++) {
        char *v = g_key_file_get_string (kf, "Windows", *k, NULL);
        if (v != NULL) {
            g_hash_table_insert (dock.window_sizes, g_strdup (*k), v);
        }
    }

    g_hash_table_remove_all (dock.bare_ids);
    bare = g_key_file_get_string (kf, "Chrome", "hidden-actions", NULL);
    if (bare != NULL) {
        g_auto (GStrv) ids = g_strsplit (bare, ";", -1);
        for (char **id = ids; *id != NULL; id++) {
            if (**id != '\0') {
                g_hash_table_add (dock.bare_ids, g_strdup (*id));
            }
        }
    }
}

/* A file from before the dock was mullion-gtk: its tree, dividers,
 * closed panels and undocked windows, as the dock's JSON. The dividers
 * were pixels, and are read as shares of the main window's saved size. */
static char *
import_legacy (GKeyFile *kf)
{
    g_autofree char *tree = g_key_file_get_string (kf, "Dock", "tree", NULL);
    g_autofree char *sizes = g_key_file_get_string (kf, "Dock", "sizes", NULL);
    g_autofree char *closed
        = g_key_file_get_string (kf, "Dock", "closed", NULL);
    g_auto (GStrv) keys = g_key_file_get_keys (kf, "Undocked", NULL, NULL);
    GPtrArray *undocked = g_ptr_array_new_with_free_func (g_free);
    char *json;

    if (tree == NULL) {
        g_ptr_array_unref (undocked);
        return NULL;
    }

    for (char **k = keys; k != NULL && *k != NULL; k++) {
        char *v = g_key_file_get_string (kf, "Undocked", *k, NULL);
        if (v != NULL) {
            g_ptr_array_add (undocked, g_strdup (*k));
            g_ptr_array_add (undocked, v);
        }
    }
    g_ptr_array_add (undocked, NULL);

    json = dl_import_legacy (tree, sizes, closed, (char **)undocked->pdata,
                             gtkhx_prefs.geo.tool.xsize,
                             gtkhx_prefs.geo.tool.ysize);
    g_ptr_array_unref (undocked);

    if (json == NULL) {
        g_warning ("dock_layout: the saved tree does not parse; "
                   "the default layout comes up");
    } else {
        debug_log ("layout", "imported the layout from before mullion-gtk");
    }

    return json;
}

char *
dock_layout_load (void)
{
    char *path = layout_file_path ();
    GKeyFile *kf = g_key_file_new ();
    GError *err = NULL;
    char *json = NULL;

    ensure_tables ();

    if (!g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, &err)) {
        if (!g_error_matches (err, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
            g_warning ("dock_layout: load %s: %s", path,
                       err ? err->message : "?");
        }
        g_clear_error (&err);
        goto out;
    }

    load_chrome (kf);

    json = g_key_file_get_string (kf, "Dock", "layout", NULL);
    if (json == NULL) {
        json = import_legacy (kf);
    }

    /* What the dock is kept as until it asks for something else: a
     * layout that loads is not a change, and a file saved before then --
     * a chrome toggle -- keeps it. */
    g_free (dock.layout);
    dock.layout = g_strdup (json);
    debug_log ("layout", "loaded: %s", path);

out:
    g_key_file_unref (kf);
    g_free (path);
    return json;
}

/* ----------------------------------------------------------------- */
/* Chrome                                                            */
/* ----------------------------------------------------------------- */

gboolean
dock_layout_panel_actions_hidden (const char *id)
{
    return id != NULL && dock.bare_ids != NULL
           && g_hash_table_contains (dock.bare_ids, id);
}

void
dock_layout_set_panel_actions_hidden (const char *id, gboolean hidden)
{
    g_return_if_fail (id != NULL);

    ensure_tables ();
    if (hidden) {
        g_hash_table_add (dock.bare_ids, g_strdup (id));
    } else {
        g_hash_table_remove (dock.bare_ids, id);
    }
    dock_layout_request_save ();
}

gboolean
dock_layout_toolbar_visible (void)
{
    return dock.toolbar_shown;
}

void
dock_layout_set_toolbar_visible (gboolean visible)
{
    dock.toolbar_shown = visible;
    dock_layout_request_save ();
}

gboolean
dock_layout_pane_titles_visible (void)
{
    return dock.pane_titles;
}

void
dock_layout_set_pane_titles_visible (gboolean visible)
{
    dock.pane_titles = visible;
    dock_layout_request_save ();
}

gboolean
dock_layout_get_window_size (const char *name, int *w, int *h)
{
    const char *v;
    int sw = 0, sh = 0;

    if (name == NULL || dock.window_sizes == NULL) {
        return FALSE;
    }
    v = g_hash_table_lookup (dock.window_sizes, name);
    if (v == NULL || sscanf (v, "%d,%d", &sw, &sh) != 2 || sw <= 0 || sh <= 0) {
        return FALSE;
    }
    *w = sw;
    *h = sh;
    return TRUE;
}

void
dock_layout_set_window_size (const char *name, int w, int h)
{
    g_return_if_fail (name != NULL);

    ensure_tables ();
    g_hash_table_insert (dock.window_sizes, g_strdup (name),
                         g_strdup_printf ("%d,%d", w, h));
    dock_layout_request_save ();
}

/* ----------------------------------------------------------------- */
/* Lifecycle                                                         */
/* ----------------------------------------------------------------- */

void
dock_layout_shutdown (void)
{
    if (dock.save_id != 0) {
        /* Flush a pending save before quit. */
        g_source_remove (dock.save_id);
        dock.save_id = 0;
        save_now (NULL);
    }
}
