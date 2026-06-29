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
#include "compat.h"   /* PACKED — required before hotline.h */
#include "hotline.h"
#include "protocol.h" /* struct htlc_conn, RCV_TASK_FN */
#include "proto_helpers.h" /* struct hx_chunk */
#include "hotline_proto.h"
#include "network.h"  /* hlwrite_chunks */
#include "gtkhx_session.h" /* `session` typedef needed by tasks.h */
#include "session.h"  /* the_session (for gtask_delete_tsk on probe timeout) */
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

    htlc->gif_icons_probe_timer = 0;
    if (htlc->gif_icons_state == GIF_ICONS_UNKNOWN) {
        htlc->gif_icons_state = GIF_ICONS_UNSUPPORTED;
        debug_log ("icon",
                   "GIF-icons probe timed out; server appears not to "
                   "support the extension");
        /* Dismiss the probe's Tasks-window row. A legacy server drops
		 * the unknown ICON_GETLIST opcode with no reply, so the task
		 * would otherwise sit in the UI forever. We only remove the
		 * gtask row (not the model task), so a late reply — slow
		 * server, not an unsupporting one — still dispatches through
		 * hx_rcv_task -> rcv_task_icon_getlist and loads avatars. */
        gtask_delete_tsk (&the_session, htlc->gif_icons_probe_trans);
    }
    return G_SOURCE_REMOVE;
}

void
hx_icon_probe (struct htlc_conn *htlc)
{
    if (!htlc) {
        return;
    }
    htlc->gif_icons_state = GIF_ICONS_UNKNOWN;
    if (htlc->gif_icons_probe_timer) {
        g_source_remove (htlc->gif_icons_probe_timer);
    }
    /* hx_icon_getlist's task_new snapshots htlc->trans (the increment
	 * happens later inside hlwrite_chunks), so the trans the probe task
	 * is keyed on is htlc->trans right now. Stash it for the watchdog. */
    htlc->gif_icons_probe_trans = htlc->trans;
    hx_icon_getlist (htlc);
    htlc->gif_icons_probe_timer = g_timeout_add_seconds (
        GIF_ICONS_PROBE_TIMEOUT_S, gif_icons_probe_timeout, htlc);
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
