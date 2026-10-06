/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#include "config.h"
#include <stddef.h> /* offsetof — for the HxHistoryEntry layout pin */
#include <glib.h>
#include "chat_history.h"

/* ---- HxHistoryEntry layout pin --------------------------------- */

/* chat.c reads HxHistoryEntry's fields directly and the Rust gtkhx-core
 * crate (boxed/history.rs) builds the entries, so the layout is fixed on
 * both sides: these _Static_asserts against the `offset_of!` block there. */
_Static_assert (sizeof (HxHistoryEntry) == 56, "HxHistoryEntry size drift");
_Static_assert (offsetof (HxHistoryEntry, message_id) == 0, "field drift");
_Static_assert (offsetof (HxHistoryEntry, timestamp) == 8, "field drift");
_Static_assert (offsetof (HxHistoryEntry, flags) == 16, "field drift");
_Static_assert (offsetof (HxHistoryEntry, icon_id) == 18, "field drift");
_Static_assert (offsetof (HxHistoryEntry, nick) == 24, "field drift");
_Static_assert (offsetof (HxHistoryEntry, nick_len) == 32, "field drift");
_Static_assert (offsetof (HxHistoryEntry, message) == 40, "field drift");
_Static_assert (offsetof (HxHistoryEntry, message_len) == 48, "field drift");
