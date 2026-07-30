/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/unit/test_voice_ptt_keyspec.c — pin the PTT key-spec
 * vocabulary so a future tweak (e.g. allowing Tab+Shift after all)
 * has to update the test and announce itself.
 *
 * Coverage:
 *
 *   accepted_dedicated     — F1..F24, Pause, ScrollLock, Insert,
 *                            Print, Menu accepted with no
 *                            modifiers.
 *   accepted_modified      — Ctrl+letter, Alt+digit, Super+anything
 *                            accepted via strong-modifier rule.
 *   rejected_typing        — plain letters, digits, Space, Tab,
 *                            Return, Escape rejected without
 *                            modifiers (would conflict with chat
 *                            input typing).
 *   rejected_shift_only    — Shift alone doesn't promote a typing
 *                            key into a valid bind (Shift+a is
 *                            still a typed letter).
 *   rejected_modifier_alone — Ctrl / Alt / Shift / Super pressed by
 *                            themselves never make a valid bind.
 *   canonicalize_round_trip — every accepted spec round-trips
 *                            through canonicalize → parse →
 *                            (same keyval, same modifier mask).
 *   parse_empty            — NULL / "" parse as FALSE (the
 *                            "no key captured yet" sentinel).
 *   parse_rejects_vocabulary_violations
 *                          — hand-edited specs like "a" or
 *                            "<Shift>1" or "Control_L" must NOT
 *                            round-trip back into a usable bind.
 *                            Out-params are untouched on failure.
 *   canonicalize_modifier_order — Control + Shift + Alt produces a
 *                            stable, sorted modifier prefix.
 */

#include "config.h"

#include <glib.h>
#include <gtk/gtk.h>

#include "voice_ptt_keyspec.h"

/* ------------------------------------------------------------------ */

static void
test_accepted_dedicated (void)
{
    /* No modifiers needed for the dedicated keys. */
    g_assert_true (hx_voice_ptt_keyspec_allowed (GDK_KEY_Pause, 0));
    g_assert_true (hx_voice_ptt_keyspec_allowed (GDK_KEY_F1, 0));
    g_assert_true (hx_voice_ptt_keyspec_allowed (GDK_KEY_F8, 0));
    g_assert_true (hx_voice_ptt_keyspec_allowed (GDK_KEY_F12, 0));
    g_assert_true (hx_voice_ptt_keyspec_allowed (GDK_KEY_F24, 0));
    g_assert_true (hx_voice_ptt_keyspec_allowed (GDK_KEY_Scroll_Lock, 0));
    g_assert_true (hx_voice_ptt_keyspec_allowed (GDK_KEY_Insert, 0));
    g_assert_true (hx_voice_ptt_keyspec_allowed (GDK_KEY_Print, 0));
    g_assert_true (hx_voice_ptt_keyspec_allowed (GDK_KEY_Menu, 0));
}

static void
test_accepted_modified (void)
{
    /* Strong modifier (Control, Alt, Super) makes any keyval a
     * valid bind. Letters, digits, Space all become non-typing
     * combinations. */
    g_assert_true (hx_voice_ptt_keyspec_allowed (GDK_KEY_a, GDK_CONTROL_MASK));
    g_assert_true (hx_voice_ptt_keyspec_allowed (GDK_KEY_1, GDK_ALT_MASK));
    g_assert_true (
        hx_voice_ptt_keyspec_allowed (GDK_KEY_space, GDK_SUPER_MASK));
    /* Dedicated key + strong modifier is also fine. */
    g_assert_true (hx_voice_ptt_keyspec_allowed (
        GDK_KEY_F12, GDK_CONTROL_MASK | GDK_ALT_MASK));
}

static void
test_rejected_typing (void)
{
    /* Plain typing keys without any modifier — would conflict with
     * chat-input typing. */
    g_assert_false (hx_voice_ptt_keyspec_allowed (GDK_KEY_a, 0));
    g_assert_false (hx_voice_ptt_keyspec_allowed (GDK_KEY_z, 0));
    g_assert_false (hx_voice_ptt_keyspec_allowed (GDK_KEY_0, 0));
    g_assert_false (hx_voice_ptt_keyspec_allowed (GDK_KEY_9, 0));
    g_assert_false (hx_voice_ptt_keyspec_allowed (GDK_KEY_space, 0));
    g_assert_false (hx_voice_ptt_keyspec_allowed (GDK_KEY_Tab, 0));
    g_assert_false (hx_voice_ptt_keyspec_allowed (GDK_KEY_Return, 0));
    g_assert_false (hx_voice_ptt_keyspec_allowed (GDK_KEY_Escape, 0));
    g_assert_false (hx_voice_ptt_keyspec_allowed (GDK_KEY_BackSpace, 0));
    g_assert_false (hx_voice_ptt_keyspec_allowed (GDK_KEY_period, 0));
    g_assert_false (hx_voice_ptt_keyspec_allowed (GDK_KEY_slash, 0));
}

