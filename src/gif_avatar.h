/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/* View-side per-uid GIF avatar cache (GIF-icons extension, Phase 10.B).
 *
 * Holds a decoded GdkTexture per user id — the source of truth the
 * user-list cells read at snapshot time (hx_user_cell_name). Decoding
 * runs through the bounded, sandboxed inline-media loader
 * (inline_media_decode_async, STRICT allowlist = JPEG/PNG/GIF), so a
 * hostile GIF can't stall the UI or escape the size/dimension caps.
 *
 * Phase 10.D: animated GIFs are decoded to all their frames and played
 * by a single shared frame timer (gtkhx_avatar_get returns the current
 * frame). Animation is gated on the global CFG_ANIMATE_AVATARS pref and
 * a per-user pause toggle (click an animated avatar, or the right-click
 * menu).
 *
 * This is a view-side module (it produces GdkTextures and pokes the
 * user-list views), so model code never calls it directly — the
 * GtkhxSession::gif-icon-data / gif-icon-changed handlers in gtkhx.c
 * are the entry points. */

#ifndef GTKHX_GIF_AVATAR_H
#define GTKHX_GIF_AVATAR_H

#include <glib.h>

typedef struct _GdkTexture GdkTexture;

/* The frame of `uid`'s avatar to display *right now*, or NULL if none
 * is cached (or its decode is still in flight / failed). For an
 * animated avatar this is the current frame (advanced by the shared
 * timer); when animation is globally off it's always the first frame.
 * Borrowed — the cell refs it for the lifetime of the binding. */
GdkTexture *gtkhx_avatar_get (guint16 uid);

/* Ingest a raw GIF payload for `uid`. `len == 0` (or NULL `gif`) is a
 * clear — the cached avatar is dropped immediately. A non-empty
 * payload is decoded asynchronously; on success the texture replaces
 * any cached one. Either way the affected user-list rows are refreshed
 * (synchronously for a clear, on decode completion otherwise). A
 * second call for the same uid cancels an in-flight decode. */
void gtkhx_avatar_update (guint16 uid, const guint8 *gif, gsize len);

/* Drop every cached avatar and cancel all in-flight decodes. Called
 * when the public user list is cleared (disconnect) — avatars are
 * per-session server-side, so a reconnect re-probes from scratch. */
void gtkhx_avatar_clear_all (void);

/* ---- Animation control (Phase 10.D) -------------------------------- */

/* Global on/off for avatar animation (CFG_ANIMATE_AVATARS). When off,
 * every avatar shows its still first frame and the frame timer stops.
 * options.c calls this from the pref's changefunc. */
void gtkhx_avatar_set_animation_enabled (gboolean enabled);

/* True if `uid` has a cached *animated* (multi-frame) avatar. Drives
 * the click-to-pause affordance + the right-click menu item visibility. */
gboolean gtkhx_avatar_is_animated (guint16 uid);

/* Per-user pause override. Independent of the global pref: a paused
 * avatar freezes on its current frame even when animation is enabled.
 * Set from the click-to-toggle gesture and the right-click menu. */
gboolean gtkhx_avatar_is_paused (guint16 uid);
void gtkhx_avatar_set_paused (guint16 uid, gboolean paused);

#endif /* GTKHX_GIF_AVATAR_H */
