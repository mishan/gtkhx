/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * src/bookmark_rc4_dialog.c — RC4 migration prompt implementation.
 *
 * See bookmark_rc4_dialog.h for the API contract.
 *
 * Same synchronous-from-async pattern as tls_trust_dialog.c: the
 * AdwAlertDialog response API is async (signal-driven), but the
 * caller (connect_open_bookmark_by_name and its peers) needs a
 * synchronous answer to decide whether to proceed with the
 * connection or abandon. Spin a nested GMainLoop until the response
 * handler fires.
 */

#include "config.h"

#include <glib.h>
#include <gtk/gtk.h>
#include <adwaita.h>

#include "compat.h"
#include "bookmark_cipher.h"
#include "bookmark_rc4_dialog.h"
#include "bookmarks.h"

/* Response IDs used in the AdwAlertDialog. Map 1:1 to the four
 * possible outcomes; the "result" field of dialog_state captures
 * the chosen cipher byte (or -1 for cancel). */
#define RESP_CANCEL   "cancel"
#define RESP_NONE     "none"
#define RESP_BLOWFISH "blowfish"
#define RESP_CHACHA20 "chacha20"

typedef struct {
    GMainLoop *loop;
    int result; /* cipher byte chosen, or -1 for cancel */
} dialog_state;

static void
on_dialog_response (AdwAlertDialog *dialog G_GNUC_UNUSED,
                    const char *response,
                    gpointer user_data)
{
    dialog_state *state = user_data;

    if (g_strcmp0 (response, RESP_NONE) == 0) {
        state->result = BOOKMARK_CIPHER_BYTE_NONE;
    } else if (g_strcmp0 (response, RESP_BLOWFISH) == 0) {
        state->result = BOOKMARK_CIPHER_BYTE_BLOWFISH;
    } else if (g_strcmp0 (response, RESP_CHACHA20) == 0) {
        state->result = BOOKMARK_CIPHER_BYTE_CHACHA20_POLY1305;
    } else {
        /* "cancel", Esc, X-close, OS window-manager close — anything
         * that isn't a positive selection — folds to cancel. */
        state->result = -1;
    }

    if (state->loop) {
        g_main_loop_quit (state->loop);
    }
}

/* Persist the chosen cipher byte back to the bookmark file. Skips
 * silently when bookmark_name is NULL (e.g. last_conn-derived
 * caller with no on-disk file to update) or when load / save fail
 * — the dialog already returned a usable answer to the caller, the
 * persistence step is a best-effort UX nicety so subsequent opens
 * of the same bookmark don't re-prompt. We don't surface persistence
 * errors to the user because there's nothing actionable they can do
 * about them mid-connection-attempt. */
static void
persist_choice (const char *bookmark_name, int chosen_byte)
{
    HxBookmark *bm;
    GError *err = NULL;

    if (!bookmark_name || !*bookmark_name) {
        return;
    }
    if (chosen_byte < 0) {
        return;
    }
    bm = hx_bookmark_load (bookmark_name);
    if (!bm) {
        return;
    }
    bm->cipher = (char) chosen_byte;
    if (!hx_bookmark_save (bm, &err)) {
        g_clear_error (&err);
    }
    hx_bookmark_free (bm);
}

int
hx_bookmark_rc4_dialog_run_sync (GtkWindow *parent, const char *bookmark_name)
{
    AdwAlertDialog *dialog;
    g_autoptr (GString) body = g_string_new (NULL);

    /* Body text adapts to whether we have a bookmark name to name in
     * the prompt. Either way, explain WHY we're asking (RC4 is no
     * longer offered) and that the choice gets saved (so the user
     * understands the bookmark mutation). */
    if (bookmark_name && *bookmark_name) {
        g_string_append_printf (
            body,
            _ ("The bookmark \"%s\" was saved with RC4 as its HOPE "
               "cipher. GtkHx no longer offers RC4 — it's a known-"
               "broken stream cipher and the \"Secure\" label it "
               "used to wear was misleading."),
            bookmark_name);
    } else {
        g_string_append (
            body,
            _ ("This connection was saved with RC4 as its HOPE "
               "cipher. GtkHx no longer offers RC4 — it's a known-"
               "broken stream cipher and the \"Secure\" label it "
               "used to wear was misleading."));
    }
    g_string_append (body, "\n\n");
    g_string_append (
        body,
        _ ("Pick a replacement cipher and the bookmark will be "
           "rewritten so this prompt doesn't appear again. \"No "
           "cipher\" sends the connection in plaintext — that's "
           "less secure than Blowfish or ChaCha20-Poly1305, but at "
           "least you'll know the connection isn't protected. The "
           "server has to support whichever cipher you pick; if the "
           "negotiation fails you can change the bookmark's cipher "
           "from the Bookmarks dialog and try again."));

    dialog = ADW_ALERT_DIALOG (
        adw_alert_dialog_new (_ ("Replace RC4 cipher"), body->str));

    /* Order in the dialog matches their "increasingly cautious"
     * ranking: ChaCha20 → Blowfish → None → Cancel. The first
     * positive response (ChaCha20) is the suggested default
     * because it's the strongest of the offered options. Cancel
     * stays as the close-response so Esc / X / WM-close land
     * there. */
    adw_alert_dialog_add_response (dialog, RESP_CHACHA20,
                                   _ ("Use _ChaCha20-Poly1305"));
    adw_alert_dialog_add_response (dialog, RESP_BLOWFISH,
                                   _ ("Use _Blowfish"));
    adw_alert_dialog_add_response (dialog, RESP_NONE,
                                   _ ("Connect without _encryption"));
    adw_alert_dialog_add_response (dialog, RESP_CANCEL, _ ("Ca_ncel"));
    adw_alert_dialog_set_default_response (dialog, RESP_CHACHA20);
    adw_alert_dialog_set_close_response (dialog, RESP_CANCEL);
    adw_alert_dialog_set_response_appearance (dialog, RESP_CHACHA20,
                                              ADW_RESPONSE_SUGGESTED);
    /* "Connect without encryption" gets the destructive tint so the
     * user doesn't pick it by reflex — they have to look at the
     * red-coloured button and consciously decide they're OK with a
     * plaintext connection. */
    adw_alert_dialog_set_response_appearance (dialog, RESP_NONE,
                                              ADW_RESPONSE_DESTRUCTIVE);

    dialog_state state = { .loop = NULL, .result = -1 };
    g_signal_connect (dialog, "response",
                      G_CALLBACK (on_dialog_response), &state);

    /* Present + spin until response. */
    adw_dialog_present (ADW_DIALOG (dialog),
                        parent ? GTK_WIDGET (parent) : NULL);

    state.loop = g_main_loop_new (NULL, FALSE);
    g_main_loop_run (state.loop);
    g_main_loop_unref (state.loop);
    state.loop = NULL;

    /* Persist before returning so a caller that re-loads the same
     * bookmark mid-connection (unlikely but possible) sees the new
     * cipher byte. */
    persist_choice (bookmark_name, state.result);

    return state.result;
}
