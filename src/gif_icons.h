/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/* GIF-icons extension (fogWraith GIF-Icons.md) — client send path and
 * probe-and-fallback negotiation. Receive-side parsing lives in the
 * Rust hotline-proto crate (crate::gif_icons); the rcv handlers
 * (rcv_task_icon_get / _getlist, hx_rcv_icon_change in rcv.c) call
 * those parsers and emit GtkhxSession::gif-icon-* signals. */

#ifndef GTKHX_GIF_ICONS_H
#define GTKHX_GIF_ICONS_H

#include <glib.h>

struct htlc_conn;

/* Spec-recommended max avatar upload size (fogWraith GIF-Icons.md). This
 * is the single source of truth for the cap: the Settings picker rejects
 * larger files with it, and the persistence layer (hx_icon_save /
 * hx_icon_load_saved) enforces the same value so a hand-edited or
 * legacy $CONFIG/avatar.gif can't slip a larger payload past the UI and
 * earn a server rejection on auto-send. The u16 wire-length field would
 * permit up to 0xffff, but we hold to the 32 KiB recommendation. */
#define GTKHX_AVATAR_MAX_BYTES (32 * 1024)

/* Probe-and-fallback negotiation state (the extension defines no
 * capability bit). Stored in htlc->gif_icons_state. */
enum {
    GIF_ICONS_UNKNOWN = 0,     /* not probed yet                       */
    GIF_ICONS_SUPPORTED = 1,   /* an ICON_GETLIST/GET reply was seen   */
    GIF_ICONS_UNSUPPORTED = 2, /* probe watchdog fired with no reply   */
};

/* Post-login probe: send ICON_GETLIST and arm a watchdog timer. On a
 * reply the session flips to GIF_ICONS_SUPPORTED; if the watchdog
 * fires first it flips to GIF_ICONS_UNSUPPORTED. Safe against legacy
 * servers (one ignored transaction + a short timer). */
void hx_icon_probe (struct htlc_conn *htlc);

/* Request the full per-user avatar list (ICON_GETLIST / 1861). */
void hx_icon_getlist (struct htlc_conn *htlc);

/* Request one user's avatar (ICON_GET / 1863). */
void hx_icon_get (struct htlc_conn *htlc, guint16 uid);

/* Set our own avatar (ICON_SET / 1862). gif must be a valid GIF;
 * a non-GIF payload is rejected (no-op). */
void hx_icon_set (struct htlc_conn *htlc, const guint8 *gif, gsize len);

/* Clear our own avatar (ICON_SET with an empty payload). */
void hx_icon_clear (struct htlc_conn *htlc);

/* ---- Persisted avatar -------------------------------------------------
 *
 * The user's chosen GIF avatar is stored at $CONFIG/avatar.gif,
 * independent of any connection — they can pick one while offline (or
 * on a server that doesn't support the extension), and it's sent
 * automatically once a capable server is found (hx_icon_send_saved,
 * called from the post-login probe). This decouples "choose an avatar"
 * from "the current server supports avatars." */

/* Persist `gif` as the saved avatar. Validates the GIF signature and
 * rejects a non-GIF payload or one over GTKHX_AVATAR_MAX_BYTES (32 KiB) —
 * the same cap the Settings picker enforces. Returns TRUE on success
 * (validated + written to disk). */
gboolean hx_icon_save (const guint8 *gif, gsize len);

/* Forget the persisted avatar. Returns TRUE if the saved file is now gone
 * (deleted, or never existed), FALSE if it could not be removed (e.g.
 * permissions / I/O) and will reappear next start. The in-memory cache is
 * cleared either way. */
gboolean hx_icon_forget (void);

/* Read the saved avatar, or NULL if none is stored / it's invalid.
 * Caller owns the returned GBytes. Backed by an in-memory cache so the
 * disk is touched at most once per process. */
GBytes *hx_icon_load_saved (void);

/* If the session is GIF-icon-capable and a saved avatar exists, send
 * it (ICON_SET). Called from the post-login probe once support is
 * confirmed; a no-op otherwise. */
void hx_icon_send_saved (struct htlc_conn *htlc);

#endif /* GTKHX_GIF_ICONS_H */
