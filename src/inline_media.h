/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * Inline-media extension client-side helpers (fogWraith
 * Capabilities-Inline-Media.md). Phase 9.A — wire-protocol layer
 * only.
 *
 * Capability bit is HTLC_CAP_INLINE_MEDIA (0x0008) in
 * DATA_CAPABILITIES. Servers advertise advisory limits
 * (DATA_CHAT_MEDIA_MAX_BYTES / _DIMENSION / _PIXELS /
 * _CHUNK_SIZE / _MAX_FRAMES / _MAX_DURATION_MS) in the LOGIN reply
 * when the cap is confirmed; hxhandlers' recv::login stashes them on
 * htlc->media_max_*.
 *
 * The upload and download are hxhandlers' media.rs; what is here is the
 * cap gate and the limits the attach flow checks a picture against.
 */

#ifndef HX_INLINE_MEDIA_H
#define HX_INLINE_MEDIA_H 1

#include <glib.h>
#include <stdbool.h>
#include <stdint.h>

#include "protocol.h"
#include "hxconn.h"

/* Defensive: every inline-media-send op is gated on the server
 * having echoed HTLC_CAP_INLINE_MEDIA. Sending an upload to a
 * server that didn't negotiate the cap earns a task-error per
 * spec; logging the skip is more useful than spamming the user
 * with toasts. Same convention as voice.c::voice_cap_ok and
 * chat_history.c.
 *
 * Returns TRUE when the cap is set, FALSE otherwise. Safe to call
 * with NULL htlc (returns FALSE). */
extern gboolean inline_media_cap_ok (struct htlc_conn *htlc);

/* Resolve a server-advisory limit to the effective value the
 * client should enforce.
 *
 * Gates on two things:
 *
 *   1. HTLC_CAP_INLINE_MEDIA being lit in hx_conn_caps (htlc) for the
 *      current session. struct htlc_conn is reused across
 *      reconnects: hx_conn_caps (htlc) gets overwritten by every LOGIN
 *      reply, but htlc->media_max_* aren't cleared at connect
 *      time. Without this gate a prior session's advertisement
 *      could leak into a new session against a server that
 *      doesn't echo the cap, leading the upload pre-flight to
 *      enforce caps the new server may not actually honour.
 *      When the cap isn't lit, return the spec default — the
 *      caller has no business uploading anyway, but the safer
 *      value is what we want.
 *
 *   2. htlc->media_max_* being non-zero. The LOGIN-reply chunk
 *      walker in rcv.c writes 0 for fields the server didn't
 *      advertise; spec recommends client-side fallback to
 *      HX_MEDIA_DEFAULT_* per missing field. 0 isn't a
 *      meaningful "explicit 0 cap" value here — every cap is in
 *      units (bytes / pixels / frames / ms) where 0 means "no
 *      image is small enough to satisfy this," which would
 *      block every upload. Treating 0 as "absent" is what every
 *      field is documented to mean in hotline.h.
 *
 * Phase 9.A keeps these inline accessors trivial; the Phase 9.C
 * upload-state-machine consumes them in the pre-flight UI step. */
static inline guint32
inline_media_max_bytes (const struct htlc_conn *htlc)
{
    if (!htlc || !(hx_conn_caps (htlc) & HTLC_CAP_INLINE_MEDIA)) {
        return HX_MEDIA_DEFAULT_MAX_BYTES;
    }
    guint32 v = hx_conn_media_max_bytes (htlc);
    return v ? v : HX_MEDIA_DEFAULT_MAX_BYTES;
}

static inline guint32
inline_media_max_dimension (const struct htlc_conn *htlc)
{
    if (!htlc || !(hx_conn_caps (htlc) & HTLC_CAP_INLINE_MEDIA)) {
        return HX_MEDIA_DEFAULT_MAX_DIMENSION;
    }
    guint32 v = hx_conn_media_max_dimension (htlc);
    return v ? v : HX_MEDIA_DEFAULT_MAX_DIMENSION;
}

static inline guint32
inline_media_max_pixels (const struct htlc_conn *htlc)
{
    if (!htlc || !(hx_conn_caps (htlc) & HTLC_CAP_INLINE_MEDIA)) {
        return HX_MEDIA_DEFAULT_MAX_PIXELS;
    }
    guint32 v = hx_conn_media_max_pixels (htlc);
    return v ? v : HX_MEDIA_DEFAULT_MAX_PIXELS;
}

static inline guint32
inline_media_max_frames (const struct htlc_conn *htlc)
{
    if (!htlc || !(hx_conn_caps (htlc) & HTLC_CAP_INLINE_MEDIA)) {
        return HX_MEDIA_DEFAULT_MAX_FRAMES;
    }
    guint32 v = hx_conn_media_max_frames (htlc);
    return v ? v : HX_MEDIA_DEFAULT_MAX_FRAMES;
}

static inline guint32
inline_media_max_duration_ms (const struct htlc_conn *htlc)
{
    if (!htlc || !(hx_conn_caps (htlc) & HTLC_CAP_INLINE_MEDIA)) {
        return HX_MEDIA_DEFAULT_MAX_DURATION_MS;
    }
    guint32 v = hx_conn_media_max_duration_ms (htlc);
    return v ? v : HX_MEDIA_DEFAULT_MAX_DURATION_MS;
}

#endif /* HX_INLINE_MEDIA_H */
