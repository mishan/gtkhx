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

#endif /* GTKHX_GIF_ICONS_H */
