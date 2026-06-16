/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * voice_model.c — per-uid voice presence + state. See voice_model.h
 * for the contract and rationale.
 *
 * Internally a GHashTable<uid → struct entry> where each entry
 * carries the three input flags (in_voice, muted, speaking) and
 * the last-emitted indicator value. Every ingest path:
 *
 *   1. Computes the new (in_voice, muted, speaking) triple for a
 *      uid.
 *   2. Derives the resulting HxVoiceIndicator via the policy in
 *      compute_indicator().
 *   3. Compares against last_indicator; emits "indicator-changed"
 *      only on diff.
 *
 * Keeping last_indicator on the entry rather than recomputing the
 * previous state matters because the policy can collapse multiple
 * input flags into the same indicator value (e.g. presence and
 * speaking both flip but the uid stays in MUTED state). We want a
 * single signal-emit per real visible change, not a churn-burst.
 */

#include "config.h"

#include <string.h>
#include <glib.h>

#include "hotline_proto.h"
#include "voice_model.h"

/* Whether to emit HX_VOICE_INDICATOR_SPEAKING from compute_indicator
 * based on the runtime's per-pad RTP-activity probe. See the long
 * comment on compute_indicator() for the full background:
 *
 *   - PCMU has no VAD, so RTP packets arrive at 50 pps continuously
 *     while a peer is unmuted regardless of whether they're actually
 *     making sound. Reporting "speaking" off RTP-arrival alone is
 *     misleading.
 *   - The plumbing stays end-to-end so the eventual VAD upgrade (via
 *     GStreamer `level` or RFC 6464 `audio-level`) is a one-line
 *     flip here.
 *
 * This macro is private to voice_model.c — flip to 1 when real
 * volume-graded speaking detection lands on the runtime side, then
 * update the matching expectations in
 * tests/unit/test_voice_model.c::test_speaking_overlay and
 * test_signal_emitted (both already annotate the flip point with
 * comments). The test does NOT define its own copy of this symbol;
 * it asserts on the externally-visible behaviour (which uid's
 * `get_indicator()` returns) that the macro controls. */
#define HX_VOICE_INDICATOR_SHIPS_SPEAKING 0

struct entry {
    gboolean in_voice;
    gboolean muted;
    gboolean speaking;
    HxVoiceIndicator last_indicator;
};

struct _HxVoiceModel {
    GObject parent_instance;
    /* key: GUINT_TO_POINTER (uid as guint16), value: owned `struct
     * entry *` freed via g_free. */
    GHashTable *by_uid;
};

G_DEFINE_FINAL_TYPE (HxVoiceModel, hx_voice_model, G_TYPE_OBJECT)

enum {
    SIGNAL_INDICATOR_CHANGED,
    SIGNAL_LAST
};
static guint signals[SIGNAL_LAST];

/* ------------------------------------------------------------------ */

static HxVoiceIndicator
compute_indicator (const struct entry *e)
{
    if (!e || !e->in_voice) {
        return HX_VOICE_INDICATOR_NONE;
    }
    /* MUTED overrides SPEAKING — a server-flagged muted participant
     * shouldn't render as speaking even if the per-pad probe trips
     * on a half-second of pre-mute residual audio. The spec treats
     * the mute flag as authoritative for "is producing audio", so
     * we mirror that here. */
    if (e->muted) {
        return HX_VOICE_INDICATOR_MUTED;
    }
    /* SPEAKING is deliberately demoted to IN_VOICE in the current
     * shipping configuration. Why: the runtime's per-pad RTP-
     * activity probe (`per_user_rtp_buffers` in hxvoice-runtime)
     * fires for every PCMU buffer that lands on a participant's
     * receive bin, but PCMU + WebRTC + mulawenc has no VAD —
     * 50 packets/sec arrive continuously while a peer is unmuted
     * regardless of whether they're actually making sound. So
     * "speaking" reported by the probe really just means "unmuted
     * and pipeline alive", which is barely more than the mute
     * bit alone tells us.
     *
     * The PLUMBING stays end-to-end (runtime probe → SignalKind::
     * SpeakerChanged → hx_voice_model_set_speaking → entry->speaking)
     * so the eventual VAD upgrade is a one-line revert of this
     * arm. Two paths to real speaker detection are documented in
     * docs/voice-chat-plan.md §12:
     *
     *   - Client-side VAD via the GStreamer `level` element on
     *     each receive bin's decoded PCM tap. ~100-150 LOC,
     *     no server changes, ships actual volume-based detection.
     *
     *   - RFC 6464 audio-level header extension. Best quality but
     *     requires fogWraith Capabilities-Voice.md to ratify the
     *     extension and Janus to advertise it; calendar-coupled
     *     to upstream.
     *
     * Until one of those lands, falling through to IN_VOICE keeps
     * the indicator honest. */
#if HX_VOICE_INDICATOR_SHIPS_SPEAKING
    if (e->speaking) {
        return HX_VOICE_INDICATOR_SPEAKING;
    }
#endif
    return HX_VOICE_INDICATOR_IN_VOICE;
}

