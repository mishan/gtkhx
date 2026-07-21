/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef HX_BOOKMARKS_H
#define HX_BOOKMARKS_H

/*
 * Bookmarks management dialog entry point.
 *
 * The dialog itself is the Rust `gtkhx-ui` crate's `bookmarks` module;
 * bookmark storage (a single TOML file at $CONFIG/gtkhx/bookmarks.toml,
 * with one-time import of legacy per-file HTsc bookmarks and the built-in
 * seeds) lives in the `hxbookmarks` crate. No C bookmark API remains — this
 * header just declares the toolbar's entry point into the Rust dialog.
 */

/* Open (or focus) the bookmarks management dialog over the toolbar
 * window. Idempotent — a second call while open just re-raises.
 * Implemented in gtkhx-ui's bookmarks.rs. */
extern void create_bookmarks_window (void);

#endif /* HX_BOOKMARKS_H */
