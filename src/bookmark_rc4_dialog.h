/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * src/bookmark_rc4_dialog.h — RC4 bookmark migration prompt.
 *
 * GtkHx removed RC4 from the HOPE cipher offer list in
 * claude/remove-rc4 (insecure stream cipher, no honest "Secure"
 * label). Legacy bookmarks saved with cipher byte=1 (RC4 in the
 * stable bookmark vocabulary, see bookmark_cipher.h) need to be
 * migrated to a still-supported choice before the connect path
 * can proceed.
 *
 * This dialog runs synchronously from the bookmark-load site,
 * presents the user with four choices (No cipher / Blowfish /
 * ChaCha20-Poly1305 / Cancel), and persists their selection back
 * to the bookmark file so subsequent opens don't re-prompt.
 *
 * Uses the same spin-a-nested-GMainLoop trick as
 * tls_trust_dialog.c — the dialog is async by AdwAlertDialog's
 * nature, but we need a synchronous answer for the connect-path
 * caller.
 */

#ifndef HX_BOOKMARK_RC4_DIALOG_H
#define HX_BOOKMARK_RC4_DIALOG_H 1

#include <gtk/gtk.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Show the RC4 migration dialog modally over `parent`, prompt the
 * user for a replacement cipher, and persist the choice back to
 * the bookmark named `bookmark_name`.
 *
 * `bookmark_name` is the canonicalized filename in the bookmarks
 * directory (the same value HxBookmark.name carries and that
 * connect.c's bookmark_parse / connect_open_bookmark_by_name pass
 * around). May be NULL if there's no on-disk bookmark to update
 * (e.g. the value came from last_conn's in-memory cache) — the
 * dialog still runs and returns the user's choice, just without
 * the persistence step.
 *
 * Returns the user's chosen cipher byte (one of
 *   BOOKMARK_CIPHER_BYTE_NONE     (0)
 *   BOOKMARK_CIPHER_BYTE_BLOWFISH (2)
 *   BOOKMARK_CIPHER_BYTE_CHACHA20_POLY1305 (3)
 * ), or -1 if the user cancelled. On cancel the caller should
 * abandon the connection attempt — the bookmark stays as-is on
 * disk so a future load will re-prompt. */
extern int hx_bookmark_rc4_dialog_run_sync (GtkWindow *parent,
                                            const char *bookmark_name);

#ifdef __cplusplus
}
#endif

#endif /* HX_BOOKMARK_RC4_DIALOG_H */
