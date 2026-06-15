/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#include "config.h"

#include <gdk/gdk.h>
#include <gtk/gtk.h>
#include <glib.h>

#include "debug.h"
#include "prefs.h"
#include "session.h"
#include "voice.h"
#include "voice_ptt.h"
#include "voice_ptt_keyspec.h"
#include "voice_runtime.h"

/* Per-controller state, stashed via g_object_set_data_full on the
 * GtkEventControllerKey. Tracks the "last sent" mute state so we
 * can edge-detect against repeated key-pressed signals (GTK fires
 * key-pressed every ~30 ms while a key is held). */
struct ptt_ctrl {
    session *sess; /* borrowed */
    /* Keyval that owns the current press, latched at the moment the
     * unmute send succeeded. Zero when no PTT key is currently
     * held.
     *
     * Why latch instead of re-deriving from prefs at release time:
     * a user can clear or rebind the PTT key WHILE holding the
     * old key. If on_key_released checked the live prefs, the
     * release of the old key wouldn't match the new bind and the
     * re-mute would be skipped — leaving the user unmuted
     * indefinitely. Latching means the release-side keyval check
     * is authoritative regardless of mid-press pref changes. */
    guint pressed_keyval;
};

static void
ptt_ctrl_free (gpointer p)
{
    g_free (p);
}

/* Read the live PTT bind from prefs. Returns FALSE if disabled or
 * unset; on TRUE, populates *out_keyval / *out_state with the
 * masked modifiers. */
static gboolean
ptt_current_bind (guint *out_keyval, GdkModifierType *out_state)
{
    if (!gtkhx_prefs.voice_ptt_enabled) {
        return FALSE;
    }
    return hx_voice_ptt_keyspec_parse (gtkhx_prefs.voice_ptt_key,
                                       out_keyval, out_state);
}

/* Compare an inbound (keyval, state) against the configured bind.
 * The inbound state is masked to the PTT modifier subset before
 * the comparison; lock-state bits don't matter. Exact-match on
 * the modifier set — Ctrl+F12 is bound, Ctrl+Shift+F12 is NOT a
 * match. */
static gboolean
matches_bind (guint inbound_keyval, GdkModifierType inbound_state)
{
    guint bound_keyval = 0;
    GdkModifierType bound_state = 0;
    if (!ptt_current_bind (&bound_keyval, &bound_state)) {
        return FALSE;
    }
    if (inbound_keyval != bound_keyval) {
        return FALSE;
    }
    return (inbound_state & HX_VOICE_PTT_MODIFIER_MASK) == bound_state;
}

/* Returns TRUE iff the session is currently in a voice room (has
 * a runtime AND the runtime reports an active cid). The PTT key
 * controller stays dormant — i.e. doesn't consume the keystroke —
 * outside this window, so users can still use the bound key as a
 * normal shortcut when they're not in voice. Optionally writes the
 * active cid through `out_cid` for the caller to use. */
static gboolean
session_in_voice (session *sess, uint32_t *out_cid)
{
    if (!sess || !sess->voice_runtime) {
        return FALSE;
    }
    uint32_t cid = 0;
    if (!gtkhx_voice_runtime_active_cid (sess->voice_runtime, &cid)) {
        return FALSE;
    }
    if (out_cid) {
        *out_cid = cid;
    }
    return TRUE;
}

/* Send VOICE_MUTE and fire the matching runtime event. The caller
 * has already confirmed the session is in voice via
 * session_in_voice; this helper just performs the wire-out + state-
 * machine drive. Returns TRUE on a successful wire send, FALSE if
 * the wire helper refused (e.g. CAP_VOICE cleared mid-session). */
