/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * panel_registry.c — the static panel ids, and the "has been built"
 * latch.
 */

#include "config.h"

#include "panel_registry.h"

#include <glib.h>

/* String literals from the header, so nothing here is owned. */
const char *const hx_panel_static_ids[] = {
    HX_PANEL_ID_USERS,
    HX_PANEL_ID_TASKS,
    HX_PANEL_ID_NEWS,
    HX_PANEL_ID_CHAT,
    HX_PANEL_ID_NEWS15,
#ifdef HAVE_VOICE
    HX_PANEL_ID_VIDEO,
#endif
    NULL,
};

/* The "has ever been constructed" latch: it outlives a panel close. */
static GHashTable *
get_constructed (void)
{
    static GHashTable *table = NULL;
    if (G_UNLIKELY (table == NULL)) {
        /* Ids are string literals from panel_registry.h, so neither
         * key nor value is owned here. */
        table = g_hash_table_new (g_str_hash, g_str_equal);
    }
    return table;
}

void
hx_panel_mark_constructed (const char *id)
{
    g_return_if_fail (id != NULL);

    g_hash_table_add (get_constructed (), (gpointer)id);
}

gboolean
hx_panel_was_constructed (const char *id)
{
    g_return_val_if_fail (id != NULL, FALSE);

    return g_hash_table_contains (get_constructed (), id);
}
