/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 * Public License for more details.
 */

#include "config.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <glib.h>
#include <glib/gprintf.h>
#include "debug.h"

/* GHashTable<const char *, GINT_TO_POINTER(1)>. NULL means "debug
 * subsystem not initialised yet, all categories disabled". A populated
 * table with the special key "all" means every category is enabled.
 *
 * The table is built once in debug_init() before any worker threads
 * exist, then read-only thereafter. No locking needed for the
 * read-side calls. */
static GHashTable *enabled_cats = NULL;

void
debug_init (void)
{
    const char *spec = g_getenv ("GTKHX_DEBUG");
    gchar **parts;
    int i;

    if (enabled_cats) {
        return;
    }

    enabled_cats = g_hash_table_new (g_str_hash, g_str_equal);
    if (!spec || !*spec) {
        return;
    }

    parts = g_strsplit (spec, ",", -1);
    for (i = 0; parts[i]; i++) {
        gchar *trim = g_strstrip (parts[i]);
        if (*trim) {
            /* The string slot is owned by `parts` for the
			 * duration of this loop; g_strdup so the table
			 * keeps a stable pointer after we strfreev. */
            g_hash_table_add (enabled_cats, g_strdup (trim));
        }
    }
    g_strfreev (parts);

    /* Surface the configuration once so the user can verify their
	 * env var was actually picked up — common mistake is setting
	 * GTKHX_DEBUG only in one shell and launching the app from
	 * another. */
    if (g_hash_table_size (enabled_cats) > 0) {
        g_printerr ("[debug] enabled categories: %s\n", spec);
    }
}

gboolean
debug_category_enabled (const char *cat)
{
    if (!enabled_cats || !cat) {
        return FALSE;
    }
    if (g_hash_table_contains (enabled_cats, "all")) {
        return TRUE;
    }
    return g_hash_table_contains (enabled_cats, cat);
}

void
debug_log (const char *cat, const char *fmt, ...)
{
    va_list ap;
    gsize fmtlen;

    if (!debug_category_enabled (cat)) {
        return;
    }

    g_printerr ("[%s] ", cat);
    va_start (ap, fmt);
    g_vfprintf (stderr, fmt, ap);
    va_end (ap);

    /* Auto-newline if the caller didn't supply one — most users
	 * forget, and unterminated lines fragment the log. */
    fmtlen = strlen (fmt);
    if (fmtlen == 0 || fmt[fmtlen - 1] != '\n') {
        g_printerr ("\n");
    }
}

/* Phase 5: tracing helper for the htlc->name corruption hunt. Each
 * write site to htlc->name calls this with a label naming the call
 * site, the bytes about to be copied, and their length. With
 * GTKHX_DEBUG=name set, a per-write line dumps the source label, the
 * length, the hex bytes, and the printable rendering — which lets us
 * see exactly which path stamps corrupt bytes into the buffer when
 * the bug recurs. Quiet when the category is off (single hash lookup
 * via debug_category_enabled). */
void
debug_log_name_write (const char *label, const char *src, gsize srclen)
{
    GString *hex;
    GString *printable;
    gsize i;

    if (!debug_category_enabled ("name")) {
        return;
    }

    hex = g_string_new (NULL);
    printable = g_string_new (NULL);
    for (i = 0; i < srclen; i++) {
        guint8 b = (guint8)src[i];
        if (i) {
            g_string_append_c (hex, ' ');
        }
        g_string_append_printf (hex, "%02x", b);
        if (b >= 0x20 && b < 0x7f) {
            g_string_append_c (printable, (char)b);
        } else {
            g_string_append_c (printable, '.');
        }
    }
    debug_log ("name", "write site '%s' len=%zu hex=[%s] printable='%s'",
               label ? label : "(null)", (size_t)srclen, hex->str,
               printable->str);
    g_string_free (hex, TRUE);
    g_string_free (printable, TRUE);
}
