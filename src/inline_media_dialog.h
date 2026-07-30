/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * Inline-media click-to-view dialog (Phase 9.D UI).
 *
 * Triggered from chat.c's word_click on the inline-media
 * placeholder row. Builds an AdwDialog with a GtkStack body
 * (Loading… → image | error), kicks off
 * inline_media_download_start, and swaps to the rendered image
 * (or an error message) when the download completes.
 *
 * Header bar carries two actions:
 *
 *   Save As   — writes the canonical bytes to a file the user
 *               picks via GtkFileDialog. Suffix defaulted from
 *               the canonical MIME (image/png → .png, …).
 *
 *   Open Externally — drops the bytes into a temp file and
 *               opens it via GtkFileLauncher so the OS / Flatpak
 *               portal routes to the user's preferred viewer.
 *
 * Both buttons are insensitive until the download completes.
 *
 * The dialog cancels the in-flight download on close so a
 * dismissed dialog doesn't hold the network round-trip live.
 */

#ifndef HX_INLINE_MEDIA_DIALOG_H
#define HX_INLINE_MEDIA_DIALOG_H 1

#include <glib.h>
#include <gtk/gtk.h>

#include "protocol.h"

/* Open the dialog for a known media handle. parent_widget is the
 * widget that triggered the click (typically the chat output
 * xtext); the dialog is presented over its enclosing window via
 * adw_dialog_present. mime, width_hint, height_hint, bytes_hint
 * come from the chat companion fields and are used to size the
 * loading placeholder pre-fetch; all may be 0 / NULL.
 *
 * Returns immediately; the dialog runs async. */
extern void inline_media_show_dialog (GtkWidget *parent_widget,
                                      struct htlc_conn *htlc,
                                      const guint8 *media_id,
                                      gsize media_id_len, const char *mime,
                                      guint32 width_hint, guint32 height_hint,
                                      guint32 bytes_hint);

#endif /* HX_INLINE_MEDIA_DIALOG_H */