static void
test_rejected_shift_only (void)
{
    /* Shift alone doesn't make a typing key into a valid PTT bind.
     * Shift+a is just an uppercase letter on every keyboard layout
     * — still a typing keystroke. */
    g_assert_false (hx_voice_ptt_keyspec_allowed (GDK_KEY_a, GDK_SHIFT_MASK));
    g_assert_false (hx_voice_ptt_keyspec_allowed (GDK_KEY_1, GDK_SHIFT_MASK));
    g_assert_false (
        hx_voice_ptt_keyspec_allowed (GDK_KEY_space, GDK_SHIFT_MASK));
    /* …but Shift on a dedicated key is fine (the dedicated key
     * is already non-typing). */
    g_assert_true (hx_voice_ptt_keyspec_allowed (GDK_KEY_F8, GDK_SHIFT_MASK));
}

static void
test_rejected_modifier_alone (void)
{
    /* The modifier keysyms (the keysym that fires when Ctrl is
     * pressed on its own) are never a valid bind, with or without
     * other modifiers held. */
    g_assert_false (hx_voice_ptt_keyspec_allowed (GDK_KEY_Control_L, 0));
    g_assert_false (hx_voice_ptt_keyspec_allowed (GDK_KEY_Control_R, 0));
    g_assert_false (hx_voice_ptt_keyspec_allowed (GDK_KEY_Alt_L, 0));
    g_assert_false (hx_voice_ptt_keyspec_allowed (GDK_KEY_Shift_L, 0));
    g_assert_false (hx_voice_ptt_keyspec_allowed (GDK_KEY_Super_L, 0));
    /* Even with modifier state set — pressing Ctrl while Alt is
     * already held still produces the Ctrl_L keysym, still not a
     * bind. */
    g_assert_false (
        hx_voice_ptt_keyspec_allowed (GDK_KEY_Control_L, GDK_ALT_MASK));
}

/* ------------------------------------------------------------------ */

static void
round_trip (guint keyval, GdkModifierType state)
{
    char *spec = hx_voice_ptt_keyspec_canonicalize (keyval, state);
    g_assert_nonnull (spec);

    guint out_keyval = 0;
    GdkModifierType out_state = 0;
    g_assert_true (hx_voice_ptt_keyspec_parse (spec, &out_keyval, &out_state));
    g_assert_cmpuint (out_keyval, ==, keyval);
    /* parse masks to the PTT modifier subset — comparing the input
     * needs the same mask applied. */
    g_assert_cmphex (out_state, ==, state & HX_VOICE_PTT_MODIFIER_MASK);
    g_free (spec);
}

static void
test_canonicalize_round_trip (void)
{
    round_trip (GDK_KEY_Pause, 0);
    round_trip (GDK_KEY_F8, 0);
    round_trip (GDK_KEY_F12, GDK_CONTROL_MASK);
    round_trip (GDK_KEY_F12, GDK_CONTROL_MASK | GDK_ALT_MASK);
    round_trip (GDK_KEY_a, GDK_CONTROL_MASK);
    round_trip (GDK_KEY_space, GDK_SUPER_MASK);
    round_trip (GDK_KEY_Insert, GDK_SHIFT_MASK);
}

static void
test_parse_empty (void)
{
    guint k = 0;
    GdkModifierType s = 0;
    g_assert_false (hx_voice_ptt_keyspec_parse (NULL, &k, &s));
    g_assert_false (hx_voice_ptt_keyspec_parse ("", &k, &s));
    /* And parse against garbage stays FALSE — the canonicaliser
     * never produces non-parseable output, so anything we can't
     * round-trip means a corrupt prefs file. */
    g_assert_false (hx_voice_ptt_keyspec_parse ("not a key", &k, &s));
}

/* Parse must reject specs that gtk_accelerator_parse would happily
 * accept but that violate the PTT vocabulary. This is the contract
 * that protects chat-input typing: a hand-edited prefs file with
 * "a" or "1" or "<Shift>a" must NOT be honoured by the runtime
 * hook — otherwise PTT-pressing the letter "a" would intercept
 * typing on every focused widget in the window. */
