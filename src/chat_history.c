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

/* Every function that used to live here moved to Rust:
 *   - hx_history_entry_parse / hx_history_entry_free → the gtkhx-core crate
 *     (rust/crates/gtkhx-core/src/boxed/history.rs).
 *   - hx_get_chat_history (the TRAN 700 sender) → the hxhandlers crate
 *     (rust/crates/hxhandlers/src/send/chat_history.rs).
 *   - hx_get_chat_history_build_chunks (the pure chunk builder) → hotline-proto
 *     (native build_get_chat_history_chunks + the C-ABI shim of the same name in
 *     ffi.rs, kept for the integration harness).
 *
 * All that remains in C is the byte-layout pin for HxHistoryEntry: chat.c reads
 * the struct's fields directly, so the layout is fixed on both sides — these
 * _Static_asserts against the `offset_of!` block in history.rs. The struct
 * definition stays in chat_history.h. */
_Static_assert (sizeof (HxHistoryEntry) == 56, "HxHistoryEntry size drift");
_Static_assert (offsetof (HxHistoryEntry, message_id) == 0, "field drift");
_Static_assert (offsetof (HxHistoryEntry, timestamp) == 8, "field drift");
_Static_assert (offsetof (HxHistoryEntry, flags) == 16, "field drift");
_Static_assert (offsetof (HxHistoryEntry, icon_id) == 18, "field drift");
_Static_assert (offsetof (HxHistoryEntry, nick) == 24, "field drift");
_Static_assert (offsetof (HxHistoryEntry, nick_len) == 32, "field drift");
_Static_assert (offsetof (HxHistoryEntry, message) == 40, "field drift");
_Static_assert (offsetof (HxHistoryEntry, message_len) == 48, "field drift");
