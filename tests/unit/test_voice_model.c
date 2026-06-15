/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/unit/test_voice_model.c — pin the HxVoiceModel per-uid
 * voice indicator model. (Not a state machine — that lives in
 * the Rust hxvoice crate; this is a per-uid `{in_voice, muted,
 * speaking} → indicator` projection with a single signal.)
 *
 * Coverage:
 *
 *   empty                — fresh model: every uid reads NONE.
 *   ingest_basic         — single participant, not muted: IN_VOICE.
 *   ingest_muted         — mute bit set: MUTED.
 *   speaking_overlay     — RTP probe flips IN_VOICE → SPEAKING and
 *                          back; MUTED dominates over SPEAKING.
 *   leavers_cleared      — uid that disappears from a fresh blob
 *                          drops to NONE.
 *   speaking_unknown_uid — set_speaking for a uid not in any
 *                          participants blob is a no-op (per the
 *                          model's pre-presence flicker contract).
 *   clear                — clear() resets every active uid to NONE.
 *   signal_emitted       — indicator-changed fires once per uid
 *                          whose computed indicator flipped, and
 *                          NOT for unchanged flips.
 *
 * The signal-emission assertions use a small recorder closure
 * that accumulates (uid, indicator) pairs into a GArray; this lets
 * the test compare expected vs observed emissions ordering-
 * agnostically (the model's leaver-sweep walks a HashMap so
 * ordering is unspecified).
 */

#include "config.h"
#include <string.h>
#include <glib.h>

#include "voice_model.h"

/* ---- Wire-format helpers --------------------------------------- */

/* Build a packed VOICE_PARTICIPANTS blob from an array of
 * (uid, flags) pairs. Each entry is 6 bytes BE:
 *   u16 user_id, u16 flags, u16 codec_id.
 * codec_id is always 0 for these tests (PCMU is the only codec
 * the spec defines).
 *
 * Returns a heap-allocated GByteArray the caller frees via
 * g_byte_array_unref. */
typedef struct {
    guint16 uid;
    guint16 flags;
} part_entry;

static GByteArray *
make_blob (const part_entry *ents, gsize n)
{
    GByteArray *b = g_byte_array_new ();
    for (gsize i = 0; i < n; i++) {
        guint16 ub = g_htons (ents[i].uid);
        guint16 fb = g_htons (ents[i].flags);
        guint16 cb = 0;
        g_byte_array_append (b, (const guint8 *) &ub, 2);
        g_byte_array_append (b, (const guint8 *) &fb, 2);
        g_byte_array_append (b, (const guint8 *) &cb, 2);
    }
    return b;
}

/* ---- Signal recorder ------------------------------------------- */

typedef struct {
    guint uid;
    guint indicator;
} emit_record;

typedef struct {
    GArray *records; /* of emit_record */
} recorder;

static void
on_indicator_changed (HxVoiceModel *model, guint uid, guint indicator,
                      gpointer user_data)
{
    recorder *r = user_data;
    emit_record rec = { uid, indicator };
    (void) model;
    g_array_append_val (r->records, rec);
}

static recorder *
install_recorder (HxVoiceModel *model)
{
    recorder *r = g_new0 (recorder, 1);
    r->records = g_array_new (FALSE, FALSE, sizeof (emit_record));
    g_signal_connect (model, "indicator-changed",
                      G_CALLBACK (on_indicator_changed), r);
    return r;
}

static void
free_recorder (recorder *r)
{
    g_array_free (r->records, TRUE);
    g_free (r);
}

/* Returns TRUE if a record for (uid, indicator) sits anywhere in
 * the recorder's history. */
static gboolean
recorder_saw (recorder *r, guint uid, HxVoiceIndicator ind)
{
    for (guint i = 0; i < r->records->len; i++) {
        emit_record rec = g_array_index (r->records, emit_record, i);
        if (rec.uid == uid && rec.indicator == (guint) ind) {
            return TRUE;
        }
    }
    return FALSE;
}

/* ---- Tests ----------------------------------------------------- */

static void
test_empty (void)
{
    HxVoiceModel *m = hx_voice_model_new ();
    g_assert_cmpuint (hx_voice_model_get_indicator (m, 0), ==,
                      HX_VOICE_INDICATOR_NONE);
    g_assert_cmpuint (hx_voice_model_get_indicator (m, 12345), ==,
                      HX_VOICE_INDICATOR_NONE);
    g_object_unref (m);
}

static void
test_ingest_basic (void)
{
    HxVoiceModel *m = hx_voice_model_new ();
    part_entry ents[] = { { 13, 0x0000 } };
    GByteArray *b = make_blob (ents, G_N_ELEMENTS (ents));
    hx_voice_model_ingest_participants (m, b->data, b->len);
    g_assert_cmpuint (hx_voice_model_get_indicator (m, 13), ==,
                      HX_VOICE_INDICATOR_IN_VOICE);
    g_byte_array_unref (b);
    g_object_unref (m);
}

static void
test_ingest_muted (void)
{
    HxVoiceModel *m = hx_voice_model_new ();
    part_entry ents[] = { { 13, 0x0001 } };
    GByteArray *b = make_blob (ents, G_N_ELEMENTS (ents));
    hx_voice_model_ingest_participants (m, b->data, b->len);
    g_assert_cmpuint (hx_voice_model_get_indicator (m, 13), ==,
                      HX_VOICE_INDICATOR_MUTED);
    g_byte_array_unref (b);
    g_object_unref (m);
}

static void
test_speaking_overlay (void)
{
    /* Until real VAD ships (see voice_model.c::
     * HX_VOICE_INDICATOR_SHIPS_SPEAKING), set_speaking is honored
     * internally but compute_indicator demotes SPEAKING → IN_VOICE
     * at render time. The MUTED-beats-SPEAKING precedence still
     * holds end-to-end because that path runs before the demotion. */
    HxVoiceModel *m = hx_voice_model_new ();
    part_entry ents[] = { { 13, 0x0000 } };
    GByteArray *b = make_blob (ents, G_N_ELEMENTS (ents));
    hx_voice_model_ingest_participants (m, b->data, b->len);

    /* Speaking flip — model accepts the input, indicator stays
     * IN_VOICE because of the demotion. Flip
     * HX_VOICE_INDICATOR_SHIPS_SPEAKING in voice_model.c when VAD
     * ships and update this assertion to expect SPEAKING. */
    hx_voice_model_set_speaking (m, 13, TRUE);
    g_assert_cmpuint (hx_voice_model_get_indicator (m, 13), ==,
                      HX_VOICE_INDICATOR_IN_VOICE);

    hx_voice_model_set_speaking (m, 13, FALSE);
    g_assert_cmpuint (hx_voice_model_get_indicator (m, 13), ==,
                      HX_VOICE_INDICATOR_IN_VOICE);

    /* MUTED beats the speaking flag regardless of demotion. */
    part_entry mute_ents[] = { { 13, 0x0001 } };
    GByteArray *bm = make_blob (mute_ents, G_N_ELEMENTS (mute_ents));
    hx_voice_model_ingest_participants (m, bm->data, bm->len);
    hx_voice_model_set_speaking (m, 13, TRUE);
    g_assert_cmpuint (hx_voice_model_get_indicator (m, 13), ==,
                      HX_VOICE_INDICATOR_MUTED);

    g_byte_array_unref (b);
    g_byte_array_unref (bm);
    g_object_unref (m);
}

static void
test_leavers_cleared (void)
{
    HxVoiceModel *m = hx_voice_model_new ();
    part_entry first[] = { { 13, 0 }, { 14, 0 } };
    GByteArray *b1 = make_blob (first, G_N_ELEMENTS (first));
    hx_voice_model_ingest_participants (m, b1->data, b1->len);
    g_assert_cmpuint (hx_voice_model_get_indicator (m, 13), ==,
                      HX_VOICE_INDICATOR_IN_VOICE);
    g_assert_cmpuint (hx_voice_model_get_indicator (m, 14), ==,
                      HX_VOICE_INDICATOR_IN_VOICE);

    /* 14 is gone; 13 remains. */
    part_entry second[] = { { 13, 0 } };
    GByteArray *b2 = make_blob (second, G_N_ELEMENTS (second));
    hx_voice_model_ingest_participants (m, b2->data, b2->len);
    g_assert_cmpuint (hx_voice_model_get_indicator (m, 13), ==,
                      HX_VOICE_INDICATOR_IN_VOICE);
    g_assert_cmpuint (hx_voice_model_get_indicator (m, 14), ==,
                      HX_VOICE_INDICATOR_NONE);

    /* Empty blob — everyone's gone. */
    hx_voice_model_ingest_participants (m, NULL, 0);
    g_assert_cmpuint (hx_voice_model_get_indicator (m, 13), ==,
                      HX_VOICE_INDICATOR_NONE);

    g_byte_array_unref (b1);
    g_byte_array_unref (b2);
    g_object_unref (m);
}

static void
test_speaking_unknown_uid (void)
{
    HxVoiceModel *m = hx_voice_model_new ();
    /* No participants blob yet. set_speaking should be a no-op
     * — the indicator stays NONE. */
    hx_voice_model_set_speaking (m, 99, TRUE);
    g_assert_cmpuint (hx_voice_model_get_indicator (m, 99), ==,
                      HX_VOICE_INDICATOR_NONE);
    g_object_unref (m);
}

static void
test_clear (void)
{
    HxVoiceModel *m = hx_voice_model_new ();
    part_entry ents[] = { { 13, 0 }, { 14, 0x0001 } };
    GByteArray *b = make_blob (ents, G_N_ELEMENTS (ents));
    hx_voice_model_ingest_participants (m, b->data, b->len);
    hx_voice_model_set_speaking (m, 13, TRUE);

    hx_voice_model_clear (m);
    g_assert_cmpuint (hx_voice_model_get_indicator (m, 13), ==,
                      HX_VOICE_INDICATOR_NONE);
    g_assert_cmpuint (hx_voice_model_get_indicator (m, 14), ==,
                      HX_VOICE_INDICATOR_NONE);

    g_byte_array_unref (b);
    g_object_unref (m);
}

static void
test_signal_emitted (void)
{
    HxVoiceModel *m = hx_voice_model_new ();
    recorder *r = install_recorder (m);

    /* uid 13 joins: emit (13, IN_VOICE). */
    part_entry e1[] = { { 13, 0 } };
    GByteArray *b1 = make_blob (e1, G_N_ELEMENTS (e1));
    hx_voice_model_ingest_participants (m, b1->data, b1->len);
    g_assert_true (recorder_saw (r, 13, HX_VOICE_INDICATOR_IN_VOICE));

    /* Same blob again: no flip, no emit. */
    guint prev_len = r->records->len;
    hx_voice_model_ingest_participants (m, b1->data, b1->len);
    g_assert_cmpuint (r->records->len, ==, prev_len);

    /* Speaking flip: model's last_indicator stays IN_VOICE because
     * compute_indicator demotes SPEAKING in the current shipping
     * configuration. The signal therefore does NOT fire — no
     * visible state change, no emit. Flip the assertion when VAD
     * ships and the demotion is reverted (see voice_model.c::
     * HX_VOICE_INDICATOR_SHIPS_SPEAKING). */
    guint speak_prev_len = r->records->len;
    hx_voice_model_set_speaking (m, 13, TRUE);
    g_assert_cmpuint (r->records->len, ==, speak_prev_len);
    g_assert_false (recorder_saw (r, 13, HX_VOICE_INDICATOR_SPEAKING));

    /* Leaver sweep: emit (13, NONE) when the next blob omits us. */
    hx_voice_model_ingest_participants (m, NULL, 0);
    g_assert_true (recorder_saw (r, 13, HX_VOICE_INDICATOR_NONE));

    g_byte_array_unref (b1);
    free_recorder (r);
    g_object_unref (m);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/voice_model/empty", test_empty);
    g_test_add_func ("/voice_model/ingest_basic", test_ingest_basic);
    g_test_add_func ("/voice_model/ingest_muted", test_ingest_muted);
    g_test_add_func ("/voice_model/speaking_overlay", test_speaking_overlay);
    g_test_add_func ("/voice_model/leavers_cleared", test_leavers_cleared);
    g_test_add_func ("/voice_model/speaking_unknown_uid",
                     test_speaking_unknown_uid);
    g_test_add_func ("/voice_model/clear", test_clear);
    g_test_add_func ("/voice_model/signal_emitted", test_signal_emitted);

    return g_test_run ();
}
