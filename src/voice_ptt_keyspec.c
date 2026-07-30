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
#include <string.h>

#include "voice_ptt_keyspec.h"

/* ------------------------------------------------------------------ */
/* Vocabulary                                                          */
/* ------------------------------------------------------------------ */

/* Keysyms that are dedicated non-typing keys and therefore safe
 * to use as a PTT bind with no modifier. Pause is the canonical
 * Linux-desktop PTT key — most users will end up here. */
static gboolean
is_dedicated_ptt_key (guint keyval)
{
    switch (keyval) {
    case GDK_KEY_F1:
    case GDK_KEY_F2:
    case GDK_KEY_F3:
    case GDK_KEY_F4:
    case GDK_KEY_F5:
    case GDK_KEY_F6:
    case GDK_KEY_F7:
    case GDK_KEY_F8:
    case GDK_KEY_F9:
    case GDK_KEY_F10:
    case GDK_KEY_F11:
    case GDK_KEY_F12:
    case GDK_KEY_F13:
    case GDK_KEY_F14:
    case GDK_KEY_F15:
    case GDK_KEY_F16:
    case GDK_KEY_F17:
    case GDK_KEY_F18:
    case GDK_KEY_F19:
    case GDK_KEY_F20:
    case GDK_KEY_F21:
    case GDK_KEY_F22:
    case GDK_KEY_F23:
    case GDK_KEY_F24:
    case GDK_KEY_Pause:
    case GDK_KEY_Scroll_Lock:
    case GDK_KEY_Insert:
    case GDK_KEY_Print:
    case GDK_KEY_Menu:
        return TRUE;
    }
    return FALSE;
}

/* Modifier keysyms — pressing one of these by itself is never a
 * valid PTT bind. The modifier is communicated via `state`, not via
 * the keyval. */
static gboolean
is_modifier_keyval (guint keyval)
{
    switch (keyval) {
    case GDK_KEY_Shift_L:
    case GDK_KEY_Shift_R:
    case GDK_KEY_Control_L:
    case GDK_KEY_Control_R:
    case GDK_KEY_Alt_L:
    case GDK_KEY_Alt_R:
    case GDK_KEY_Meta_L:
    case GDK_KEY_Meta_R:
    case GDK_KEY_Super_L:
    case GDK_KEY_Super_R:
    case GDK_KEY_Hyper_L:
    case GDK_KEY_Hyper_R:
    case GDK_KEY_Caps_Lock:
    case GDK_KEY_Num_Lock:
    case GDK_KEY_ISO_Level3_Shift:
    case GDK_KEY_ISO_Level5_Shift:
        return TRUE;
    }
    return FALSE;
}

/* Whether `state` carries a non-Shift modifier that's strong enough
 * to make any keyval (including plain letters) a non-typing combo.
 * Shift alone doesn't count — Shift+a is just an uppercase letter,
 * still a typing keystroke. */
static gboolean
has_strong_modifier (GdkModifierType state)
{
    return (state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) != 0;
}

gboolean
hx_voice_ptt_keyspec_allowed (guint keyval, GdkModifierType state)
{
    /* Reject lone modifier keys. */
    if (is_modifier_keyval (keyval)) {
        return FALSE;
    }

    /* Dedicated non-typing keys are always fine, with or without
     * modifiers. */
    if (is_dedicated_ptt_key (keyval)) {
        return TRUE;
    }

    /* Any other keyval needs a strong (non-Shift) modifier to be
     * accepted. This catches Ctrl+letter, Alt+digit, Super+anything
     * — all unambiguously non-typing combinations. */
    if (has_strong_modifier (state)) {
        return TRUE;
    }

    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Canonicalisation                                                    */
/* ------------------------------------------------------------------ */

/* Mask `state` down to the PTT modifier subset. */
static GdkModifierType
ptt_mask (GdkModifierType state)
{
    return state & HX_VOICE_PTT_MODIFIER_MASK;
}

char *
hx_voice_ptt_keyspec_canonicalize (guint keyval, GdkModifierType state)
{
    if (!hx_voice_ptt_keyspec_allowed (keyval, state)) {
        return NULL;
    }
    /* gtk_accelerator_name returns the canonical "<Modifier...>Key"
     * form using a fixed modifier order (Control, Shift, Alt, Super)
     * — same form gtk_accelerator_parse round-trips. */
    return gtk_accelerator_name (keyval, ptt_mask (state));
}

gboolean
hx_voice_ptt_keyspec_parse (const char *spec, guint *out_keyval,
                            GdkModifierType *out_state)
{
    guint keyval = 0;
    GdkModifierType state = 0;

    if (!spec || !*spec) {
        return FALSE;
    }
    /* GTK4: gtk_accelerator_parse writes 0 / 0 if the input is
     * unparseable. We treat that as parse failure rather than
     * "accept the zero bind". */
    gtk_accelerator_parse (spec, &keyval, &state);
    if (keyval == 0) {
        return FALSE;
    }
    /* Reject specs whose modifier bits include anything outside
     * the PTT-accepted set. A hand-edited prefs file with e.g.
     * "<Hyper>F12" would otherwise silently get its Hyper bit
     * masked to nothing and parse as plain F12 — widening the
     * binding from the user's intent without telling them.
     * Forcing the parser to reject means corrupt or out-of-
     * vocabulary persisted specs become "PTT not bound" rather
     * than "PTT bound to something different than what was
     * written". */
    if ((state & ~HX_VOICE_PTT_MODIFIER_MASK) != 0) {
        return FALSE;
    }
    /* And verify the (keyval, masked state) pair actually meets
     * the PTT vocabulary contract. Without this check, a
     * manually-edited prefs file could bind PTT to "a" (parses
     * fine — `a` is a valid GDK keysym, no modifiers — but
     * violates the "no plain typing keys" guarantee that keeps
     * PTT from eating chat-input keystrokes). */
    GdkModifierType masked = ptt_mask (state);
    if (!hx_voice_ptt_keyspec_allowed (keyval, masked)) {
        return FALSE;
    }
    if (out_keyval) {
        *out_keyval = keyval;
    }
    if (out_state) {
        *out_state = masked;
    }
    return TRUE;
}
