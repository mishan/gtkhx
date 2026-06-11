/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * GStreamer-based voice runtime — C-facing FFI prototypes.
 *
 * Distinct from src/voice.h, which is the Phase 8.A wire-out path
 * (HTLC_HDR_VOICE_* send wrappers). This header surfaces the Rust
 * hxvoice-runtime crate to the C side: the audio pipeline +
 * device-enumeration + (eventually) the per-session opaque runtime
 * handle. Phase 8.B lands just one entry point — the GStreamer
 * subsystem initialiser — and Phase 8.C will grow the surface to
 * include the per-session handle (gtkhx_voice_runtime_new / _free /
 * _handle_join / _handle_leave / _handle_mute_toggle /
 * _handle_rcv_task) once the state machine and webrtcbin dispatch
 * land.
 *
 * FFI discipline: same as the rest of the workspace — hand-declared
 * `extern` blocks rather than cbindgen output. Signature drift
 * surfaces as a link-time undefined symbol.
 */

#ifndef _VOICE_RUNTIME_H
#define _VOICE_RUNTIME_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialise the GStreamer subsystem.
 *
 * Idempotent: gstreamer-rs's `gst::init` checks an internal
 * "already initialised" flag, so repeat calls are cheap no-ops.
 * Returns 1 on success, 0 on failure (no plugin path, missing
 * core plugins, broken GStreamer install). On failure GStreamer's
 * own diagnostics print to stderr; the caller decides whether to
 * disable voice UI or fail loudly.
 *
 * Call once during application startup, after gtk_init, before any
 * code path that might construct a GStreamer element. Per the
 * Phase 8.B exit criterion, no code path does that yet — the call
 * still goes in `main` so the lifecycle is honest by the time
 * Phase 8.D's voice toolbar arrives.
 */
extern int gtkhx_voice_init (void);

#ifdef __cplusplus
}
#endif

#endif /* _VOICE_RUNTIME_H */
