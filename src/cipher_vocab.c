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
 * cipher_vocab.c — see cipher_vocab.h. The cipher / compression
 * picker vocabulary, extracted from connect.c when the Connect dialog
 * moved to Rust (R5.3). Pure data + a handful of pure helpers; no GTK.
 */

#include "config.h"
#include <string.h>
#include "bookmark_cipher.h"
#include "cipher_vocab.h"

/* RC4 was once the first entry here and the default cipher offered to
 * old Hotline servers. It was removed in claude/remove-rc4 because RC4
 * is a known-broken stream cipher and shipping it under a "Secure"
 * label gives users a false sense of security — plaintext is more
 * honest, BLOWFISH is still acceptable as a minimum bar, and
 * CHACHA20-POLY1305 is the modern choice.
 *
 * This array is UI-only. The on-disk cipher byte uses the SEPARATE
 * stable vocabulary in bookmark_cipher.h, so reordering / shortening
 * this array doesn't shift the meaning of any bookmark byte. */
char *valid_ciphers[] = { "BLOWFISH",
                          /* preferred AEAD cipher, advertised when the
                           * connection is encrypted. The negotiation sends
                           * a multi-entry list strongest-first; server
                           * picks whichever it supports. */
                          "CHACHA20-POLY1305",
                          0 };

/* Order matters — the connect dialog shows these in the compression
 * picker, and the first-listed is the default. ZSTD first (best ratio
 * per the HOPE-Secure-Login spec), LZ4 second (fastest), GZIP last
 * (universally supported, the legacy default). */
char *valid_compressors[] = {
    "ZSTD",
    "LZ4",
    "GZIP",
    0
};

unsigned char
connect_dropdown_to_cipher_byte (unsigned int dropdown_idx)
{
    unsigned int n_valid = 0;

    if (dropdown_idx == 0) {
        return BOOKMARK_CIPHER_BYTE_NONE;
    }
    while (valid_ciphers[n_valid]) {
        n_valid++;
    }
    if (dropdown_idx > n_valid) {
        return BOOKMARK_CIPHER_BYTE_NONE;
    }
    return bookmark_cipher_byte_from_name (valid_ciphers[dropdown_idx - 1]);
}

unsigned int
connect_cipher_byte_to_dropdown (unsigned char byte)
{
    const char *name;
    unsigned int i;

    if (byte == BOOKMARK_CIPHER_BYTE_NONE) {
        return 0;
    }
    name = bookmark_cipher_name (byte);
    if (!name) {
        return 0;
    }
    for (i = 0; valid_ciphers[i]; i++) {
        if (strcmp (valid_ciphers[i], name) == 0) {
            return i + 1;
        }
    }
    /* Name resolved (e.g. "RC4") but is not in the live dropdown.
     * Caller's RC4 intercept should have caught this; surface as
     * "no cipher" defensively so the connect path doesn't auto-
     * pick a different cipher under the user's nose. */
    return 0;
}

int
valid_cipher (const char *cipheralg)
{
    unsigned int i;

    for (i = 0; valid_ciphers[i]; i++) {
        if (!strcmp (valid_ciphers[i], cipheralg)) {
            return 1;
        }
    }

    return 0;
}

int
valid_compress (const char *compressalg)
{
    unsigned int i;

    for (i = 0; valid_compressors[i]; i++) {
        if (!strcmp (valid_compressors[i], compressalg)) {
            return 1;
        }
    }

    return 0;
}

/* --- accessors for the Rust Connect dialog (connect.rs) ------------ */

/* valid_ciphers[] / valid_compressors[] are complete NULL-terminated
 * arrays in this TU, so their element count is a compile-time sizeof —
 * both count and name(i) are O(1) (minus 1 for the NULL terminator). */
#define VOCAB_N(arr) ((int) (sizeof (arr) / sizeof ((arr)[0]) - 1))

int
cipher_vocab_cipher_count (void)
{
    return VOCAB_N (valid_ciphers);
}

const char *
cipher_vocab_cipher_name (int i)
{
    if (i < 0 || i >= VOCAB_N (valid_ciphers)) {
        return NULL;
    }
    return valid_ciphers[i];
}

int
cipher_vocab_compress_count (void)
{
    return VOCAB_N (valid_compressors);
}

const char *
cipher_vocab_compress_name (int i)
{
    if (i < 0 || i >= VOCAB_N (valid_compressors)) {
        return NULL;
    }
    return valid_compressors[i];
}
