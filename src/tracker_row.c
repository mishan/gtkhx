/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "config.h"

#include <glib.h>

#include "tracker_row.h"

struct _HxTrackerRow {
    GObject parent_instance;
    char *name;
    char *desc;
    char *address;
    guint16 port;
    guint16 nusers;
    HxTrackerV3Meta *meta;
};

G_DEFINE_FINAL_TYPE (HxTrackerRow, hx_tracker_row, G_TYPE_OBJECT)

static void
hx_tracker_row_finalize (GObject *obj)
{
    HxTrackerRow *r = HX_TRACKER_ROW (obj);
    g_free (r->name);
    g_free (r->desc);
    g_free (r->address);
    hx_tracker_v3_meta_free (r->meta);
    G_OBJECT_CLASS (hx_tracker_row_parent_class)->finalize (obj);
}

static void
hx_tracker_row_class_init (HxTrackerRowClass *klass)
{
    G_OBJECT_CLASS (klass)->finalize = hx_tracker_row_finalize;
}

static void
hx_tracker_row_init (HxTrackerRow *self)
{
    (void)self;
}

HxTrackerRow *
hx_tracker_row_new_from_event (HxTrackerServer *event)
{
    HxTrackerRow *r;

    g_return_val_if_fail (event != NULL, NULL);
    g_return_val_if_fail (event->address != NULL, NULL);

    r = g_object_new (HX_TYPE_TRACKER_ROW, NULL);
    r->address = g_strdup (event->address);
    r->port = event->port;
    r->nusers = event->nusers;
    /* event->name / event->desc are UTF-8 and Pango-safe by the time
     * we get here: hx_tracker_server_new_v1 transcodes MacRoman →
     * UTF-8 for v1 records, and hx_tracker_server_new_v3 runs the
     * v3-side strings through g_utf8_make_valid. */
    r->name = g_strdup (event->name ? event->name : "");
    r->desc = g_strdup (event->desc ? event->desc : "");
    /* Deep copy the typed view so the row owns it. event->meta is
     * always non-NULL (the event constructors populate a zero-init
     * meta for v1 records too), so the copy is too. */
    r->meta = hx_tracker_v3_meta_copy (event->meta);
    return r;
}

const char *
hx_tracker_row_get_name (HxTrackerRow *r)
{
    return r ? r->name : "";
}

const char *
hx_tracker_row_get_desc (HxTrackerRow *r)
{
    return r ? r->desc : "";
}

const char *
hx_tracker_row_get_address (HxTrackerRow *r)
{
    return r ? r->address : "";
}

guint16
hx_tracker_row_get_port (HxTrackerRow *r)
{
    return r ? r->port : 0;
}

guint16
hx_tracker_row_get_nusers (HxTrackerRow *r)
{
    return r ? r->nusers : 0;
}

HxTrackerV3Meta *
hx_tracker_row_get_meta (HxTrackerRow *r)
{
    return r ? r->meta : NULL;
}
