/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * panel_registry.c — id → HxPanel hashtable, lazily created.
 *
 * Keyed on the panel's stable string id (g_str_hash / g_str_equal).
 * Values are strong refs on HxPanel — the registry guarantees the
 * panel survives a Frame remove during Undock without disposing
 * itself (a PanelFrame is the only "real" parent a PanelWidget
 * normally has; remove it and the widget hits a zero refcount
 * unless something else holds it).
 */

#include "config.h"

#include "panel_registry.h"
#include "dock_layout.h"

#include <glib.h>

static GHashTable *
get_table (void)
{
    static GHashTable *table = NULL;
    if (G_UNLIKELY (table == NULL)) {
        /* key destroy is NULL — the key is the same g_strdup-ed
         * string we stash inside HxPanel via hx_panel_new, which the
         * panel itself owns; the registry just borrows the pointer.
         * value destroy is g_object_unref so the registry's strong
         * ref is dropped when the entry is removed. */
        table = g_hash_table_new_full (g_str_hash, g_str_equal,
                                       NULL, g_object_unref);
    }
    return table;
}

void
hx_panel_registry_register (HxPanel *panel)
{
    const char *id;

    g_return_if_fail (HX_IS_PANEL (panel));

    id = hx_panel_get_id (panel);
    g_return_if_fail (id != NULL);

    /* g_hash_table_replace drops the old entry's strong ref before
     * inserting the new one; protects against a re-register on the
     * same id (which would otherwise leak the previous panel). */
    g_hash_table_replace (get_table (), (gpointer) id, g_object_ref (panel));

    /* If a saved layout is in effect, the panel may belong in a
     * different leaf than the factory just added it to. Reseat it
     * now, before the user sees the brief flash of the wrong
     * placement. No-op when no saved layout is loaded or when the
     * panel's id isn't mentioned in the layout. */
    dock_layout_place_panel (panel);
}

void
hx_panel_registry_unregister (const char *id)
{
    g_return_if_fail (id != NULL);

    g_hash_table_remove (get_table (), id);
}

HxPanel *
hx_panel_registry_lookup (const char *id)
{
    g_return_val_if_fail (id != NULL, NULL);

    return g_hash_table_lookup (get_table (), id);
}

void
hx_panel_registry_foreach (HxPanelRegistryForeachFunc func,
                           gpointer                   user_data)
{
    GHashTableIter iter;
    gpointer       value;

    g_return_if_fail (func != NULL);

    g_hash_table_iter_init (&iter, get_table ());
    while (g_hash_table_iter_next (&iter, NULL, &value))
        func (HX_PANEL (value), user_data);
}
