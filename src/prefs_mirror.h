/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * prefs_mirror.h — the read-only C view of the settings.
 *
 * `hxconfig` (rust/crates/hxconfig) owns the values and the file. This
 * translation unit owns a plain C struct holding a copy of them, refreshed
 * from Rust whenever anything changes, so the ~70 C sites that read a
 * preference keep doing exactly what they always did.
 *
 * **C reads this. C does not write it.** A write here would be silently
 * discarded by the next refresh, which is the point: the old system handed
 * out the addresses of these fields in a lookup table, so a preference could
 * be changed from anywhere and the file was rebuilt from those addresses on
 * every save. Now there is one write path — the by-name setters in options.c,
 * which go through Rust — and the file is the thing being edited rather than
 * regenerated.
 *
 * The strings are `g_strdup`ed copies owned by this file. Readers must not
 * free them or hold them across a refresh.
 *
 * `docs/preferences.md` sketched this the other way round, with Rust owning
 * the storage as a `#[repr(C)]` mirror and `_Static_assert`s pinning the
 * layout on both sides. Keeping the storage in C and refreshing it through
 * by-name getters buys the same thing and couples the two languages not at
 * all: there is no shared layout, so there is nothing to pin and nothing to
 * get wrong when a field is added.
 */

#ifndef GTKHX_PREFS_MIRROR_H
#define GTKHX_PREFS_MIRROR_H 1

#include <glib.h>

G_BEGIN_DECLS

/* Re-read every value from hxconfig. Called after the initial load and after
 * any successful set; cheap enough (a few dozen small copies) that nothing
 * tries to be clever about which fields moved. */
void hx_prefs_mirror_refresh (void);

G_END_DECLS

#endif /* ndef GTKHX_PREFS_MIRROR_H */
