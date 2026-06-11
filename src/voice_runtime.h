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

/* Standard library headers go OUTSIDE extern "C" so C++ consumers
 * pick up the standard-library declarations with their natural
 * linkage. Matches the convention used by the rest of the headers
 * in this repo. */
#include <stdint.h>
#include <stddef.h>

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
 * code path that might construct a GStreamer element.
 */
extern int gtkhx_voice_init (void);

/*
 * Per-session voice runtime.
 *
 * Opaque pointer: the C side holds a `gtkhx_voice_runtime *` and
 * passes it back to the event-injection shims without inspecting
 * the bytes. The Rust side knows it's a `Box<VoiceRuntime>` that
 * was leaked across the FFI boundary at construction time.
 *
 * Lifetime: created lazily on first voice interaction
 * (typically the Join Voice click), freed on session teardown /
 * disconnect. Per-session, not per-room — one runtime drives all
 * the room transitions for a session (the state machine's
 * mid-session-room-switch path handles JoinRequested with a
 * different cid by tearing down the prior room state internally).
 *
 * Thread safety: the runtime is main-thread-only. All event
 * injection shims must be called from the GLib main thread; the
 * state machine + GStreamer dispatch live there.
 *
 * Outgoing wire frames: the runtime currently uses a NoopBackend
 * for `Action::SendWireFrame` because the C side already calls
 * `hx_send_voice_*` directly from the UI click path. A follow-up
 * will swap to a bridge Backend so the state machine becomes the
 * single source of truth for outbound voice opcodes.
 */
typedef struct gtkhx_voice_runtime gtkhx_voice_runtime;

/*
 * Construct a new runtime. Returns NULL on failure (GStreamer not
 * initialised, webrtcbin plugin missing). The caller frees with
 * gtkhx_voice_runtime_free.
 */
extern gtkhx_voice_runtime *gtkhx_voice_runtime_new (void);

/*
 * Free a runtime. Safe to call with NULL. The caller must not use
 * the pointer after this call returns.
 */
extern void gtkhx_voice_runtime_free (gtkhx_voice_runtime *rt);

/* Event-injection shims. Each translates from C-flavoured
 * parameters into the typed hxvoice::Event variant and feeds it
 * through VoiceRuntime::handle_event. All are NULL-safe on the
 * runtime pointer (drop the event silently). */

/* Fire Event::JoinRequested { cid }. */
extern void gtkhx_voice_runtime_join (gtkhx_voice_runtime *rt, uint32_t cid);

/* Fire Event::LeaveRequested { cid }. */
extern void gtkhx_voice_runtime_leave (gtkhx_voice_runtime *rt, uint32_t cid);

/* Fire Event::MuteToggleRequested { muted }. */
extern void gtkhx_voice_runtime_mute (gtkhx_voice_runtime *rt, int muted);

/* Fire Event::SdpOfferReceived { cid, sdp }. sdp is a NUL-terminated
 * C string; NULL is treated as empty (and dropped by the state
 * machine's downstream parser). */
extern void gtkhx_voice_runtime_sdp_offer (gtkhx_voice_runtime *rt,
                                           uint32_t cid,
                                           const char *sdp);

/* Fire Event::IceCandidateReceived { cid, candidate_json }. The
 * empty-string end-of-candidates marker is handled inside the
 * state machine. */
extern void gtkhx_voice_runtime_ice_candidate (gtkhx_voice_runtime *rt,
                                               uint32_t cid,
                                               const char *candidate_json);

/* Fire Event::ParticipantsUpdated { cid, entries }. blob+len is
 * the 6-byte-per-entry packed binary the server ships in
 * DATA_VOICE_PARTICIPANTS; the Rust side parses it via the
 * hotline_proto::voice::parse_voice_participants iterator. NULL
 * blob with len==0 is an empty list (room is empty). */
extern void gtkhx_voice_runtime_room_status (gtkhx_voice_runtime *rt,
                                             uint32_t cid,
                                             const uint8_t *blob,
                                             size_t len);

/* Fire Event::ServerTaskError { origin_opcode, text }. Called from
 * the HTLS_HDR_TASK error dispatch when the originating opcode was
 * one of the voice opcodes that registers a TASK: 600 (JOIN),
 * 601 (LEAVE), 603 (SDP_ANSWER), 606 (MUTE). 604 (ICE) is a
 * bidirectional notification with no task reply, so it never
 * reaches this entry point. text may be NULL (empty message). */
extern void gtkhx_voice_runtime_task_error (gtkhx_voice_runtime *rt,
                                            uint32_t origin_opcode,
                                            const char *text);

#ifdef __cplusplus
}
#endif

#endif /* _VOICE_RUNTIME_H */
