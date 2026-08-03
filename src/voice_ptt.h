/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * voice_ptt — window-scoped push-to-talk hook.
 *
 * Reads `gtkhx_prefs.voice_ptt_enabled` and `gtkhx_prefs.voice_ptt_key`
 * on every key event; no setter API (the Settings rows write through the
 * by-name setters and the mirror is refreshed from there, so the hook just
 * samples). The hook is dormant when:
 *
 *   - PTT is disabled in prefs, OR
 *   - PTT is enabled but no key is captured (empty CFG_VOICE_PTT_KEY), OR
 *   - The current session is not joined to a voice room (voice_runtime
 *     is NULL or no active cid).
 *
 * Behaviour when active:
 *
 *   - key-pressed matching the configured PTT key → VOICE_MUTE(0).
 *     Auto-repeat presses (held key) are suppressed so we only fire
 *     one VOICE_MUTE on the press edge. The handler returns TRUE to
 *     consume the press (prevents the PTT key from leaking into
 *     chat input or other widgets).
 *   - key-released matching the latched press → VOICE_MUTE(1).
 *     The release signal is void in GTK4, so the handler doesn't
 *     suppress the event — but since key-released is a delivery-
 *     order tail end and the matching press was already consumed
 *     above, no downstream widget would have a stale press state
 *     to act on anyway.
 *
 * Modifier handling: we match on (keyval, ptt_modifier_mask & state).
 * Extra modifier bits we don't care about (lock states etc.) are
 * masked before the comparison. Press edges with EXTRA non-PTT
 * modifiers — e.g. user assigned `<Control>F12` and holds
 * `<Control><Shift>F12` — are treated as NOT matching, to keep the
 * binding precise.
 */

#ifndef HX_VOICE_PTT_H
#define HX_VOICE_PTT_H 1

#include <glib.h>
#include <gtk/gtk.h>

#include "session.h"

G_BEGIN_DECLS

/*
 * Attach the PTT key controller to `window`. Idempotent — a second call is a
 * no-op. The controller's lifetime is the window's; no cleanup API needed.
 *
 * Takes no session. There is one microphone, so the key acts on whichever
 * connection currently holds voice — which the arbiter answers
 * (voice_arbiter.rs). It used to stash the session it was attached with, which
 * was the same thing while there was one connection and silently did nothing
 * once voice moved to another.
 */
extern void hx_voice_ptt_attach (GtkWidget *window);

G_END_DECLS

#endif /* HX_VOICE_PTT_H */
