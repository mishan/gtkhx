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
 * when the cap is confirmed; the parser in
 * rcv.c::rcv_task_login stashes them on htlc->media_max_*.
 *
 * Send-side helpers (Phase 9.C will add the actual upload state
 * machine and chat-with-attachment send) and receive-side
 * dispatch (Phase 9.D will add the placeholder-textentry hook and
 * the bytes-fetch path) build on the C wrappers below.
 *
 * Phase 9.A: just the cap gate + a per-session post-LOGIN logging
 * helper for the server's advertised limits. The Rust crate
 * (hotline-proto::inline_media) does the chunk shaping; src/
 * inline_media.c provides the thin C wrappers C dispatch sites
 * can call.
 */

#ifndef HX_INLINE_MEDIA_H
#define HX_INLINE_MEDIA_H 1

#include <glib.h>
#include <stdbool.h>
#include <stdint.h>

#include "protocol.h"

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
 * client should enforce: the LOGIN-reply value when the server
 * advertised it (non-zero check is insufficient because 0 is a
 * legitimate "explicit 0 cap" value; the parser writes 0 for
 * "absent" too, hence presence flags also kept on htlc — see
 * hotline.h's HX_MEDIA_DEFAULT_* and rcv.c).
 *
 * Phase 9.A keeps these inline accessors trivial; the Phase 9.C
 * upload-state-machine consumes them in the pre-flight UI step. */
static inline guint32
inline_media_max_bytes (const struct htlc_conn *htlc)
{
    if (!htlc) {
        return HX_MEDIA_DEFAULT_MAX_BYTES;
    }
    return htlc->media_max_bytes ? htlc->media_max_bytes
                                 : HX_MEDIA_DEFAULT_MAX_BYTES;
}

static inline guint32
inline_media_max_dimension (const struct htlc_conn *htlc)
{
    if (!htlc) {
        return HX_MEDIA_DEFAULT_MAX_DIMENSION;
    }
    return htlc->media_max_dimension ? htlc->media_max_dimension
                                     : HX_MEDIA_DEFAULT_MAX_DIMENSION;
}

static inline guint32
inline_media_max_pixels (const struct htlc_conn *htlc)
{
    if (!htlc) {
        return HX_MEDIA_DEFAULT_MAX_PIXELS;
    }
    return htlc->media_max_pixels ? htlc->media_max_pixels
                                  : HX_MEDIA_DEFAULT_MAX_PIXELS;
}

static inline guint32
inline_media_chunk_size (const struct htlc_conn *htlc)
{
    if (!htlc) {
        return HX_MEDIA_DEFAULT_CHUNK_SIZE;
    }
    guint32 v = htlc->media_chunk_size;
    if (v == 0) {
        return HX_MEDIA_DEFAULT_CHUNK_SIZE;
    }
    /* Clamp the server-advertised chunk size to a sane ceiling so
	 * a hostile server can't ask us to allocate absurdly large
	 * per-chunk buffers. 60000 leaves room for the chunk header
	 * (4 bytes) plus a few wrapper chunks (PART_INDEX / PART_FINAL
	 * / UPLOAD_TOKEN) inside the 65535-byte wire frame.
	 *
	 * The spec doesn't bound CHAT_MEDIA_CHUNK_SIZE explicitly;
	 * this clamp is documented in docs/inline-media-plan.md
	 * "Open questions". */
    if (v > HX_MEDIA_DEFAULT_CHUNK_SIZE) {
        v = HX_MEDIA_DEFAULT_CHUNK_SIZE;
    }
    return v;
}

static inline guint32
inline_media_max_frames (const struct htlc_conn *htlc)
{
    if (!htlc) {
        return HX_MEDIA_DEFAULT_MAX_FRAMES;
    }
    return htlc->media_max_frames ? htlc->media_max_frames
                                  : HX_MEDIA_DEFAULT_MAX_FRAMES;
}

static inline guint32
inline_media_max_duration_ms (const struct htlc_conn *htlc)
{
    if (!htlc) {
        return HX_MEDIA_DEFAULT_MAX_DURATION_MS;
    }
    return htlc->media_max_duration_ms ? htlc->media_max_duration_ms
                                       : HX_MEDIA_DEFAULT_MAX_DURATION_MS;
}

/* Log the server's advertised inline-media limits at LOGIN time.
 * Called from rcv.c::rcv_task_login after the HTLS_DATA_CAPABILITIES
 * echo confirms CAP_INLINE_MEDIA. Single-line debug_log("media",
 * ...) so the proto-trace category covers it without spamming the
 * INFO chat. */
extern void inline_media_log_advertised_limits (struct htlc_conn *htlc);

#endif /* HX_INLINE_MEDIA_H */
