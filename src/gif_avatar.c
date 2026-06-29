/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#include "config.h"
#include <gtk/gtk.h>
#include "gif_avatar.h"
#include "inline_media_decode.h" /* inline_media_decode_async + caps */
#include "users.h"               /* users_refresh_avatar */
#include "debug.h"

/* uid (guint) -> GdkTexture *. Value destroy unrefs the texture. */
static GHashTable *avatar_cache;
/* uid (guint) -> in-flight decode cancel token. At most one decode per
 * uid is live; a new update for the same uid cancels the previous. */
static GHashTable *avatar_pending;

static void
ensure_tables (void)
{
    if (!avatar_cache) {
        avatar_cache = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                              NULL, g_object_unref);
    }
    if (!avatar_pending) {
        /* Token value destroy is inline_media_decode_cancel — removing
		 * an entry (supersede / clear) cancels the in-flight decode and
		 * frees the token in one step. */
        avatar_pending = g_hash_table_new_full (
            g_direct_hash, g_direct_equal, NULL,
            (GDestroyNotify) inline_media_decode_cancel);
    }
}

GdkTexture *
gtkhx_avatar_get (guint16 uid)
{
    if (!avatar_cache || uid == 0) {
        return NULL;
    }
    return g_hash_table_lookup (avatar_cache, GUINT_TO_POINTER ((guint) uid));
}

/* Avatar decode bounds. The wire length field already caps a GIF at
 * 64 KiB; we re-assert it plus dimension / pixel limits. GIF icons are
 * authored at the same sizes as the classic cicn icons — ~16 px for a
 * normal user icon, up to banner-width (a few hundred px) for the
 * wide-banner style (see HX_USER_WIDE_ICON_* in users_view.c). 1024 px
 * per axis comfortably covers a banner while still bounding a hostile
 * GIF that would otherwise decode to a huge canvas. The cell renders
 * the texture at its intrinsic size (clipped to the row), exactly like
 * a cicn icon, so the on-screen size is whatever the author chose. */
static const HxInlineMediaCaps avatar_caps = {
    .max_bytes = 64 * 1024,
    .max_dimension = 1024,
    .max_pixels = 1024 * 1024,
    /* 10.B renders a still — cap the decode at the first frame so an
	 * animated (or hostile) GIF isn't decoded frame-by-frame just to
	 * throw the rest away. (max_frames = 0 would fall back to the
	 * inline-media default of 150 frames / 15 s.) max_duration_ms is
	 * moot once the frame cap is 1. Animation arrives in 10.D. */
    .max_frames = 1,
    .max_duration_ms = 0,
};

static void
on_avatar_decoded (HxInlineMediaDecoded *result, gpointer user_data)
{
    /* The uid rides through user_data (GUINT_TO_POINTER) — no heap ctx.
	 * A heap ctx would leak whenever a decode is cancelled (supersede /
	 * clear-all), because inline_media_decode_cancel suppresses this
	 * callback, which is the only place a ctx would be freed. */
    guint16 uid = (guint16) GPOINTER_TO_UINT (user_data);

    /* The token for this uid has resolved — drop the pending entry.
	 * Removing it runs the value-destroy (inline_media_decode_cancel),
	 * the canonical free for the token: cancel-after-completion is a
	 * documented no-op that still releases our token ref. (Stealing
	 * instead would leak the token.) */
    if (avatar_pending) {
        g_hash_table_remove (avatar_pending, GUINT_TO_POINTER ((guint) uid));
    }

    if (result && result->texture) {
        ensure_tables ();
        /* Cache takes its own ref; the result keeps ownership of its
		 * texture until inline_media_decoded_free below. */
        g_hash_table_insert (avatar_cache, GUINT_TO_POINTER ((guint) uid),
                             g_object_ref (result->texture));
        debug_log ("icon", "avatar decoded for uid=%u", (unsigned) uid);
    } else {
        /* Decode failed (non-GIF, oversize, corrupt). Drop any stale
		 * avatar so the cell falls back to the standard icon. */
        if (avatar_cache) {
            g_hash_table_remove (avatar_cache, GUINT_TO_POINTER ((guint) uid));
        }
        debug_log ("icon", "avatar decode failed for uid=%u (code=%u)",
                   (unsigned) uid, result ? result->error_code : 0);
    }

    inline_media_decoded_free (result);
    users_refresh_avatar (uid);
}

void
gtkhx_avatar_update (guint16 uid, const guint8 *gif, gsize len)
{
    if (uid == 0) {
        return;
    }
    ensure_tables ();

    /* Cancel any in-flight decode for this uid (removing the entry
	 * invokes inline_media_decode_cancel via the value-destroy). */
    g_hash_table_remove (avatar_pending, GUINT_TO_POINTER ((guint) uid));

    if (!gif || len == 0) {
        /* Clear: the user dropped their avatar. */
        g_hash_table_remove (avatar_cache, GUINT_TO_POINTER ((guint) uid));
        users_refresh_avatar (uid);
        return;
    }

    gpointer token = inline_media_decode_async (
        gif, len, &avatar_caps, on_avatar_decoded,
        GUINT_TO_POINTER ((guint) uid));
    /* token == NULL means the decode synchronously rejected (and the
	 * callback already fired). Only track a live token. */
    if (token) {
        g_hash_table_insert (avatar_pending, GUINT_TO_POINTER ((guint) uid),
                             token);
    }
}

void
gtkhx_avatar_clear_all (void)
{
    /* Cancel in-flight decodes first (value-destroy cancels each token),
	 * so no late callback writes into a just-cleared cache. */
    if (avatar_pending) {
        g_hash_table_remove_all (avatar_pending);
    }
    if (avatar_cache) {
        g_hash_table_remove_all (avatar_cache);
    }
}
