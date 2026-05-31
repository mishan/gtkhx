/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * tests/unit/test_bookmark_cipher.c — Tier 1 pin for the stable
 * bookmark cipher-byte vocabulary.
 *
 * What it locks down:
 *
 *   - Byte ↔ name round-trip for the live entries
 *     (BLOWFISH = 2, CHACHA20-POLY1305 = 3).
 *   - "RC4" still resolves to byte=1 (the retired-but-named slot)
 *     and the load path can detect a legacy RC4 bookmark by
 *     string-comparing this slot. If a refactor accidentally
 *     NULLs out the RC4 entry, the migration dialog stops firing
 *     and old bookmarks silently switch to plaintext — exactly the
 *     UX failure the dialog is meant to prevent.
 *   - byte=0 ("no cipher") and unknown names map to NONE.
 *   - Out-of-range bytes (past the table) return NULL rather than
 *     dereferencing past the array.
 *
 * No GLib / GTK dependency in the module under test — this binary
 * links only bookmark_cipher.c + the test source.
 */

#include "config.h"

#include <glib.h>
#include <string.h>

#include "bookmark_cipher.h"

static void
test_byte_to_name_known_entries (void)
{
    g_assert_null (bookmark_cipher_name (BOOKMARK_CIPHER_BYTE_NONE));
    g_assert_cmpstr (bookmark_cipher_name (BOOKMARK_CIPHER_BYTE_RC4),
                     ==, "RC4");
    g_assert_cmpstr (bookmark_cipher_name (BOOKMARK_CIPHER_BYTE_BLOWFISH),
                     ==, "BLOWFISH");
    g_assert_cmpstr (
        bookmark_cipher_name (BOOKMARK_CIPHER_BYTE_CHACHA20_POLY1305),
        ==, "CHACHA20-POLY1305");
}

static void
test_byte_assignments_are_stable (void)
{
    /* These integers are part of the on-disk format. Don't change
     * them — bumping any of these breaks every bookmark currently
     * on disk. Pin the numeric values explicitly so a renumber
     * trips this test before the bookmark dir does. */
    g_assert_cmpint (BOOKMARK_CIPHER_BYTE_NONE,               ==, 0);
    g_assert_cmpint (BOOKMARK_CIPHER_BYTE_RC4,                ==, 1);
    g_assert_cmpint (BOOKMARK_CIPHER_BYTE_BLOWFISH,           ==, 2);
    g_assert_cmpint (BOOKMARK_CIPHER_BYTE_CHACHA20_POLY1305,  ==, 3);
}

static void
test_byte_to_name_out_of_range (void)
{
    /* Past the end of the table. The save path can only write
     * stable bytes, but a corrupt or forward-format file could
     * carry anything; verify we return NULL instead of
     * dereferencing past the array. */
    g_assert_null (bookmark_cipher_name (4));
    g_assert_null (bookmark_cipher_name (42));
    g_assert_null (bookmark_cipher_name (255));
}

static void
test_name_to_byte_known_entries (void)
{
    g_assert_cmpint (bookmark_cipher_byte_from_name ("RC4"),
                     ==, BOOKMARK_CIPHER_BYTE_RC4);
    g_assert_cmpint (bookmark_cipher_byte_from_name ("BLOWFISH"),
                     ==, BOOKMARK_CIPHER_BYTE_BLOWFISH);
    g_assert_cmpint (bookmark_cipher_byte_from_name ("CHACHA20-POLY1305"),
                     ==, BOOKMARK_CIPHER_BYTE_CHACHA20_POLY1305);
}

static void
test_name_to_byte_edge_cases (void)
{
    g_assert_cmpint (bookmark_cipher_byte_from_name (NULL),
                     ==, BOOKMARK_CIPHER_BYTE_NONE);
    g_assert_cmpint (bookmark_cipher_byte_from_name (""),
                     ==, BOOKMARK_CIPHER_BYTE_NONE);
    g_assert_cmpint (bookmark_cipher_byte_from_name ("AES-256"),
                     ==, BOOKMARK_CIPHER_BYTE_NONE);
    /* Case-sensitive on purpose — HOPE cipher names are uppercase
     * on the wire and the dispatch comparisons elsewhere
     * (hope_cipher_id_from_name, valid_cipher) are case-sensitive
     * too. */
    g_assert_cmpint (bookmark_cipher_byte_from_name ("blowfish"),
                     ==, BOOKMARK_CIPHER_BYTE_NONE);
}

static void
test_round_trip (void)
{
    /* Every live byte → name → byte returns the same byte. RC4 is
     * intentionally included even though it's no longer offered;
     * the round-trip property is what the load-time migration
     * detector relies on. */
    for (unsigned char b = 1; b <= BOOKMARK_CIPHER_BYTE_CHACHA20_POLY1305;
         b++) {
        const char *name = bookmark_cipher_name (b);
        g_assert_nonnull (name);
        unsigned char round = bookmark_cipher_byte_from_name (name);
        g_assert_cmpint (round, ==, b);
    }
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/bookmark_cipher/byte_to_name/known_entries",
                     test_byte_to_name_known_entries);
    g_test_add_func ("/bookmark_cipher/byte_assignments_are_stable",
                     test_byte_assignments_are_stable);
    g_test_add_func ("/bookmark_cipher/byte_to_name/out_of_range",
                     test_byte_to_name_out_of_range);
    g_test_add_func ("/bookmark_cipher/name_to_byte/known_entries",
                     test_name_to_byte_known_entries);
    g_test_add_func ("/bookmark_cipher/name_to_byte/edge_cases",
                     test_name_to_byte_edge_cases);
    g_test_add_func ("/bookmark_cipher/round_trip", test_round_trip);

    return g_test_run ();
}
