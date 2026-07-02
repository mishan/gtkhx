/*
 * Copyright (C) 2000-2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 * Public License for more details.
 */

/*
 * cipher_vocab.h — the connect dialog's cipher / compression picker
 * vocabulary.
 *
 * These arrays are UI models: valid_ciphers[] / valid_compressors[]
 * back the AdwComboRow dropdowns in both the Connect dialog
 * (connect.rs) and the Bookmarks management dialog (bookmarks.c). The
 * first-listed entry is the default; item 0 in the combos is a
 * synthetic "NONE" prepended by the caller.
 *
 * The on-disk cipher byte uses a SEPARATE stable vocabulary
 * (bookmark_cipher.h), so reordering / shortening these arrays never
 * shifts the meaning of a saved bookmark byte. The two translation
 * helpers connect_dropdown_to_cipher_byte / connect_cipher_byte_to_
 * dropdown live at that boundary.
 *
 * Extracted from connect.c (which moved to Rust in R5.3): the arrays
 * stay C data so the still-C bookmarks.c can index them directly, and
 * the Rust Connect dialog reads them through the cipher_vocab_*
 * accessors below rather than importing a C data array.
 */

#ifndef HX_CIPHER_VOCAB_H
#define HX_CIPHER_VOCAB_H 1

#ifdef __cplusplus
extern "C" {
#endif

/* NULL-terminated dropdown model arrays. First entry is the default. */
extern char *valid_ciphers[];
extern char *valid_compressors[];

/* 1 if the given HOPE cipher / compression name is one this build
 * offers, 0 otherwise. */
extern int valid_cipher (const char *cipheralg);
extern int valid_compress (const char *compressalg);

/* Translate the connect dialog's AdwComboRow cipher index (0 = no
 * cipher, 1..N indexes valid_ciphers[N-1]) to a stable on-disk
 * bookmark byte from bookmark_cipher.h. Save paths use this when
 * writing the cipher byte to the HTsc bookmark file so the byte's
 * semantics stay stable across UI dropdown reorderings. */
extern unsigned char connect_dropdown_to_cipher_byte (unsigned int dropdown_idx);

/* Inverse of connect_dropdown_to_cipher_byte: translate a stable
 * bookmark cipher byte to the matching AdwComboRow index, or 0
 * ("no cipher") if the byte names a cipher the dropdown no longer
 * offers (e.g. RC4 after claude/remove-rc4). */
extern unsigned int connect_cipher_byte_to_dropdown (unsigned char byte);

/* Accessors for the Rust Connect dialog (connect.rs) — enumerate the
 * dropdown models without importing the C data arrays. Count excludes
 * the NULL terminator; name(i) returns NULL for out-of-range i. */
extern int cipher_vocab_cipher_count (void);
extern const char *cipher_vocab_cipher_name (int i);
extern int cipher_vocab_compress_count (void);
extern const char *cipher_vocab_compress_name (int i);

#ifdef __cplusplus
}
#endif

#endif /* HX_CIPHER_VOCAB_H */
