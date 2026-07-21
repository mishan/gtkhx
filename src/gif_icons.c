/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#include "config.h"
#include <gtk/gtk.h>  /* tasks.h references GtkWidget */
#include <glib/gstdio.h> /* g_unlink */
#include <errno.h>
#include "compat.h"   /* PACKED — required before hotline.h */
#include "hotline.h"
#include "protocol.h" /* struct htlc_conn, RCV_TASK_FN */
#include "hxconn.h"
#include "proto_helpers.h" /* struct hx_chunk */
#include "hotline_proto.h"
#include "network.h"  /* hlwrite_chunks */
#include "gtkhx_session.h" /* `session` typedef needed by tasks.h */
#include "session.h"  /* sess_from_htlc (for gtask_delete_tsk on probe timeout) */
#include "tasks.h"    /* task_new, gtask_delete_tsk */
#include "rcv.h"      /* rcv_task_icon_get / _getlist */
#include "gif_icons.h"
#include "debug.h"

/* Watchdog window for the probe. Generous enough for a slow server,
 * short enough not to leave the UI guessing. Mirrors the tracker-v3
 * probe's order of magnitude. */
#define GIF_ICONS_PROBE_TIMEOUT_S 2

void
hx_icon_getlist (struct htlc_conn *htlc)
{
    if (!htlc) {
        return;
    }
    /* ICON_GETLIST is a zero-chunk request. */
    task_new (htlc, RCV_TASK_FN (rcv_task_icon_getlist), NULL, 0, "icon-list");
    hlwrite_chunks (htlc, HTLC_HDR_ICON_GETLIST, 0, NULL, 0);
}

static gboolean
gif_icons_probe_timeout (gpointer data)
{
    struct htlc_conn *htlc = data;

    hx_conn_set_gif_icons_probe_timer (htlc, 0);
    if (hx_conn_gif_icons_state (htlc) == GIF_ICONS_UNKNOWN) {
        hx_conn_set_gif_icons_state (htlc, GIF_ICONS_UNSUPPORTED);
        debug_log ("icon",
                   "GIF-icons probe timed out; server appears not to "
                   "support the extension");
        /* Dismiss the probe's Tasks-window row. A legacy server drops
		 * the unknown ICON_GETLIST opcode with no reply, so the task
		 * would otherwise sit in the UI forever. We only remove the
		 * gtask row (not the model task), so a late reply — slow
		 * server, not an unsupporting one — still dispatches through
		 * hx_rcv_task -> rcv_task_icon_getlist and loads avatars. */
        gtask_delete_tsk (sess_from_htlc (htlc), hx_conn_gif_icons_probe_trans (htlc));
    }
    return G_SOURCE_REMOVE;
}

void
hx_icon_probe (struct htlc_conn *htlc)
{
    if (!htlc) {
        return;
    }
    hx_conn_set_gif_icons_state (htlc, GIF_ICONS_UNKNOWN);
    if (hx_conn_gif_icons_probe_timer (htlc)) {
        g_source_remove (hx_conn_gif_icons_probe_timer (htlc));
    }
    /* hx_icon_getlist's task_new snapshots htlc->trans (the increment
	 * happens later inside hlwrite_chunks), so the trans the probe task
	 * is keyed on is htlc->trans right now. Stash it for the watchdog. */
    hx_conn_set_gif_icons_probe_trans (htlc, hx_conn_trans (htlc));
    hx_icon_getlist (htlc);
    hx_conn_set_gif_icons_probe_timer (
        htlc, g_timeout_add_seconds (GIF_ICONS_PROBE_TIMEOUT_S,
                                     gif_icons_probe_timeout, htlc));
    debug_log ("icon", "GIF-icons probe sent (ICON_GETLIST), watchdog armed");
}

void
hx_icon_get (struct htlc_conn *htlc, guint16 uid)
{
    if (!htlc) {
        return;
    }
    struct hx_chunk chunks[1];
    guint8 scratch[2];
    int hc = (int) gtkhx_proto_build_icon_get_chunks (
        uid, chunks, G_N_ELEMENTS (chunks), scratch, sizeof (scratch));
    if (hc > 0) {
        task_new (htlc, RCV_TASK_FN (rcv_task_icon_get),
                  GUINT_TO_POINTER ((guint) uid), 0, "icon-get");
        hlwrite_chunks (htlc, HTLC_HDR_ICON_GET, 0, chunks, hc);
    }
}

void
hx_icon_set (struct htlc_conn *htlc, const guint8 *gif, gsize len)
{
    if (!htlc) {
        return;
    }
    /* A non-empty payload must be a real GIF — the server validates
	 * the signature and rejects non-GIF uploads, so mirror that
	 * client-side rather than earn a task error. A clear (len == 0)
	 * is always allowed. */
    if (len > 0 && !gtkhx_proto_gif_icon_is_gif (gif, len)) {
        debug_log ("icon",
                   "refusing ICON_SET: payload is not a GIF (%zu bytes)",
                   (size_t) len);
        return;
    }
    struct hx_chunk chunks[1];
    int hc = (int) gtkhx_proto_build_icon_set_chunks (gif, len, chunks,
                                                      G_N_ELEMENTS (chunks));
    if (hc <= 0) {
        /* Builder rejected it — the only failure for a validated GIF is
		 * exceeding the u16 wire-length limit. Log + bail so an oversize
		 * upload is diagnosable rather than a silent no-op that looks
		 * like a hang. */
        debug_log ("icon",
                   "ICON_SET not sent: builder rejected %zu-byte payload "
                   "(over the 64 KiB wire limit?)",
                   (size_t) len);
        return;
    }
    /* The reply is a bare task completion with no payload; we don't
	 * register a task handler — an unmatched TASK reply is handled
	 * benignly by hx_rcv_task, and a task error surfaces through its
	 * standard error path. */
    hlwrite_chunks (htlc, HTLC_HDR_ICON_SET, 0, chunks, hc);
}

void
hx_icon_clear (struct htlc_conn *htlc)
{
    hx_icon_set (htlc, NULL, 0);
}

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
        && (guint64) st.st_size <= GTKHX_AVATAR_MAX_BYTES) {
        char *data = NULL;
        gsize len = 0;
        if (g_file_get_contents (path, &data, &len, NULL)
            && avatar_bytes_valid ((const guint8 *) data, len)) {
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
        = g_file_set_contents (path, (const char *) gif, (gssize) len, &err);
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
        g_warning ("hx_icon_forget: could not delete saved avatar %s: %s",
                   path, g_strerror (errno));
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
    hx_icon_set (htlc, gif, len);
    debug_log ("icon", "sent saved avatar (%zu bytes) to capable server",
               (size_t) len);
}
