/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

/*
 * files_entry.c — the two presentation formatters for a files-browser
 * row.
 *
 * The HxFileEntry GObject itself (its state, construction, get_type, and
 * the six field getters) now lives in the Rust hxfiles-entry crate
 * (rust/crates/hxfiles-entry/src/lib.rs), which exports the same
 * hx_file_entry_* C ABI declared in files_entry.h. These two formatters
 * stay in C: they are thin g_format_size_full / g_dngettext / GDateTime
 * i18n wrappers whose whole value is GLib's locale handling, and they
 * read the entry only through the public accessors — no struct access.
 */

#include "config.h"

#include <glib.h>
#include <glib/gi18n.h>
#include <time.h>

#include "files_entry.h"

/* Size column.
 *
 *   - directory with child count → "(N items)"  (Hotline carries
 *                         the count in HTLC_DATA_FILESIZE for
 *                         folder rows; the remote provider stores
 *                         it in the size field)
 *   - directory without count    → "—"  (local-FS folders, whose
 *                         byte-count would be a meaningless 4096
 *                         or similar)
 *   - 0 bytes    → "0 B"  (legitimate empty file, not "unknown")
 *   - everything → "N B" / "N.N KB" / "N.N MB" / etc.
 *
 * Mirrors g_format_size's behaviour but goes through GLib so we
 * get locale-aware decimal separators and 1024-based units. */
char *
hx_file_entry_format_size (HxFileEntry *e)
{
    if (!e) {
        return g_strdup ("—");
    }
    if (hx_file_entry_is_dir (e)) {
        guint64 size = hx_file_entry_get_size (e);
        if (size > 0) {
            return g_strdup_printf (
                g_dngettext (NULL, "(%" G_GUINT64_FORMAT " item)",
                             "(%" G_GUINT64_FORMAT " items)", (gulong)size),
                size);
        }
        return g_strdup ("—");
    }
    return g_format_size_full (hx_file_entry_get_size (e),
                               G_FORMAT_SIZE_IEC_UNITS
                                   | G_FORMAT_SIZE_LONG_FORMAT);
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
    gint64 modified;
    int year_dt, year_now;
    char *out;

    if (!e || (modified = hx_file_entry_get_modified (e)) <= 0) {
        return g_strdup ("");
    }

    dt = g_date_time_new_from_unix_local (modified);
    if (!dt) {
        return g_strdup ("");
    }
    now = g_date_time_new_now_local ();
    delta = g_date_time_difference (now, dt);
    year_dt = g_date_time_get_year (dt);
    year_now = g_date_time_get_year (now);

    if (delta >= 0 && delta < G_TIME_SPAN_DAY) {
        out = g_date_time_format (dt, "%H:%M");
    } else if (year_dt == year_now) {
        out = g_date_time_format (dt, "%b %e");
    } else {
        out = g_date_time_format (dt, "%Y-%m-%d");
    }

    g_date_time_unref (dt);
    g_date_time_unref (now);
    return out ? out : g_strdup ("");
}
