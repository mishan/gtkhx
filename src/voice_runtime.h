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

#ifndef GTKHX_VOICE_RUNTIME_H
#define GTKHX_VOICE_RUNTIME_H

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
 * Audio-device enumeration + preference setters (Phase 8.E).
 *
 * The Settings → Voice page populates input and output combo boxes
 * from gtkhx_voice_list_input_devices / _list_output_devices, stores
 * the user's pick as gst::Device::name() in gtkhxrc, and pushes the
 * choice back at runtime construction time via
 * gtkhx_voice_set_input_device / _set_output_device. NULL or empty
 * string clears the preference and the next VoiceRuntime build falls
 * back to autoaudiosrc / autoaudiosink (system default).
 *
 * The list handle is opaque — caller iterates via _len + _name +
 * _display_name and frees with _free. Returned strings live as long
 * as the list does (no per-string free), matching the GListModel
 * idiom the Settings UI already uses.
 */
typedef struct gtkhx_voice_device_list gtkhx_voice_device_list;

extern gtkhx_voice_device_list *gtkhx_voice_list_input_devices (void);
extern gtkhx_voice_device_list *gtkhx_voice_list_output_devices (void);
extern size_t gtkhx_voice_device_list_len (const gtkhx_voice_device_list *list);
extern const char *gtkhx_voice_device_list_name (
    gtkhx_voice_device_list *list, size_t idx);
extern const char *gtkhx_voice_device_list_display_name (
    gtkhx_voice_device_list *list, size_t idx);
extern void gtkhx_voice_device_list_free (gtkhx_voice_device_list *list);

extern void gtkhx_voice_set_input_device (const char *name);
extern void gtkhx_voice_set_output_device (const char *name);

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
 * Outgoing wire frames: production uses
 * gtkhx_voice_runtime_new_with_callbacks to register a bridge
 * callback that the state machine drives for opcodes 603
 * (SDP_ANSWER) and 604 (ICE), which originate from webrtcbin
 * events and have no other path to the wire. Opcodes 600 (JOIN),
 * 601 (LEAVE), and 606 (MUTE) come from UI click handlers in
 * voice_panel.c which call `hx_send_voice_*` directly with proper
 * return-value handling, so the dispatcher skips those to avoid
 * double-send. The legacy NoopBackend constructor still exists for
 * test paths that don't care about wire output.
 */
typedef struct gtkhx_voice_runtime gtkhx_voice_runtime;

/*
 * Outgoing-wire-frame callback. The runtime invokes this for every
 * `Action::SendWireFrame` action, passing the opaque `user_data`
 * that was registered alongside the callback (production: the
 * `htlc_conn *` for the session), the opcode (one of
 * HTLC_HDR_VOICE_*), and a pointer + length describing the action
 * body.
 *
 * Body layout matches the encoders in hxvoice::state::encode_*:
 *
 *   - 600 (JOIN), 601 (LEAVE): 4 bytes BE cid
 *   - 603 (SDP_ANSWER): 4 bytes BE cid + SDP bytes (not NUL-terminated)
 *   - 604 (ICE):        4 bytes BE cid + JSON bytes (not NUL-terminated)
 *   - 606 (MUTE):       4 bytes BE cid + 2 bytes BE muted-flag
 *
 * The `body` slice is valid for the duration of the call only; the
 * callback must copy out anything it wants to retain. Implementations
 * must NOT call back into the runtime synchronously (re-entrancy
 * is unsupported on this path).
 */
typedef void (*gtkhx_voice_runtime_send_wire_frame_cb) (void *user_data,
                                                        uint32_t opcode,
                                                        const uint8_t *body,
                                                        size_t body_len);

/*
 * Construct a new runtime with no outbound-wire bridge (NoopBackend).
 * Returns NULL on failure (GStreamer not initialised, webrtcbin
 * plugin missing). The caller frees with gtkhx_voice_runtime_free.
 * Useful for test paths that don't care about outbound wire frames.
 */
extern gtkhx_voice_runtime *gtkhx_voice_runtime_new (void);

