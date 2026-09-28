/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/* The GIF avatar cache (the GIF-icons extension), implemented in Rust
 * (gtkhx-ui's avatar module).
 *
 * Holds each user's decoded avatar, the source the user-list cells and the
 * chat view draw from. Decoding runs through the bounded inline-media
 * loader, so a hostile GIF can't stall the UI or escape the size caps.
 * Animated GIFs keep every frame; a shared timer animates the avatars that
 * are on screen, subject to the "animate avatars" preference and a per-user
 * pause (click an animated avatar, or the right-click menu).
 *
 * View-side: model code never calls it. The GtkhxSession gif-icon handlers
 * in gtkhx.c are the entry points. */

#ifndef GTKHX_GIF_AVATAR_H
#define GTKHX_GIF_AVATAR_H

#include <glib.h>

/* Opaque here — the avatar tables key on (connection, uid) and only ever
 * pass the pointer to hx_conn_serial. */
struct htlc_conn;

typedef struct _GdkTexture GdkTexture;
typedef struct _GdkPaintable GdkPaintable;

/* The frame of `uid`'s avatar to show right now, or NULL if there is none
 * (or its decode is in flight, or failed). The first frame while animation
 * is off. Borrowed. */
GdkTexture *gtkhx_avatar_get (struct htlc_conn *htlc, guint16 uid);

/* `uid`'s avatar as a paintable that animates itself while it is drawn,
 * invalidating its contents at each frame; NULL if there is none. The same
 * object for as long as the avatar is unchanged. Borrowed. */
GdkPaintable *gtkhx_avatar_get_paintable (struct htlc_conn *htlc, guint16 uid);

/* Ingest a raw GIF payload for `uid`. `len == 0` (or NULL `gif`) is a
 * clear — the cached avatar is dropped immediately. A non-empty
 * payload is decoded asynchronously; on success the avatar replaces
 * any cached one. Either way the affected user-list rows are refreshed
 * (synchronously for a clear, on decode completion otherwise). A
 * second call for the same uid cancels an in-flight decode. */
void gtkhx_avatar_update (struct htlc_conn *htlc, guint16 uid,
                          const guint8 *gif, gsize len);

/* Drop one connection's avatars — its cache entries and any decode still in
 * flight for it. Other connections are untouched.
 *
 * A uid is only unique within a connection, so the tables are keyed on the
 * pair. This used to be a clear-all, which meant one server's user list going
 * away wiped every server's faces. */
void gtkhx_avatar_clear_conn (struct htlc_conn *htlc);

/* Global on/off for avatar animation (CFG_ANIMATE_AVATARS). When off,
 * every avatar shows its still first frame and the frame timer stops.
 * options.c calls this from the pref's changefunc. */
void gtkhx_avatar_set_animation_enabled (gboolean enabled);

/* True if `uid` has a cached *animated* (multi-frame) avatar. Drives
 * the click-to-pause affordance + the right-click menu item visibility. */
gboolean gtkhx_avatar_is_animated (struct htlc_conn *htlc, guint16 uid);

/* Per-user pause override. Independent of the global pref: a paused
 * avatar freezes on its current frame even when animation is enabled.
 * Set from the click-to-toggle gesture and the right-click menu. */
gboolean gtkhx_avatar_is_paused (struct htlc_conn *htlc, guint16 uid);
void gtkhx_avatar_set_paused (struct htlc_conn *htlc, guint16 uid,
                              gboolean paused);

#endif /* GTKHX_GIF_AVATAR_H */
