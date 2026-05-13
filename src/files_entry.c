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
#include <glib/gi18n.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "files.h"           /* ICON_* defaults */
#include "files_entry.h"

struct _HxFileEntry {
	GObject  parent_instance;
	char    *name;
	gboolean is_dir;
	guint64  size;
	gint64   modified;
	char    *kind;
	guint16  icon_id;
};

G_DEFINE_FINAL_TYPE (HxFileEntry, hx_file_entry, G_TYPE_OBJECT)

static void
hx_file_entry_finalize (GObject *obj)
{
	HxFileEntry *e = HX_FILE_ENTRY (obj);
	g_free (e->name);
	g_free (e->kind);
	G_OBJECT_CLASS (hx_file_entry_parent_class)->finalize (obj);
}

static void
hx_file_entry_class_init (HxFileEntryClass *klass)
{
	G_OBJECT_CLASS (klass)->finalize = hx_file_entry_finalize;
}

static void
hx_file_entry_init (HxFileEntry *self)
{
	(void) self;
}

HxFileEntry *
hx_file_entry_new (const char *name,
                   gboolean    is_dir,
                   guint64     size,
                   gint64      modified,
                   const char *kind,
                   guint16     icon_id)
{
	HxFileEntry *e = g_object_new (HX_TYPE_FILE_ENTRY, NULL);
	e->name     = g_strdup (name ? name : "");
	e->is_dir   = is_dir;
	e->size     = size;
	e->modified = modified;
	e->kind     = g_strdup (kind ? kind : "");
	/* 0 = "caller didn't classify" — default to the generic icon
	 * appropriate for the kind. Saves both providers from spelling
	 * out the same fallback. */
	e->icon_id  = icon_id ? icon_id : (is_dir ? ICON_FOLDER : ICON_FILE);
	return e;
}

guint16
hx_file_entry_get_icon_id (HxFileEntry *e)
{
	return e ? e->icon_id : 0;
}

const char *
hx_file_entry_get_name (HxFileEntry *e)
{
	return e ? e->name : "";
}

gboolean
hx_file_entry_is_dir (HxFileEntry *e)
{
	return e ? e->is_dir : FALSE;
}

guint64
hx_file_entry_get_size (HxFileEntry *e)
{
	return e ? e->size : 0;
}

gint64
hx_file_entry_get_modified (HxFileEntry *e)
{
	return e ? e->modified : 0;
}

const char *
hx_file_entry_get_kind (HxFileEntry *e)
{
	return e ? e->kind : "";
}

/* Size column.
 *
 *   - directory  → "—"   (size is opaque on the local side; on
 *                         Hotline it's a child count but rendering
 *                         that here is wrong for the symmetric
 *                         local case, so keep simple)
 *   - 0 bytes    → "0 B"  (legitimate empty file, not "unknown")
 *   - everything → "N B" / "N.N KB" / "N.N MB" / etc.
 *
 * Mirrors g_format_size's behaviour but goes through GLib so we
 * get locale-aware decimal separators and 1024-based units. */
char *
hx_file_entry_format_size (HxFileEntry *e)
{
	if (!e || e->is_dir)
		return g_strdup ("—");
	return g_format_size_full (e->size,
		G_FORMAT_SIZE_IEC_UNITS | G_FORMAT_SIZE_LONG_FORMAT);
}

/* Modified-time column.
 *
 *   - within the last 24h → "HH:MM"
 *   - within this year     → "Mon DD"
 *   - older                → "YYYY-MM-DD"
 *   - 0                    → "" (unknown)
 *
 * Mirrors what most modern file managers do — readable at a
 * glance, short enough to fit, full year only when ambiguous. */
char *
hx_file_entry_format_modified (HxFileEntry *e)
{
	GDateTime *dt, *now;
	GTimeSpan delta;
	int year_dt, year_now;
	char *out;

	if (!e || e->modified <= 0)
		return g_strdup ("");

	dt = g_date_time_new_from_unix_local (e->modified);
	if (!dt)
		return g_strdup ("");
	now = g_date_time_new_now_local ();
	delta = g_date_time_difference (now, dt);
	year_dt  = g_date_time_get_year (dt);
	year_now = g_date_time_get_year (now);

	if (delta >= 0 && delta < G_TIME_SPAN_DAY)
		out = g_date_time_format (dt, "%H:%M");
	else if (year_dt == year_now)
		out = g_date_time_format (dt, "%b %e");
	else
		out = g_date_time_format (dt, "%Y-%m-%d");

	g_date_time_unref (dt);
	g_date_time_unref (now);
	return out ? out : g_strdup ("");
}