static void
test_parse_rejects_vocabulary_violations (void)
{
    guint k = 99;
    GdkModifierType s = (GdkModifierType)99;

    /* Plain typing keys — gtk_accelerator_parse("a") succeeds with
     * (keyval=GDK_KEY_a, state=0). Must be rejected. */
    g_assert_false (hx_voice_ptt_keyspec_parse ("a", &k, &s));
    g_assert_false (hx_voice_ptt_keyspec_parse ("Z", &k, &s));
    g_assert_false (hx_voice_ptt_keyspec_parse ("1", &k, &s));
    g_assert_false (hx_voice_ptt_keyspec_parse ("space", &k, &s));
    g_assert_false (hx_voice_ptt_keyspec_parse ("Tab", &k, &s));
    g_assert_false (hx_voice_ptt_keyspec_parse ("Return", &k, &s));

    /* Shift-only on a typing key — same vocabulary violation,
     * Shift+a is still a typed uppercase A. */
    g_assert_false (hx_voice_ptt_keyspec_parse ("<Shift>a", &k, &s));
    g_assert_false (hx_voice_ptt_keyspec_parse ("<Shift>1", &k, &s));

    /* Modifier-only keysyms must round-trip rejected even though
     * gtk_accelerator_parse may accept the canonical form. */
    g_assert_false (hx_voice_ptt_keyspec_parse ("Control_L", &k, &s));
    g_assert_false (hx_voice_ptt_keyspec_parse ("Alt_R", &k, &s));

    /* Out-parameters must be left untouched on failure (so the
     * caller can rely on "parse FALSE → no key configured"). */
    g_assert_cmpuint (k, ==, 99);
    g_assert_cmphex (s, ==, 99);

    /* Sanity: with a valid PTT spec, the out-params DO get set. */
    g_assert_true (hx_voice_ptt_keyspec_parse ("F8", &k, &s));
    g_assert_cmpuint (k, ==, GDK_KEY_F8);
    g_assert_cmphex (s, ==, 0);

    g_assert_true (hx_voice_ptt_keyspec_parse ("<Control>F12", &k, &s));
    g_assert_cmpuint (k, ==, GDK_KEY_F12);
    g_assert_cmphex (s, ==, GDK_CONTROL_MASK);
}

static void
test_canonicalize_modifier_order (void)
{
    /* Three accepted modifiers in mixed order on input — the
     * canonical output should be deterministic. We don't assert
     * on the exact bytes (GTK owns the format), but the same
     * inputs in different orders must yield byte-identical output
     * so the prefs file doesn't churn between launches. */
    char *a = hx_voice_ptt_keyspec_canonicalize (
        GDK_KEY_F12, GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_ALT_MASK);
    char *b = hx_voice_ptt_keyspec_canonicalize (
        GDK_KEY_F12, GDK_ALT_MASK | GDK_CONTROL_MASK | GDK_SHIFT_MASK);
    g_assert_nonnull (a);
    g_assert_nonnull (b);
    g_assert_cmpstr (a, ==, b);
    g_free (a);
    g_free (b);
}

/* ------------------------------------------------------------------ */
int
main (int argc, char **argv)
{
    /* gtk_accelerator_name / _parse are pure functions that don't
     * actually need a GdkDisplay, but GtkInit makes sure GDK's
     * internal symbol tables are populated. Use init_check so the
     * test passes in headless CI without a display. */
    gtk_init_check ();

    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/voice_ptt_keyspec/accepted_dedicated",
                     test_accepted_dedicated);
    g_test_add_func ("/voice_ptt_keyspec/accepted_modified",
                     test_accepted_modified);
    g_test_add_func ("/voice_ptt_keyspec/rejected_typing",
                     test_rejected_typing);
    g_test_add_func ("/voice_ptt_keyspec/rejected_shift_only",
                     test_rejected_shift_only);
    g_test_add_func ("/voice_ptt_keyspec/rejected_modifier_alone",
                     test_rejected_modifier_alone);
    g_test_add_func ("/voice_ptt_keyspec/canonicalize_round_trip",
                     test_canonicalize_round_trip);
    g_test_add_func ("/voice_ptt_keyspec/parse_empty", test_parse_empty);
    g_test_add_func ("/voice_ptt_keyspec/parse_rejects_vocabulary_violations",
                     test_parse_rejects_vocabulary_violations);
    g_test_add_func ("/voice_ptt_keyspec/canonicalize_modifier_order",
                     test_canonicalize_modifier_order);

    return g_test_run ();
}