static gboolean
ptt_drive_mute (session *sess, uint32_t active_cid, gboolean muted)
{
    gboolean sent = hx_send_voice_mute (&sess->htlc, active_cid, muted);
    if (!sent) {
        return FALSE;
    }
    /* Match the on_mute_toggled pattern: fire the runtime event so
     * the state machine's self.muted stays in sync and the
     * MuteChanged signal updates the panel button label. */
    gtkhx_voice_runtime_mute (sess->voice_runtime, muted ? 1 : 0);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Key event handlers                                                  */
/* ------------------------------------------------------------------ */

static gboolean
on_key_pressed (GtkEventControllerKey *ctrl, guint keyval, guint keycode,
                GdkModifierType state, gpointer user_data)
{
    struct ptt_ctrl *p = user_data;
    (void) ctrl;
    (void) keycode;

    if (!matches_bind (keyval, state)) {
        return FALSE;
    }
    /* Dormancy: the bound key only belongs to PTT while we're
     * actually in a voice room. Outside voice, the user might
     * still want to use F12 (or whatever they picked) as a normal
     * shortcut — consuming it indiscriminately would silently
     * eat their press. Return FALSE so the event bubbles to
     * whatever else might handle it. */
    uint32_t active_cid = 0;
    if (!session_in_voice (p->sess, &active_cid)) {
        return FALSE;
    }
    /* Edge-detect: auto-repeat presses while the key is held fire
     * this signal repeatedly. We only want one VOICE_MUTE(0) per
     * physical press. */
    if (p->pressed_keyval != 0) {
        /* Eat the event so it doesn't bubble to chat input. The
         * binding is "ours" even on the repeat. */
        return TRUE;
    }
    debug_log ("voice", "PTT press → unmute");
    if (ptt_drive_mute (p->sess, active_cid, /*muted=*/FALSE)) {
        /* Latch the specific keyval that owns this press so the
         * release-side comparison stays robust to prefs changing
         * mid-press (user rebinds PTT while holding the old key).
         * The release sends VOICE_MUTE(1) on this exact keyval,
         * not whatever the live prefs say is bound. */
        p->pressed_keyval = keyval;
    } else {
        /* Wire send refused — typically CAP_VOICE cleared mid-
         * session, or the proto builder rejected our parameters.
         * We're still IN voice (session_in_voice returned TRUE
         * above), so the binding is conceptually "ours" — keep
         * consuming so chat input doesn't see the keystroke as a
         * fallback, but don't latch pressed_keyval since the
         * release has nothing to undo. */
        debug_log ("voice",
                   "PTT press in voice but wire send refused — "
                   "consuming the key without latching");
    }
    /* In voice: consume regardless of send success so the key
     * doesn't double-act as both PTT and a chat-input character. */
    return TRUE;
}

static void
on_key_released (GtkEventControllerKey *ctrl, guint keyval, guint keycode,
                 GdkModifierType state, gpointer user_data)
{
    struct ptt_ctrl *p = user_data;
    (void) ctrl;
    (void) keycode;
    (void) state; /* on release, modifier state may differ from press */

    if (p->pressed_keyval == 0) {
        /* We never latched on the press (different bind, drive
         * failure, or session not joined). Nothing to release. */
        return;
    }
    /* Compare against the LATCHED keyval, not the live prefs. If
     * the user rebinds or unbinds PTT mid-press, the release of
     * the originally-pressed key must still re-mute — otherwise
     * the user would be left unmuted forever. The modifier state
     * on release may differ from the press (user lifted Ctrl
     * before F12); matching by keyval alone is enough because
     * the press latched the specific keysym we're tracking. */
    if (keyval != p->pressed_keyval) {
        return;
    }
    debug_log ("voice", "PTT release → mute");
    /* Re-derive the active cid here rather than caching the press-
     * side value. Between press and release, the user could have
     * Leave-and-Joined a different room (the state machine handles
     * mid-press room switches as an implicit leave + join). The
     * release should re-mute the room they're CURRENTLY in. If
     * they've left voice entirely between press and release, the
     * cid lookup fails and we skip the wire send — the runtime
     * teardown already left them muted server-side. */
    uint32_t active_cid = 0;
    if (session_in_voice (p->sess, &active_cid)) {
        ptt_drive_mute (p->sess, active_cid, /*muted=*/TRUE);
    }
    p->pressed_keyval = 0;
}

/* ------------------------------------------------------------------ */
/* Public                                                              */
/* ------------------------------------------------------------------ */

void
hx_voice_ptt_attach (GtkWidget *window, session *sess)
{
    g_return_if_fail (GTK_IS_WIDGET (window));
    g_return_if_fail (sess != NULL);

    /* Idempotence: don't attach twice. Stash a sentinel on the
     * window so a second call (e.g. reconnect path) is a no-op. */
    if (g_object_get_data (G_OBJECT (window), "voice-ptt-attached")) {
        return;
    }
    g_object_set_data (G_OBJECT (window), "voice-ptt-attached",
                       GINT_TO_POINTER (1));

    GtkEventController *kctrl = gtk_event_controller_key_new ();
    /* CAPTURE phase: PTT keys must be consumed BEFORE the chat
     * input's own key controller sees them. The keyspec
     * vocabulary already rules out plain typing keys, so capturing
     * non-PTT keys would be a bug — the handler returns FALSE
     * (don't consume) when the bind doesn't match, so other
     * widgets' default handling proceeds normally. */
    gtk_event_controller_set_propagation_phase (kctrl,
                                                GTK_PHASE_CAPTURE);

    struct ptt_ctrl *p = g_new0 (struct ptt_ctrl, 1);
    p->sess = sess;
    /* pressed_keyval starts 0 from g_new0 — no PTT key held yet. */
    /* Stash on the controller so destroy releases it. */
    g_object_set_data_full (G_OBJECT (kctrl), "voice-ptt-state",
                            p, ptt_ctrl_free);

    g_signal_connect (kctrl, "key-pressed",
                      G_CALLBACK (on_key_pressed), p);
    g_signal_connect (kctrl, "key-released",
                      G_CALLBACK (on_key_released), p);

    gtk_widget_add_controller (window, kctrl);
}