/*
 * Construct a new runtime that bridges `Action::SendWireFrame` to
 * a C callback. `user_data` and `send_wire_frame_cb` must remain
 * valid for the lifetime of the returned runtime — production
 * pairs the runtime with its `htlc_conn` and frees both on the
 * same disconnect path. `send_wire_frame_cb` may be NULL (in which
 * case the bridge behaves like NoopBackend for that surface, and
 * this constructor is equivalent to gtkhx_voice_runtime_new).
 */
extern gtkhx_voice_runtime *gtkhx_voice_runtime_new_with_callbacks (
    void *user_data,
    gtkhx_voice_runtime_send_wire_frame_cb send_wire_frame_cb);

/*
 * High-level voice-session state mirror of hxvoice::SessionState.
 * The state-changed signal callback receives one of these values.
 * The numeric assignments match the Rust state.rs enum
 * discriminants so a future cbindgen pass would line up; the
 * runtime maps via an exhaustive match at the FFI boundary.
 */
typedef enum {
    GTKHX_VOICE_STATE_IDLE          = 0,
    GTKHX_VOICE_STATE_JOIN_SENT     = 1,
    GTKHX_VOICE_STATE_OFFER_PENDING = 2,
    GTKHX_VOICE_STATE_CONNECTING    = 3,
    GTKHX_VOICE_STATE_CONNECTED     = 4,
    GTKHX_VOICE_STATE_LEAVING       = 5,
} gtkhx_voice_state;

/*
 * `voice-state-changed` signal callback. Fires on every transition
 * the state machine performs.
 *
 * Recommended mapping to a "joined" UI flag (this is what
 * voice_panel.c does — see state_is_joined): treat
 * GTKHX_VOICE_STATE_JOIN_SENT, GTKHX_VOICE_STATE_OFFER_PENDING,
 * GTKHX_VOICE_STATE_CONNECTING, and GTKHX_VOICE_STATE_CONNECTED
 * all as joined so the toolbar flips to "Leave Voice" the moment
 * the JOIN ships and the user can cancel a stuck handshake.
 * GTKHX_VOICE_STATE_IDLE (back-to-start) and
 * GTKHX_VOICE_STATE_LEAVING (post-tear-down terminal state)
 * read as not-joined so the toolbar offers a fresh Join Voice.
 */
typedef void (*gtkhx_voice_runtime_state_changed_cb) (void *user_data,
                                                      gtkhx_voice_state state);

/*
 * `voice-mute-changed` signal callback. Fires when the state
 * machine's local mute flag changes via MuteToggleRequested. The
 * value is the new mute state (1 = muted, 0 = unmuted).
 *
 * The state machine does NOT currently reflect server-reported mute
 * flips from 605 ROOM_STATUS — only local toggle changes drive this
 * signal. A future revision may extend the surface.
 */
typedef void (*gtkhx_voice_runtime_mute_changed_cb) (void *user_data,
                                                     int muted);

/*
 * `voice-speaker-changed` signal callback. Per-participant talking
 * indicator. Fires whenever the runtime's per-pad RTP-activity
 * evaluator observes a uid's speaking state flip (default cadence
 * 200 ms — see `SPEAKER_EVAL_INTERVAL_MS` in
 * hxvoice-runtime/src/runtime.rs). `is_speaking` is 0 or 1.
 *
 * The producer is NOT the state machine — it's the runtime's
 * receive-bin probe + glib timeout pair. uid maps to Hotline user
 * IDs from the SDP `a=mid:user-{uid}` labels resolved at
 * pad-added; the C side cross-references it with the chat user
 * list to repaint the speaker indicator column.
 *
 * The signal is sticky between flips: once a uid is reported
 * speaking, the runtime won't re-emit `speaker_changed(uid, 1)`
 * for that uid until it transitions to silent and back to
 * speaking. Repaint logic on the C side can therefore conflate
 * "received signal" with "state actually changed."
 */
typedef void (*gtkhx_voice_runtime_speaker_changed_cb) (void *user_data,
                                                        uint16_t uid,
                                                        int is_speaking);

