/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * voice_model — per-uid voice presence + state for the chat / users
 * list speaker indicators.
 *
 * Three data sources flow in:
 *
 *   1. The VOICE_PARTICIPANTS blob the server ships in 605
 *      ROOM_STATUS broadcasts (and the JOIN reply's initial
 *      payload). rcv.c calls hx_voice_model_ingest_participants
 *      after every such frame to refresh the {in_voice, muted}
 *      bits per uid.
 *
 *   2. The hxvoice-runtime voice-activity evaluator (a GStreamer
 *      `level` RMS detector on each receive bin). Fires a
 *      SignalCallbacks::speaker_changed callback per uid when its
 *      talking state flips; the C handler in voice_panel.c calls
 *      hx_voice_model_set_speaking.
 *
 *   3. Disconnect / VoiceRuntime teardown clears the model.
 *
 * One signal is emitted per uid whenever the COMPUTED indicator
 * (per the policy in voice_model.c::compute_indicator) flips. A
 * change to one of the input flags that doesn't move the
 * indicator (e.g. setting speaking=TRUE on a uid that's already
 * MUTED — speaking is collapsed to IN_VOICE anyway, then MUTED
 * wins over both) does not emit. Consumers (today: users_view,
 * which paints a column with a speaker / mute icon) subscribe
 * and refresh the affected row.
 *
 * Why a model GObject rather than poking users_view directly:
 * the users list lives in chat tabs AND in the standalone users
 * window AND (eventually) in any other surface that wants to
 * show who's on voice. A single source of truth keeps those
 * surfaces in agreement without a per-consumer diff path.
 *
 * Lifetime: one HxVoiceModel per session. Created in
 * `fe_init()` at startup, lives for the whole application
 * lifetime, cleared (per-uid state reset, signals emitted) on
 * disconnect via `hx_voice_model_clear`. The object itself is
 * not unref'd on disconnect — `users_view` cells subscribe to
 * `indicator-changed` once at column build, and re-creating the
 * model across reconnects would require re-attaching every cell.
 *
 * NOT thread-safe — all entry points must run on the GLib main
 * thread (same convention as the rest of the C side).
 */

#ifndef HX_VOICE_MODEL_H
#define HX_VOICE_MODEL_H 1

#include <glib-object.h>
#include <stdint.h>

G_BEGIN_DECLS

#define HX_TYPE_VOICE_MODEL (hx_voice_model_get_type ())
G_DECLARE_FINAL_TYPE (HxVoiceModel, hx_voice_model, HX, VOICE_MODEL, GObject)

/* Computed indicator state for a uid. The four enum variants are
 * the three states the user described plus the "muted" overlay:
 *
 *   NONE       — uid is not in voice chat. No indicator.
 *   IN_VOICE   — uid is in voice chat, not currently speaking,
 *                not muted. Dim speaker glyph.
 *   SPEAKING   — uid is in voice chat AND actively speaking
 *                (the runtime's GStreamer `level` voice-activity
 *                detector saw this peer's RMS clear the speaking
 *                threshold within the last ~200 ms tick).
 *                Highlighted speaker glyph.
 *   MUTED      — uid is in voice chat AND server-flagged as
 *                muted. Microphone-slash glyph. Overrides
 *                IN_VOICE/SPEAKING for the same uid because a
 *                muted participant can't be producing audio
 *                from the listener's perspective even if a
 *                residual RMS reading spuriously trips.
 *
 * Match the enum in client code via `switch` (it's exhaustive).
 *
 * The user list column renders one icon per state; producing a
 * single enum from the (in_voice, speaking, muted) triple here
 * keeps the rendering logic stupid and the policy in one place.
 */
typedef enum {
    HX_VOICE_INDICATOR_NONE = 0,
    HX_VOICE_INDICATOR_IN_VOICE,
    HX_VOICE_INDICATOR_SPEAKING,
    HX_VOICE_INDICATOR_MUTED,
} HxVoiceIndicator;

/* Construct an empty model. The model holds no per-uid state until
 * its ingest entry points are called. */
extern HxVoiceModel *hx_voice_model_new (void);

/* Update presence + mute state from a freshly-arrived
 * VOICE_PARTICIPANTS blob (the 6-byte-per-entry packed binary as
 * defined in hotline_proto::voice). `blob` may be NULL with
 * `len == 0` for an empty room. uids absent from the new blob
 * transition to NONE; uids present transition to IN_VOICE/MUTED
 * depending on bit 0 of their flags field (per fogWraith
 * Capabilities-Voice.md).
 *
 * The model invokes the "indicator-changed" signal once per uid
 * whose indicator state actually changed, in unspecified order.
 *
 * Speaking state for a uid that's still in the room is preserved
 * across this call — the RTP probe is the source of truth for
 * speaking. A uid that leaves the room has its speaking flag
 * cleared atomically with the IN_VOICE → NONE transition.
 */
extern void hx_voice_model_ingest_participants (HxVoiceModel *self,
                                                const uint8_t *blob,
                                                size_t len);

/* Update the speaking flag for `uid`. Used by the
 * SignalCallbacks::speaker_changed bridge in voice_panel.c.
 *
 * If `uid` is not currently in the model (server hasn't named
 * it in any participants blob yet), the call is a no-op: a
 * speaker activity ping for a uid we don't know about doesn't
 * become a visible indicator until the participants blob
 * confirms presence. This avoids a transient flicker when the
 * runtime's pad-added fires a beat before the corresponding
 * 605 reaches us.
 */
extern void hx_voice_model_set_speaking (HxVoiceModel *self, uint16_t uid,
                                         gboolean is_speaking);

/* Record our own uid so the join/leave notification sounds can
 * exclude it. We always know we joined/left ourselves — the toolbar
 * button did it — so playing a sound for our own presence change is
 * just noise. Set once on voice-runtime start (voice_panel.c
 * on_join_toggled). Passing 0 (a uid the server never assigns to a
 * real user) effectively disables the self-exclusion. */
extern void hx_voice_model_set_self_uid (HxVoiceModel *self, uint16_t uid);

/* Clear all per-uid state, transitioning every active uid back to
 * NONE. Used at voice-runtime teardown (session disconnect, Leave
 * sweep) so stale indicators don't survive the reconnect.
 */
extern void hx_voice_model_clear (HxVoiceModel *self);

/* Read the computed indicator state for a uid. Returns
 * HX_VOICE_INDICATOR_NONE for unknown uids. Cheap O(1) hash lookup
 * — the user list cell snapshots this on every paint without
 * worrying about cost. */
extern HxVoiceIndicator hx_voice_model_get_indicator (HxVoiceModel *self,
                                                      uint16_t uid);

/*
 * Signal: "indicator-changed", emitted whenever a uid's computed
 * HxVoiceIndicator state flips. Carries `uid` (u16 as guint) and
 * the new indicator (as guint to keep glib's enum-signal plumbing
 * out of the marshaller). Listeners typically diff against their
 * cached state and refresh only the affected row.
 *
 * Emitted from the main thread by every ingest entry point above.
 */

G_END_DECLS

#endif /* HX_VOICE_MODEL_H */