/* Look up the entry for `uid`, creating it (with all-FALSE flags +
 * NONE indicator) if it didn't exist. The "indicator-changed"
 * emit caller compares against last_indicator afterwards. */
static struct entry *
get_or_create_entry (HxVoiceModel *self, guint16 uid)
{
    gpointer key = GUINT_TO_POINTER ((guint) uid);
    struct entry *e = g_hash_table_lookup (self->by_uid, key);
    if (!e) {
        e = g_new0 (struct entry, 1);
        e->last_indicator = HX_VOICE_INDICATOR_NONE;
        g_hash_table_insert (self->by_uid, key, e);
    }
    return e;
}

/* Recompute + emit if the indicator changed. Returns TRUE iff a
 * signal was emitted. */
static gboolean
recompute_and_maybe_emit (HxVoiceModel *self, guint16 uid, struct entry *e)
{
    HxVoiceIndicator now = compute_indicator (e);
    if (now == e->last_indicator) {
        return FALSE;
    }
    e->last_indicator = now;
    g_signal_emit (self, signals[SIGNAL_INDICATOR_CHANGED], 0,
                   (guint) uid, (guint) now);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* GObject boilerplate                                                  */
/* ------------------------------------------------------------------ */

static void
hx_voice_model_finalize (GObject *obj)
{
    HxVoiceModel *self = HX_VOICE_MODEL (obj);
    if (self->by_uid) {
        g_hash_table_destroy (self->by_uid);
        self->by_uid = NULL;
    }
    G_OBJECT_CLASS (hx_voice_model_parent_class)->finalize (obj);
}

static void
hx_voice_model_class_init (HxVoiceModelClass *klass)
{
    G_OBJECT_CLASS (klass)->finalize = hx_voice_model_finalize;

    /* indicator-changed (uid, indicator). Both fields are guint at
     * the wire level — u16 for uid, the HxVoiceIndicator enum
     * coerced to guint for the indicator. This avoids the typed-
     * boxed-payload dance for a tiny scalar signal. */
    signals[SIGNAL_INDICATOR_CHANGED] = g_signal_new (
        "indicator-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_UINT, G_TYPE_UINT);
}

static void
hx_voice_model_init (HxVoiceModel *self)
{
    self->by_uid
        = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

HxVoiceModel *
hx_voice_model_new (void)
{
    return g_object_new (HX_TYPE_VOICE_MODEL, NULL);
}

/* Hard cap on participants the C model will track per ingest.
 * `blob` is server-supplied bytes; without a cap, a malicious or
 * buggy server could force this code to allocate gigabytes and run
 * an O(n) sweep on the UI thread. The wire spec doesn't define a
 * maximum (the field is just a packed array of 6-byte records up
 * to a u16 chunk length, so 65535 / 6 ≈ 10922 is the protocol-
 * level ceiling), and Janus's default room cap is 16. 1024 is
 * orders of magnitude above any plausible legitimate room and a
 * couple of orders of magnitude below any DoS-relevant size; rooms
 * over this size truncate with a debug_log note.
 *
 * Truncation here means uids past slot 1024 in the blob get
 * mis-treated as leavers by the sweep below. That's an acceptable
 * degradation under the threat model (we don't crash; the
 * indicator is wrong for the uncapped overflow case), and the
 * threshold is high enough that it never fires legitimately. */
#define HX_VOICE_MODEL_MAX_PARTICIPANTS 1024

void
hx_voice_model_ingest_participants (HxVoiceModel *self, const uint8_t *blob,
                                    size_t len)
{
    g_return_if_fail (HX_IS_VOICE_MODEL (self));

    /* Each participant entry is exactly 6 bytes on the wire (per
     * the gtkhx_proto_voice_participant ABI assert), so len / 6
     * is the upper bound on count. Cap at
     * HX_VOICE_MODEL_MAX_PARTICIPANTS so untrusted blobs can't
     * force unbounded allocation; always allocate at least 1 so
     * len == 0 ingest (the "empty room" case) still gives the
     * parser a non-NULL output buffer.
     *
     * blob == NULL with len == 0 is the legitimate "empty room"
     * case — the parser returns 0 and the second-pass leaver
     * sweep transitions every previously-known uid to NONE. */
    size_t raw_cap = (len / 6) + 1;
    if (raw_cap > HX_VOICE_MODEL_MAX_PARTICIPANTS) {
        g_warning ("hx_voice_model_ingest_participants: blob carries "
                   "%zu entries; capping at %d. uids past the cap "
                   "will be mis-treated as leavers.",
                   raw_cap - 1, HX_VOICE_MODEL_MAX_PARTICIPANTS);
        raw_cap = HX_VOICE_MODEL_MAX_PARTICIPANTS;
    }
    g_autofree struct gtkhx_proto_voice_participant *ents
        = g_new (struct gtkhx_proto_voice_participant, raw_cap);
    size_t n
        = gtkhx_proto_parse_voice_participants (blob, len, ents, raw_cap);

    /* Snapshot the set of uids we'll keep (everyone the new blob
     * named) so we can diff against the current hashtable and
     * transition the leavers to NONE in a single sweep. */
    g_autoptr (GHashTable) keep
        = g_hash_table_new (g_direct_hash, g_direct_equal);
    for (size_t i = 0; i < n; i++) {
        g_hash_table_insert (
            keep, GUINT_TO_POINTER ((guint) ents[i].user_id), NULL);
    }

    /* First pass: update / insert for every uid the blob named. */
    for (size_t i = 0; i < n; i++) {
        guint16 uid = ents[i].user_id;
        gboolean muted = (ents[i].flags & 0x0001) != 0;
        struct entry *e = get_or_create_entry (self, uid);
        e->in_voice = TRUE;
        e->muted = muted;
        /* speaking flag preserved across this call — the RTP
         * probe owns it. */
        recompute_and_maybe_emit (self, uid, e);
    }

    /* Second pass: transition uids that disappeared from the room.
     * Walk a copy of the keys to avoid mutating the table during
     * iteration. */
    GHashTableIter it;
    gpointer key;
    g_autoptr (GArray) leavers = g_array_new (FALSE, FALSE, sizeof (guint16));
    g_hash_table_iter_init (&it, self->by_uid);
    while (g_hash_table_iter_next (&it, &key, NULL)) {
        guint16 uid = (guint16) GPOINTER_TO_UINT (key);
        if (!g_hash_table_contains (keep, key)) {
            g_array_append_val (leavers, uid);
        }
    }
    for (guint i = 0; i < leavers->len; i++) {
        guint16 uid = g_array_index (leavers, guint16, i);
        gpointer key = GUINT_TO_POINTER ((guint) uid);
        struct entry *e = g_hash_table_lookup (self->by_uid, key);
        if (!e) {
            continue;
        }
        /* Clear everything — the uid is no longer in the room.
         * Speaking goes with presence; a follow-up
         * speaker_changed for a uid that's left the room is
         * already a no-op in set_speaking. */
        e->in_voice = FALSE;
        e->muted = FALSE;
        e->speaking = FALSE;
        recompute_and_maybe_emit (self, uid, e);
        /* Drop the entry from the table. Keeping it around would
         * cost ~30-40 bytes per uid ever seen, which is
         * negligible per uid but unbounded under a malicious
         * server that cycles random uids — over hours of
         * adversarial traffic the table could grow to consume
         * non-trivial memory. Re-allocation on a re-join is
         * cheap (one g_new0 + one hashtable insert). */
        g_hash_table_remove (self->by_uid, key);
    }
}

void
hx_voice_model_set_speaking (HxVoiceModel *self, uint16_t uid,
                             gboolean is_speaking)
{
    g_return_if_fail (HX_IS_VOICE_MODEL (self));

    struct entry *e = g_hash_table_lookup (
        self->by_uid, GUINT_TO_POINTER ((guint) uid));
    /* No-op for uids the server hasn't announced. See header for
     * the "transient flicker" rationale. */
    if (!e || !e->in_voice) {
        return;
    }
    if (e->speaking == (is_speaking != FALSE)) {
        return;
    }
    e->speaking = (is_speaking != FALSE);
    recompute_and_maybe_emit (self, uid, e);
}

void
hx_voice_model_clear (HxVoiceModel *self)
{
    g_return_if_fail (HX_IS_VOICE_MODEL (self));

    /* Walk a snapshot of uids so we can emit per-uid signals
     * before mutating the table. */
    g_autoptr (GArray) uids = g_array_new (FALSE, FALSE, sizeof (guint16));
    GHashTableIter it;
    gpointer key;
    g_hash_table_iter_init (&it, self->by_uid);
    while (g_hash_table_iter_next (&it, &key, NULL)) {
        guint16 uid = (guint16) GPOINTER_TO_UINT (key);
        g_array_append_val (uids, uid);
    }
    for (guint i = 0; i < uids->len; i++) {
        guint16 uid = g_array_index (uids, guint16, i);
        struct entry *e = g_hash_table_lookup (
            self->by_uid, GUINT_TO_POINTER ((guint) uid));
        if (!e) {
            continue;
        }
        e->in_voice = FALSE;
        e->muted = FALSE;
        e->speaking = FALSE;
        recompute_and_maybe_emit (self, uid, e);
    }
    /* Drop every entry after the signal sweep so the model
     * genuinely returns to empty. Done after the iterator walk
     * to avoid mutating the table while the snapshot above is
     * being built; safe to call here because the per-uid signal
     * emission is finished. */
    g_hash_table_remove_all (self->by_uid);
}

HxVoiceIndicator
hx_voice_model_get_indicator (HxVoiceModel *self, uint16_t uid)
{
    g_return_val_if_fail (HX_IS_VOICE_MODEL (self), HX_VOICE_INDICATOR_NONE);
    struct entry *e = g_hash_table_lookup (
        self->by_uid, GUINT_TO_POINTER ((guint) uid));
    if (!e) {
        return HX_VOICE_INDICATOR_NONE;
    }
    return e->last_indicator;
}
