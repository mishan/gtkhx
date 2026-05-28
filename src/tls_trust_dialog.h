#ifndef HX_TLS_TRUST_DIALOG_H
#define HX_TLS_TRUST_DIALOG_H 1

/*
 * TLS Phase 3 — TOFU prompt dialog.
 *
 * Wraps an AdwAlertDialog to show the user a "first-time-seen
 * cert" (or "MISMATCH — cert rotated or MITM") prompt synchronous
 * from inside the GSocketClient::accept-certificate signal
 * handler. The signal contract is sync (return TRUE/FALSE), so
 * the dialog spins a nested GMainLoop on the default context
 * until the user clicks a button — same pattern Adwaita uses
 * internally for adw_alert_dialog_choose_sync.
 *
 * This module is the UI surface; the storage / fingerprint /
 * lookup logic lives in tls_trust.{c,h}. The wiring inside
 * network.c looks up the cert, decides whether to prompt at all,
 * and only calls this dialog for UNKNOWN / MISMATCH outcomes.
 */

#include <glib.h>
#include <gtk/gtk.h>
#include "tls_trust.h"

/*
 * Show the TOFU prompt for (host, port, fingerprint, status).
 *
 * `status` shapes the dialog's title + body text:
 *   - HX_TLS_TRUST_UNKNOWN: "First time seeing this server" tone.
 *     Default action button is "Trust and Connect".
 *   - HX_TLS_TRUST_MISMATCH: "Certificate changed" warning tone
 *     with destructive styling on the accept button. Body text
 *     mentions cert rotation vs. MITM and tells the user where
 *     to find the existing pin (the known_hosts path).
 *   - HX_TLS_TRUST_TRUSTED: never reached — caller skips the
 *     prompt when the cert is already trusted. Treat as
 *     UNKNOWN if it somehow surfaces.
 *
 * `parent` is the transient-for window (typically the toolbar
 * window — the only top-level GtkHx always has up). NULL is
 * tolerated but the dialog will be unparented; pass a window
 * when one is available.
 *
 * Returns TRUE if the user pressed "Trust and Connect" (or the
 * MISMATCH equivalent), FALSE otherwise. Caller pins via
 * hx_tls_trust_pin on TRUE; on FALSE the TLS handshake aborts.
 *
 * Must be called on the GTK main thread.
 */
extern gboolean
hx_tls_trust_dialog_run_sync (GtkWindow *parent,
                              const char *host, guint16 port,
                              const char *fingerprint,
                              hx_tls_trust_status status);

#endif /* HX_TLS_TRUST_DIALOG_H */
