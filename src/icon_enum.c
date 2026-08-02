/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * icon_enum.c — enumerate the available Hotline icon IDs, for the Settings
 * icon picker.
 *
 * The picker lives in Rust now; the icon *resources* do not. They are Mac
 * resource forks discovered at startup into `icon_files`, a plain C global
 * that carries an array of opened `macres_file *`. Rather than mirror that
 * struct across the language boundary, this hands Rust the one thing the
 * picker actually needs — the set of distinct IDs — and lets `load_icon` do
 * the lookup, which is the same call every other icon consumer in the tree
 * already makes.
 *
 * **That is the point, not just a convenience.** `load_icon` walks the files
 * in discovery order and takes the first match, so a user's own icon in
 * `$CONFIG/icons/` shadows the system one with the same ID. The picker used to
 * reimplement that rule with its own hash table, keeping the first occurrence
 * per ID across a second walk of the same files — a duplicate of a policy that
 * only needs to live in one place. Enumerating IDs here and resolving each
 * through `load_icon` deletes the duplicate: the picker cannot disagree with
 * what the user list will actually draw, because it asks the same function.
 *
 * The snapshot shape (begin / nth / end) mirrors `gtkhx_theme_names_*`, which
 * the already-ported Settings pages use for the same reason: a Rust caller
 * wants a count and indexed access, and the C side wants to own the buffer.
 */

#include "config.h"

#include <glib.h>
#include <gtk/gtk.h>

#include "cicn.h"
#include "gtkhx.h"
#include "icon_enum.h"
#include "session.h"

/* The live snapshot. One at a time: the picker builds its grid in one pass and
 * the dialog is modal, so there is no case for reentrancy here. */
static GArray *snapshot;

static gint
cmp_u16 (gconstpointer a, gconstpointer b)
{
    guint16 x = *(const guint16 *)a;
    guint16 y = *(const guint16 *)b;

    return (x > y) - (x < y);
}

int
hx_icon_ids_begin (void)
{
    GHashTable *seen;
    unsigned int i;

    hx_icon_ids_end ();

    snapshot = g_array_new (FALSE, FALSE, sizeof (guint16));
    seen = g_hash_table_new (g_direct_hash, g_direct_equal);

    for (i = 0; i < icon_files.n; i++) {
        guint16 nres;
        guint32 n;

        if (!icon_files.cicns[i]) {
            continue;
        }
        nres = macres_file_num_res_of_type (icon_files.cicns[i], TYPE_cicn);
        for (n = 0; n < nres; n++) {
            macres_res *r = macres_file_get_nth_res_of_type (
                icon_files.cicns[i], TYPE_cicn, n);
            if (!r) {
                continue;
            }
            /* Distinct IDs only. Which *file* wins is load_icon's business,
             * not ours — we are only saying the ID exists. */
            if (!g_hash_table_contains (seen,
                                        GUINT_TO_POINTER ((guint)r->resid))) {
                g_hash_table_add (seen, GUINT_TO_POINTER ((guint)r->resid));
                g_array_append_val (snapshot, r->resid);
            }
            /* macres_file_get_nth_res_of_type hands back a g_malloc'd
             * wrapper we own. */
            g_free (r);
        }
    }

    g_hash_table_destroy (seen);
    /* Sorted so the grid is stable between openings; the resource files are
     * walked in discovery order, which is not an order a user would expect to
     * see icons in. */
    g_array_sort (snapshot, cmp_u16);
    return (int)snapshot->len;
}

int
hx_icon_ids_nth (int i)
{
    if (!snapshot || i < 0 || (guint)i >= snapshot->len) {
        return -1;
    }
    return (int)g_array_index (snapshot, guint16, (guint)i);
}

void
hx_icon_ids_end (void)
{
    g_clear_pointer (&snapshot, g_array_unref);
}

GdkPixbuf *
hx_icon_pixbuf_for_id (int id, int fallback)
{
    GdkPixbuf *pixbuf = NULL;
    GdkPixbuf *mask_unused = NULL;

    if (id < 0 || id > G_MAXUINT16) {
        return NULL;
    }
    /* load_icon's `recurse` argument is the default-icon fallback: with it
     * set, an ID that no resource file carries resolves to DEFAULT_ICON
     * instead of nothing. See the header for which caller wants which. */
    load_icon (NULL, (guint16)id, &icon_files, fallback ? 1 : 0, &pixbuf,
               &mask_unused);
    return pixbuf;
}
