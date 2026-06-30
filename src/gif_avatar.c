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

/* One user's decoded avatar: one or more frames + their delays. A
 * single-frame avatar is a still. `cur` is the frame currently shown;
 * the global animation timer advances it. `paused` is the per-user
 * freeze override (click an animated avatar, or the right-click menu).
 *
 * The user-list cell only ever holds a ref to the *current frame*
 * texture (via gtkhx_avatar_get); this struct owns every frame, so
 * advancing `cur` and re-emitting the avatar is enough to animate —
 * the cell needs no animation state of its own. */
typedef struct {
    GPtrArray *frames;   /* GdkTexture* per frame (reffed); len >= 1 */
    GArray *delays;      /* guint32 ms per frame; len == frames->len */
    guint cur;           /* index of the frame currently displayed */
    gint64 cur_since_us; /* monotonic time `cur` became current */
    gboolean paused;     /* per-user freeze override */
} Avatar;

/* uid (guint) -> Avatar *. Value destroy frees the Avatar. */
static GHashTable *avatar_cache;
/* uid (guint) -> in-flight decode cancel token. At most one decode per
 * uid is live; a new update for the same uid cancels the previous. */
static GHashTable *avatar_pending;

/* Global "animate avatars" pref (CFG_ANIMATE_AVATARS). When FALSE every
 * avatar shows its still first frame. Default TRUE; options.c pushes the
 * loaded/edited value via gtkhx_avatar_set_animation_enabled. */
static gboolean anim_enabled = TRUE;
/* Single shared frame-advance timer; 0 when not running. One timer
 * drives every animated avatar — cheap at the handful-of-avatars scale
 * of a user list, and far simpler than per-cell frame clocks. */
static guint anim_timer_id;

/* How often the timer wakes to check whether any avatar's current frame
 * has outlived its delay. 60 ms ≈ 16 fps ceiling, plenty for GIFs
 * (whose frame delays are typically 50-200 ms) without busy-spinning. */
#define ANIM_TICK_MS 60

static void
avatar_free (gpointer p)
{
    Avatar *a = p;
    if (!a) {
        return;
    }
    g_ptr_array_unref (a->frames);
    g_array_unref (a->delays);
    g_free (a);
}

