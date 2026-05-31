/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * src/bookmark_cipher.h — stable bookmark cipher-byte vocabulary.
 *
 * The on-disk bookmark format (HTsc) stores cipher selection as a
 * single byte: 0 means "no cipher selected" (HOPE without a stream/
 * AEAD cipher — HOPE on/off lives in the separate `secure` byte,
 * so `secure=1, cipher=0` is a valid "HOPE-but-no-cipher" state).
 * 1+ identifies a specific cipher. Historically the byte was the
 * connect dialog's
 * AdwComboRow.selected index — which meant the byte's semantics
 * shifted every time the dropdown was reordered, with no version
 * field on the file to disambiguate.
 *
 * This module is the single source of truth for the byte ↔ cipher-
 * name mapping. The vocabulary is STABLE: bytes don't get re-used
 * when a cipher is retired, and new ciphers get new bytes at the
 * tail of the table. claude/remove-rc4 retired the RC4 cipher
 * itself but kept byte=1 ("RC4") in the vocabulary so:
 *
 *   - The save path translates the dropdown index to a stable byte
 *     via this table, so a newly-saved bookmark never accidentally
 *     writes byte=1 (which would silently load as a legacy RC4
 *     bookmark and trip the migration dialog).
 *   - The load path translates the stored byte back to a cipher
 *     name via this table; bookmarks saved before the RC4 removal
 *     with byte=2 still mean BLOWFISH (not the new dropdown's
 *     index-2 entry).
 *   - Legacy bookmarks with byte=1 are detected at connect-time
 *     and routed through the RC4 migration dialog
 *     (bookmark_rc4_dialog.h), which prompts the user for a
 *     replacement and persists the choice.
 *
 * Pure C, no GTK, drivable from Tier 1 tests.
 */

#ifndef HX_BOOKMARK_CIPHER_H
#define HX_BOOKMARK_CIPHER_H 1

#ifdef __cplusplus
extern "C" {
#endif

/* Stable bookmark-byte assignments. The numeric values are part of
 * the on-disk file format — don't change them. New ciphers get new
 * bytes at the end of the list, never re-use a retired slot. */
#define BOOKMARK_CIPHER_BYTE_NONE              0
#define BOOKMARK_CIPHER_BYTE_RC4               1 /* legacy; never offered, prompts on load */
#define BOOKMARK_CIPHER_BYTE_BLOWFISH          2
#define BOOKMARK_CIPHER_BYTE_CHACHA20_POLY1305 3

/* Resolve a stable byte to a HOPE cipher-name string ("BLOWFISH",
 * "CHACHA20-POLY1305", or "RC4" for the legacy slot). Returns NULL
 * for byte=0 ("no cipher") and for any byte outside the table
 * (corrupt/forward-compat file). The returned string is statically
 * allocated; callers don't free. */
extern const char *bookmark_cipher_name (unsigned char byte);

/* Inverse: map a HOPE cipher-name to its stable byte. Returns
 * BOOKMARK_CIPHER_BYTE_NONE for NULL / empty / unknown names. Used
 * by the save path to translate the AdwComboRow's
 * valid_ciphers[idx-1] string into a stable byte for the file. */
extern unsigned char bookmark_cipher_byte_from_name (const char *name);

#ifdef __cplusplus
}
#endif

#endif /* HX_BOOKMARK_CIPHER_H */
