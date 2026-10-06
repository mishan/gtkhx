/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#include "config.h"
#include <glib/gstdio.h> /* g_unlink */
#include <errno.h>
#include "compat.h" /* PACKED — required before hotline.h */
#include "hotline.h"
#include "protocol.h" /* struct htlc_conn */
#include "hxconn.h"
#include "hotline_proto.h"
#include "gif_icons.h"
#include "debug.h"

/* ---- Persisted avatar ($CONFIG/avatar.gif) ----------------------- */

extern const char *gtkhx_config_dir (void); /* gtkhx.c */

/* In-memory copy of the saved avatar (NULL = none). Backs both the
 * settings preview and the auto-send, so hx_icon_send_saved on the
 * receive path never touches the disk after the first load. The cache
 * only ever holds validated bytes (GIF signature + within the wire
 * length limit). `loaded` distinguishes "no avatar" from "not read from
 * disk yet". */
static GBytes *avatar_cache;
static gboolean avatar_cache_loaded;

static char *
avatar_store_path (void)
{
    return g_build_filename (gtkhx_config_dir (), "avatar.gif", NULL);
}

/* True if `gif`/`len` is a valid avatar GIF: a real GIF, within the
 * GTKHX_AVATAR_MAX_BYTES (32 KiB) cap the picker enforces. Holding the
 * persistence layer to the same cap keeps load + auto-send in step with
 * the UI — a hand-edited or legacy avatar.gif above it is rejected here
 * rather than auto-sent and bounced by the server. */
static gboolean
avatar_bytes_valid (const guint8 *gif, gsize len)
{
    return gif && len > 0 && len <= GTKHX_AVATAR_MAX_BYTES
           && gtkhx_proto_gif_icon_is_gif (gif, len);
}

/* Populate avatar_cache from disk on first use. A corrupt / oversize /
 * non-GIF file on disk is treated as "no avatar" (and left for the next
 * save to overwrite). */
static void
avatar_cache_ensure_loaded (void)
{
    if (avatar_cache_loaded) {
        return;
    }
    avatar_cache_loaded = TRUE;
    char *path = avatar_store_path ();
    /* Size preflight before slurping the file. A valid avatar is at most
     * GTKHX_AVATAR_MAX_BYTES, so anything larger can't be one — refuse to
     * read it rather than allocating a large buffer for a file that's
     * accidentally or maliciously oversized (and which avatar_bytes_valid
     * would reject afterwards anyway). */
    GStatBuf st;
    if (g_stat (path, &st) == 0 && st.st_size > 0
        && (guint64)st.st_size <= GTKHX_AVATAR_MAX_BYTES) {
        char *data = NULL;
        gsize len = 0;
        if (g_file_get_contents (path, &data, &len, NULL)
            && avatar_bytes_valid ((const guint8 *)data, len)) {
            avatar_cache = g_bytes_new_take (data, len); /* takes ownership */
        } else {
            g_free (data);
        }
    }
    g_free (path);
}

gboolean
hx_icon_save (const guint8 *gif, gsize len)
{
    /* Validate before persisting: a non-GIF or oversize blob would only
     * be rejected later by the server, so don't store it. */
    if (!avatar_bytes_valid (gif, len)) {
        return FALSE;
    }
    char *path = avatar_store_path ();
    GError *err = NULL;
    gboolean ok
        = g_file_set_contents (path, (const char *)gif, (gssize)len, &err);
    if (!ok) {
        debug_log ("icon", "failed to save avatar to %s: %s", path,
                   err ? err->message : "?");
        g_clear_error (&err);
    } else {
        /* Mirror into the cache so the receive-path send + the preview
         * see the new avatar without re-reading the disk. */
        g_clear_pointer (&avatar_cache, g_bytes_unref);
        avatar_cache = g_bytes_new (gif, len);
        avatar_cache_loaded = TRUE;
    }
    g_free (path);
    return ok;
}

gboolean
hx_icon_forget (void)
{
    char *path = avatar_store_path ();
    /* ENOENT just means there was nothing saved — that's a clean "gone".
     * Any other errno (permissions / I/O) leaves the file on disk where it
     * reappears (and re-sends) next start, so report failure: the in-memory
     * cache is cleared for this session, but the persisted avatar is NOT
     * gone, and the caller shouldn't claim it was. */
    gboolean removed = TRUE;
    if (g_unlink (path) != 0 && errno != ENOENT) {
        g_warning ("hx_icon_forget: could not delete saved avatar %s: %s", path,
                   g_strerror (errno));
        removed = FALSE;
    }
    g_free (path);
    g_clear_pointer (&avatar_cache, g_bytes_unref);
    avatar_cache_loaded = TRUE; /* "known: none" for this session */
    return removed;
}

GBytes *
hx_icon_load_saved (void)
{
    avatar_cache_ensure_loaded ();
    return avatar_cache ? g_bytes_ref (avatar_cache) : NULL;
}

void
hx_icon_send_saved (struct htlc_conn *htlc)
{
    if (!htlc || hx_conn_gif_icons_state (htlc) != GIF_ICONS_SUPPORTED) {
        return;
    }
    avatar_cache_ensure_loaded (); /* in-memory after first call */
    if (!avatar_cache) {
        return;
    }
    gsize len = 0;
    const guint8 *gif = g_bytes_get_data (avatar_cache, &len);
    /* Cache only ever holds validated bytes, but guard anyway so the
     * log line below is never a lie about an invalid send. */
    if (!avatar_bytes_valid (gif, len)) {
        return;
    }
    hx_icon_set_saved (htlc, gif, len);
    debug_log ("icon", "sent saved avatar (%zu bytes) to capable server",
               (size_t)len);
}