static void
ensure_tables (void)
{
    if (!avatar_cache) {
        avatar_cache = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                              NULL, avatar_free);
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

static gboolean
avatar_is_animated (const Avatar *a)
{
    return a && a->frames->len > 1;
}

GdkTexture *
gtkhx_avatar_get (guint16 uid)
{
    if (!avatar_cache || uid == 0) {
        return NULL;
    }
    Avatar *a = g_hash_table_lookup (avatar_cache, GUINT_TO_POINTER ((guint) uid));
    if (!a) {
        return NULL;
    }
    /* When animation is globally off, always present the first frame. */
    guint idx = anim_enabled ? a->cur : 0;
    return g_ptr_array_index (a->frames, idx);
}

/* ---- Animation timer ------------------------------------------------ */

static gboolean anim_tick (gpointer data);

/* True if at least one cached avatar is animated and not paused — i.e.
 * the timer has work to do (subject to anim_enabled). */
static gboolean
any_avatar_running (void)
{
    if (!anim_enabled || !avatar_cache) {
        return FALSE;
    }
    GHashTableIter iter;
    gpointer val;
    g_hash_table_iter_init (&iter, avatar_cache);
    while (g_hash_table_iter_next (&iter, NULL, &val)) {
        Avatar *a = val;
        if (avatar_is_animated (a) && !a->paused) {
            return TRUE;
        }
    }
    return FALSE;
}

/* Start the timer if there's animation to drive and it isn't already
 * running; stop it if there's nothing to do. Idempotent — call after any
 * state change (new avatar, pause toggle, pref flip, clear). */
static void
anim_timer_sync (void)
{
    gboolean want = any_avatar_running ();
    if (want && anim_timer_id == 0) {
        anim_timer_id = g_timeout_add (ANIM_TICK_MS, anim_tick, NULL);
    } else if (!want && anim_timer_id != 0) {
        g_source_remove (anim_timer_id);
        anim_timer_id = 0;
    }
}

static gboolean
anim_tick (gpointer data)
{
    (void) data;
    gint64 now = g_get_monotonic_time ();
    GHashTableIter iter;
    gpointer key, val;
    g_hash_table_iter_init (&iter, avatar_cache);
    while (g_hash_table_iter_next (&iter, &key, &val)) {
        Avatar *a = val;
        if (!avatar_is_animated (a) || a->paused) {
            continue;
        }
        guint32 delay = g_array_index (a->delays, guint32, a->cur);
        if (delay == 0) {
            delay = 100; /* defensive: a 0-delay frame would busy-loop */
        }
        if ((now - a->cur_since_us) < (gint64) delay * 1000) {
            continue; /* current frame hasn't outlived its delay yet */
        }
        a->cur = (a->cur + 1) % a->frames->len;
        a->cur_since_us = now;
        users_refresh_avatar (GPOINTER_TO_UINT (key));
    }
    /* If a pref/pause change left nothing to animate, stop. Returning
	 * G_SOURCE_REMOVE here would race anim_timer_sync's bookkeeping, so
	 * sync explicitly and report whether we kept the source. */
    if (any_avatar_running ()) {
        return G_SOURCE_CONTINUE;
    }
    anim_timer_id = 0;
    return G_SOURCE_REMOVE;
}

/* ---- Decode + cache ------------------------------------------------- */

/* Avatar decode bounds. The wire length field already caps a GIF at
 * 64 KiB; we re-assert it plus dimension / pixel limits. GIF icons are
 * authored at the same sizes as the classic cicn icons and render at the
 * same place in the user list: ~16-26 px tall for a normal user icon, up
 * to the wide-banner style whose on-screen width tops out around
 * HX_USER_WIDE_ICON_LEFT_PAD (200 px) in users_view.c. We never display
 * anything larger, so clamp each decoded frame to AVATAR_MAX_DIM (256 px,
 * a clean power of two with headroom over the 200 px banner).
 *
 * This matters because we cache one GdkTexture per frame and the per-axis
 * cap is checked once on the GIF's logical canvas (which every frame
 * composites onto), so AVATAR_MAX_DIM bounds *every* frame, not just the
 * first. At 256 px the worst-case frame is ~256 KiB (vs ~4 MiB at the old
 * 1024 px cap), a 16x cut — so even a maximally hostile GIF at the
 * AVATAR_MAX_FRAMES cap tops out around 64 MiB instead of spiking into the
 * gigabytes. An over-cap canvas is rejected outright (MEDIA_ERR_TOO_LARGE),
 * falling back to the numeric icon.
 *
 * 10.D animates, so we collect frames — still bounded at AVATAR_MAX_FRAMES
 * and 30 s of cumulative animation. */
#define AVATAR_MAX_DIM 256
#define AVATAR_MAX_FRAMES 256
static const HxInlineMediaCaps avatar_caps = {
    .max_bytes = 64 * 1024,
    .max_dimension = AVATAR_MAX_DIM,
    .max_pixels = AVATAR_MAX_DIM * AVATAR_MAX_DIM,
    .max_frames = AVATAR_MAX_FRAMES,
    .max_duration_ms = 30000,
};

/* Build an Avatar from a decode result. Uses the multi-frame array when
 * present (animated GIF), else the single still texture. Returns NULL
 * if there are no usable frames. */
static Avatar *
avatar_from_result (HxInlineMediaDecoded *result)
{
    if (!result) {
        return NULL;
    }
    Avatar *a = g_new0 (Avatar, 1);
    a->frames = g_ptr_array_new_with_free_func (g_object_unref);
    a->delays = g_array_new (FALSE, FALSE, sizeof (guint32));
    a->cur = 0;
    a->cur_since_us = g_get_monotonic_time ();
    a->paused = FALSE;

    if (result->frames && result->frames->len > 0) {
        for (guint i = 0; i < result->frames->len; i++) {
            HxInlineMediaFrame *f
                = &g_array_index (result->frames, HxInlineMediaFrame, i);
            if (!f->texture) {
                continue;
            }
            g_ptr_array_add (a->frames, g_object_ref (f->texture));
            guint32 d = f->delay_ms;
            g_array_append_val (a->delays, d);
        }
    } else if (result->texture) {
        g_ptr_array_add (a->frames, g_object_ref (result->texture));
        guint32 d = 0;
        g_array_append_val (a->delays, d);
    }

    if (a->frames->len == 0) {
        avatar_free (a);
        return NULL;
    }
    return a;
}

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
	 * documented no-op that still releases our token ref. */
    if (avatar_pending) {
        g_hash_table_remove (avatar_pending, GUINT_TO_POINTER ((guint) uid));
    }

    Avatar *a = avatar_from_result (result);
    if (a) {
        ensure_tables ();
        /* Insert replaces (and frees) any prior Avatar for this uid. */
        g_hash_table_insert (avatar_cache, GUINT_TO_POINTER ((guint) uid), a);
        debug_log ("icon", "avatar decoded for uid=%u (%u frame%s)",
                   (unsigned) uid, a->frames->len, a->frames->len == 1 ? "" : "s");
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
    anim_timer_sync ();
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
        anim_timer_sync ();
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
    anim_timer_sync (); /* nothing left to animate → stop the timer */
}

/* ---- Pref + per-user pause (Phase 10.D) ----------------------------- */

void
gtkhx_avatar_set_animation_enabled (gboolean enabled)
{
    if (anim_enabled == (enabled != FALSE)) {
        return;
    }
    anim_enabled = (enabled != FALSE);
    anim_timer_sync ();
    /* Repaint every animated avatar: off → snaps to the still first
	 * frame, on → resumes from each avatar's current frame. */
    if (avatar_cache) {
        gint64 now = g_get_monotonic_time ();
        GHashTableIter iter;
        gpointer key, val;
        g_hash_table_iter_init (&iter, avatar_cache);
        while (g_hash_table_iter_next (&iter, &key, &val)) {
            Avatar *a = val;
            if (avatar_is_animated (a)) {
                /* Re-enabling: restart the running frame's clock so the
				 * next tick measures from now, not from a timestamp left
				 * stale by the disabled span (same fix as resume-from-
				 * pause). Skip paused avatars — they keep their own clock. */
                if (anim_enabled && !a->paused) {
                    a->cur_since_us = now;
                }
                users_refresh_avatar (GPOINTER_TO_UINT (key));
            }
        }
    }
}

gboolean
gtkhx_avatar_is_animated (guint16 uid)
{
    /* UI-facing: when animation is globally off, treat avatars as stills
	 * so the click-to-pause gesture doesn't claim and the right-click
	 * "Pause/Resume" item doesn't appear — there's nothing animating to
	 * pause. (The internal avatar_is_animated, used by the timer, stays
	 * purely structural; any_avatar_running gates it on anim_enabled.) */
    if (!anim_enabled || !avatar_cache || uid == 0) {
        return FALSE;
    }
    return avatar_is_animated (
        g_hash_table_lookup (avatar_cache, GUINT_TO_POINTER ((guint) uid)));
}

gboolean
gtkhx_avatar_is_paused (guint16 uid)
{
    if (!avatar_cache || uid == 0) {
        return FALSE;
    }
    Avatar *a = g_hash_table_lookup (avatar_cache, GUINT_TO_POINTER ((guint) uid));
    return a ? a->paused : FALSE;
}

void
gtkhx_avatar_set_paused (guint16 uid, gboolean paused)
{
    if (!avatar_cache || uid == 0) {
        return;
    }
    Avatar *a = g_hash_table_lookup (avatar_cache, GUINT_TO_POINTER ((guint) uid));
    if (!a || a->paused == (paused != FALSE)) {
        return;
    }
    a->paused = (paused != FALSE);
    if (!a->paused) {
        /* Resuming: restart this frame's clock so it doesn't instantly
		 * jump (it may have sat paused well past its delay). */
        a->cur_since_us = g_get_monotonic_time ();
    }
    anim_timer_sync ();
    users_refresh_avatar (uid);
}
