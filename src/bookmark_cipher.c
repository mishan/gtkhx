/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * src/bookmark_cipher.c — stable bookmark cipher-byte vocabulary.
 *
 * See bookmark_cipher.h for the contract. Pure C string ops, no
 * dependencies beyond the standard library — kept this way so the
 * Tier 1 test can link this TU without GTK / GLib / Adwaita.
 */

#include "config.h"

#include <stddef.h>
#include <string.h>

#include "bookmark_cipher.h"

/* Table indexed by stable bookmark byte. Index 0 = "no cipher".
 * Indexes 1+ name a specific HOPE cipher; NULL slots mean "this
 * byte was once assigned but the cipher is now retired AND there's
 * no migration story for it" — currently empty. RC4's byte stays
 * NAMED (not NULL) because the migration dialog at connect-time
 * needs to detect a legacy RC4 bookmark by string-comparing this
 * slot's value. */
static const char *const bookmark_cipher_byte_table[] = {
    /* 0 */ NULL,
    /* 1 */ "RC4",
    /* 2 */ "BLOWFISH",
    /* 3 */ "CHACHA20-POLY1305",
};

/* G_N_ELEMENTS is a GLib macro; spell it out locally so this TU
 * doesn't pull in <glib.h> and stays linkable from the standalone
 * Tier 1 test binary. */
#define N_ELEMENTS_LOCAL(arr) (sizeof (arr) / sizeof ((arr)[0]))

const char *
bookmark_cipher_name (unsigned char byte)
{
    if ((size_t) byte < N_ELEMENTS_LOCAL (bookmark_cipher_byte_table)) {
        return bookmark_cipher_byte_table[byte];
    }
    return NULL;
}

unsigned char
bookmark_cipher_byte_from_name (const char *name)
{
    size_t i;

    if (!name || !*name) {
        return BOOKMARK_CIPHER_BYTE_NONE;
    }
    for (i = 1; i < N_ELEMENTS_LOCAL (bookmark_cipher_byte_table); i++) {
        const char *entry = bookmark_cipher_byte_table[i];
        if (entry && strcmp (entry, name) == 0) {
            return (unsigned char) i;
        }
    }
    return BOOKMARK_CIPHER_BYTE_NONE;
}
