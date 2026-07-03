/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

/*
 * users_voice_col.h — the user-list voice-indicator column, kept in C.
 *
 * Split out of users_view.c when HxUserListView moved to Rust (Phase
 * R5.9). The per-cell voice-model subscription (HxVoiceModel's
 * "indicator-changed" signal, the strong-ref VoiceCellData lifetime, the
 * speaker/mute glyph mapping) is voice-stack-specific C; rather than reach
 * the HxVoiceModel FFI + a Rust `voice` cfg into the crate, the Rust view
 * asks this helper for a ready-made GtkColumnViewColumn and appends it
 * between the UID and Name columns.
 *
 * Returns NULL when built without voice (HAVE_VOICE unset) or when `sess`
 * is NULL, so the Rust caller just skips appending it.
 */

#ifndef HX_USERS_VOICE_COL_H
#define HX_USERS_VOICE_COL_H 1

#include <gtk/gtk.h>
#include "session.h"

G_BEGIN_DECLS

/* Build the voice-indicator column bound to `sess`'s voice model, or NULL
 * when voice is compiled out. Transfer-full: the caller owns the returned
 * GtkColumnViewColumn ref (append_column takes its own). */
GtkColumnViewColumn *gtkhx_users_voice_column_new (session *sess);

G_END_DECLS

#endif /* HX_USERS_VOICE_COL_H */