/*
 * `voice-error` signal callback. Carries a user-facing message
 * (NUL-terminated UTF-8) intended for display via AdwToast. The
 * `text` pointer is valid for the duration of the call only; the
 * runtime drops the underlying CString after the callback returns.
 * Implementations should copy out anything they want to retain.
 *
 * Producers: spec-defined session timeouts softened to non-fatal
 * in the state machine (DTLS, IceConnectivity, Media), plus the
 * ServerTaskError arm covering 600 / 601 / 603 / 606 task replies.
 * 604 (ICE notification) has no task reply and never reaches this.
 */
typedef void (*gtkhx_voice_runtime_error_cb) (void *user_data,
                                              const char *text);

/*
 * Bundle of per-SignalKind callbacks. Pass a pointer to one of
 * these to gtkhx_voice_runtime_new_v2 to subscribe to the runtime's
 * state-machine signals. The struct is read once at construction;
 * the runtime captures the function pointers + user_data and the
 * caller may free this struct as soon as the constructor returns.
 *
 * Any field may be NULL — the runtime treats NULL as "no
 * subscriber for this signal".
 *
 * ABI note: this is a plain C struct without a size / version
 * field. Adding new fields here is NOT ABI-compatible — older
 * callers built against a smaller struct definition would have
 * the callee read past the end of their allocation. Whenever a
 * new SignalKind callback slot is added below, every consumer of
 * this header must be rebuilt against the new definition. Since
 * the C side and the Rust runtime live in the same workspace and
 * are built in lockstep by the same Meson invocation, "the
 * runtime build" is the only consumer in practice.
 *
 * Current signal slots:
 *
 *   state_changed   — voice session state machine transitions
 *                     (Idle → JoinSent → … → Connected → Leaving).
 *   mute_changed    — local mute toggle reflection.
 *   speaker_changed — per-uid speaker indicator activity from the
 *                     runtime's per-pad RTP probe.
 *   error           — user-facing AdwToast text from softened
 *                     spec timeouts (DTLS / IceConnectivity / Media)
 *                     and ServerTaskError replies.
 *
 * RoomStatus has no C subscriber slot today: the participants blob
 * the C side cares about flows into hx_voice_model_ingest_participants
 * directly from rcv.c, with no round-trip through the runtime signal.
 */
typedef struct {
    gtkhx_voice_runtime_state_changed_cb    state_changed;
    gtkhx_voice_runtime_mute_changed_cb     mute_changed;
    gtkhx_voice_runtime_speaker_changed_cb  speaker_changed;
    gtkhx_voice_runtime_error_cb            error;
} gtkhx_voice_runtime_signal_callbacks;

/*
 * Construct a runtime that bridges both `Action::SendWireFrame` and
 * `Action::EmitSignal` back to the C side. The signal-callbacks
 * struct pointer may be NULL (no signal subscription); each
 * function-pointer field inside the struct may also be NULL (skip
 * just that signal). The send_wire_frame_cb has the same NULL
 * semantics as gtkhx_voice_runtime_new_with_callbacks.
 *
 * Same lifetime contract as the older constructor: user_data and
 * every non-NULL callback must remain valid for the lifetime of
 * the returned runtime.
 */
extern gtkhx_voice_runtime *gtkhx_voice_runtime_new_v2 (
    void *user_data,
    gtkhx_voice_runtime_send_wire_frame_cb send_wire_frame_cb,
    const gtkhx_voice_runtime_signal_callbacks *signals);

/*
 * Read the runtime's currently-active cid. Returns 1 and writes
 * the cid through `out_cid` when the state machine has an active
 * room (any state except Idle / Leaving); returns 0 and leaves
 * `out_cid` untouched otherwise. NULL-safe on both `rt` and
 * `out_cid` (returns 0).
 *
 * Production uses this from the signal callbacks to figure out
 * which voice panel to update — the StateChanged / MuteChanged
 * payloads don't carry cid, but the C side may have multiple
 * chat panels each tracking a different cid.
 */
extern int gtkhx_voice_runtime_active_cid (gtkhx_voice_runtime *rt,
                                           uint32_t *out_cid);

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

#endif /* GTKHX_VOICE_RUNTIME_H */
