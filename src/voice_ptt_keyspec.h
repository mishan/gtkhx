/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * voice_ptt_keyspec — pure helpers for canonicalising and validating
 * push-to-talk keybinds.
 *
 * Split from voice_ptt.c so the validation rules can be unit-tested
 * without dragging GTK / Adwaita / the runtime into the test binary.
 * Pure functions only — no widgets, no GObject, just `guint keyval`
 * (a GDK keysym) and `GdkModifierType` flag bits.
 *
 * The PTT key vocabulary is deliberately restricted so the runtime
 * hook cannot intercept keystrokes meant for chat-input typing.
 * Accepted shapes:
 *
 *   - Function keys F1 through F24 (with or without modifiers).
 *   - Common dedicated keys: Pause/Break, ScrollLock, Insert,
 *     PrintScreen, Menu.
 *   - ANY keyval with at least one of Control / Alt / Super held.
 *     (Shift alone is not enough — Shift+letter is still a
 *     typeable shape on most keyboards.)
 *
 * Rejected without modifiers (would conflict with chat-input
 * typing or with the existing chat-input key handler's bindings):
 *
 *   - Plain letters, digits, punctuation, Space, Tab, Return,
 *     Escape, Backspace, Delete, arrow keys, Home/End/PageUp/PageDn.
 *
 *   These keys ARE accepted when a strong modifier (Ctrl, Alt, or
 *   Super) is held alongside, since e.g. Ctrl+Home is unambiguous
 *   as a binding. Shift alone does not promote — Shift+letter is
 *   still a typing keystroke on every layout, and Shift+Home is
 *   the standard "extend selection" shortcut on text widgets.
 *
 * Always rejected, regardless of modifiers:
 *
 *   - Modifier keys held in isolation (Ctrl_L/R, Alt_L/R,
 *     Shift_L/R, Super_L/R, Hyper_L/R, Meta_L/R, the various ISO
 *     level shifts, CapsLock, NumLock). The keyval that fires when
 *     you press Ctrl by itself is the Ctrl_L keysym; binding PTT
 *     to that would trigger on every modifier press including
 *     ones combined with other shortcuts.
 *
 * The canonical-name format is the same string `gtk_accelerator_parse`
 * accepts:
 *
 *   "F8"
 *   "Pause"
 *   "<Control>F12"
 *   "<Control><Alt>Pause"
 *
 * Modifiers are listed in a stable order (Control, Shift, Alt,
 * Super) so identical binds always serialize to the same string.
 */

#ifndef HX_VOICE_PTT_KEYSPEC_H
#define HX_VOICE_PTT_KEYSPEC_H 1

#include <glib.h>
#include <gdk/gdk.h>

G_BEGIN_DECLS

/*
 * Returns TRUE iff (keyval, state) is an acceptable PTT bind per
 * the vocabulary above.
 *
 * The function only inspects the PTT-relevant modifier bits
 * (Control / Alt / Super / Shift); CapsLock / NumLock /
 * ScrollLock / Mode_switch and any other extra bits in `state`
 * are ignored. Callers don't need to mask before the call —
 * passing the raw `GdkModifierType` from a key event is fine.
 */
extern gboolean hx_voice_ptt_keyspec_allowed (guint keyval,
                                              GdkModifierType state);

/*
 * Build the canonical string for (keyval, state). Returns a newly-
 * allocated g_strdup'd buffer the caller frees. Returns NULL if
 * the spec is invalid per hx_voice_ptt_keyspec_allowed.
 *
 * Round-trip property: for any (keyval, state) where this returns
 * non-NULL, `gtk_accelerator_parse` on the returned string yields
 * the original (keyval, state) pair (with state masked to the
 * modifiers we serialise).
 */
extern char *hx_voice_ptt_keyspec_canonicalize (guint keyval,
                                                GdkModifierType state);

/*
 * Parse the canonical string back into (keyval, state). Returns
 * TRUE on success. The out parameters are only written on success.
 *
 * NULL or empty string returns FALSE (the "no key captured yet"
 * sentinel); the caller decides whether that's an error or a
 * legitimate "PTT enabled but key unset" state.
 */
extern gboolean hx_voice_ptt_keyspec_parse (const char *spec,
                                            guint *out_keyval,
                                            GdkModifierType *out_state);

/*
 * Modifier subset we care about for PTT binds. CapsLock / NumLock /
 * ScrollLock / Mode_switch are deliberately excluded — those toggle
 * state rather than being held, and the canonical string would
 * change under the user's feet.
 */
#define HX_VOICE_PTT_MODIFIER_MASK \
    (GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)

G_END_DECLS

#endif /* HX_VOICE_PTT_KEYSPEC_H */
