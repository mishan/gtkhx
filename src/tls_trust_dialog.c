/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

/*
 * TLS Phase 3 — TOFU prompt dialog. See tls_trust_dialog.h for
 * the API contract.
 *
 * Synchronous-from-async-callback pattern: AdwAlertDialog'\''s
 * native API is async (g_signal_connect on "response"), but the
 * GSocketClient::accept-certificate signal handler MUST return
 * TRUE/FALSE before the TLS handshake can proceed. We spin a
 * nested GMainLoop on the default GMainContext until the user
 * clicks a button, capture the decision, then quit the loop and
 * return.
 *
 * This is the same pattern Adwaita'\''s own
 * adw_alert_dialog_choose_sync wraps internally (and the same
 * pattern GTK 4 docs call out as the correct way to expose a
 * synchronous response API on top of an async dialog primitive).
 */

#include "config.h"

#include <glib.h>
#include <gtk/gtk.h>
#include <adwaita.h>

#include "compat.h" /* _() i18n macro */
#include "tls_trust.h"
#include "tls_trust_dialog.h"

/* State shared between the AdwAlertDialog::response handler and
 * the loop-spin caller. The dialog runs synchronously from the
 * caller'\''s POV, so the state lives on the stack and never
 * outlives the call. */
typedef struct {
    GMainLoop *loop;
    gboolean accepted;
} dialog_state;

static void
on_dialog_response (AdwAlertDialog *dialog G_GNUC_UNUSED,
                    const char *response,
                    gpointer user_data)
{
    dialog_state *state = user_data;
    /* Response IDs are AdwAlertDialog "response" strings — we
     * keyed them "trust" and "cancel" when building the dialog.
     * Anything that isn't "trust" — cancel, Escape, the X
     * close, the OS window-manager close — folds to a reject
     * here. Adwaita's default-response / close-response wiring
     * routes Escape and the window-close to "cancel"; we don't
     * need a separate "reject" response. */
    state->accepted = (g_strcmp0 (response, "trust") == 0);
    if (state->loop) {
        g_main_loop_quit (state->loop);
    }
}

gboolean
hx_tls_trust_dialog_run_sync (GtkWindow *parent,
                              const char *host, guint16 port,
                              const char *fingerprint,
                              hx_tls_trust_status status)
{
    g_return_val_if_fail (host != NULL, FALSE);
    g_return_val_if_fail (fingerprint != NULL, FALSE);

    const char *title;
    const char *trust_label;
    gboolean destructive;
    if (status == HX_TLS_TRUST_MISMATCH) {
        title = _ ("Certificate changed");
        trust_label = _ ("_Trust New Certificate");
        destructive = TRUE;
    } else {
        title = _ ("Unknown server certificate");
        trust_label = _ ("_Trust and Connect");
        destructive = FALSE;
    }

    /* Body builds a multi-line description: the host:port we'\''re
     * about to talk to, the SHA-256 fingerprint as the user can
     * cross-check it against a server admin or out-of-band
     * source, and (for MISMATCH) a pointer at the known_hosts
     * path so the user can edit it by hand if needed. */
    g_autofree gchar *known_hosts = hx_tls_trust_known_hosts_path ();
    g_autoptr (GString) body = g_string_new (NULL);
    if (status == HX_TLS_TRUST_MISMATCH) {
        g_string_append_printf (
            body,
            _ ("The TLS certificate for %s:%u doesn'\''t match the one "
               "you previously trusted. This usually means the server "
               "rotated its certificate, but it can also indicate a "
               "man-in-the-middle attack. Verify the fingerprint "
               "out-of-band with the server operator before accepting."),
            host, (unsigned) port);
    } else {
        g_string_append_printf (
            body,
            _ ("You haven'\''t connected to %s:%u over TLS before. "
               "GtkHx will pin this certificate so future "
               "connections are silent — but only if you trust it "
               "now."),
            host, (unsigned) port);
        g_string_append (body, "\n\n");
        g_string_append_printf (
            body,
            _ ("If %s later presents the same certificate on a "
               "different port (for example the file transfer "
               "subchannel), it will be accepted silently without "
               "another prompt."),
            host);
    }
    g_string_append (body, "\n\n");
    g_string_append_printf (body, _ ("Fingerprint:\n%s"), fingerprint);
    if (known_hosts) {
        g_string_append (body, "\n\n");
        g_string_append_printf (
            body, _ ("Pinned certificates live in %s."), known_hosts);
    }

    AdwAlertDialog *dialog = ADW_ALERT_DIALOG (
        adw_alert_dialog_new (title, body->str));

    adw_alert_dialog_add_response (dialog, "cancel", _ ("_Cancel"));
    adw_alert_dialog_add_response (dialog, "trust", trust_label);
    adw_alert_dialog_set_default_response (dialog,
                                           destructive ? "cancel" : "trust");
    adw_alert_dialog_set_close_response (dialog, "cancel");
    if (destructive) {
        adw_alert_dialog_set_response_appearance (
            dialog, "trust", ADW_RESPONSE_DESTRUCTIVE);
    } else {
        adw_alert_dialog_set_response_appearance (
            dialog, "trust", ADW_RESPONSE_SUGGESTED);
    }

    dialog_state state = { .loop = NULL, .accepted = FALSE };
    g_signal_connect (dialog, "response",
                      G_CALLBACK (on_dialog_response), &state);

    /* Present the dialog on the parent window (or as a detached
     * floating dialog if no parent is available). The
     * AdwDialog::present API doesn'\''t block; the nested loop
     * below is what makes the call synchronous from our caller'\''s
     * POV. */
    adw_dialog_present (ADW_DIALOG (dialog),
                        parent ? GTK_WIDGET (parent) : NULL);

    /* Spin the default GMainContext until the response handler
     * fires. This is safe to call from inside a GSocketClient
     * async callback because that callback was scheduled on the
     * same default context — the GSocketClient state machine is
     * pinned waiting for our return, but every other GIO
     * source (xtext redraw, timer-driven UI updates) keeps
     * running. */
    state.loop = g_main_loop_new (NULL, FALSE);
    g_main_loop_run (state.loop);
    g_main_loop_unref (state.loop);
    state.loop = NULL;

    return state.accepted;
}
