/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/proto/test_connect_magic.c — Tier 2 coverage for the
 * HTLS_MAGIC validator carved out of network.c::on_magic_received
 * into src/connect_magic.c.
 *
 * The headline bug this test pins down: the pre-extraction
 * production code used strncmp(HTLS_MAGIC, ctx->magic,
 * HTLS_MAGIC_LEN) for the magic comparison. HTLS_MAGIC is
 * "TRTP\0\0\0\0" — eight bytes, four of which are NUL. strncmp's
 * stop-on-NUL behaviour means that once the comparison reaches a
 * NUL byte in BOTH inputs, it returns 0 (equal) without checking
 * the remaining bytes. So a server that sent the 8-byte sequence
 * "TRTP\0XYZ" would pass the strncmp check despite the trailing
 * bytes being wrong.
 *
 * memcmp doesn't have that semantics — it compares every byte
 * unconditionally. The replacement helper uses memcmp; the test
 * below pins the contract by including the 8-byte "TRTP\0XYZ"
 * case as a deliberate negative.
 */

#include "config.h"

#include <string.h>
#include <glib.h>
#include "compat.h"             /* PACKED — hotline.h's struct attrs */
#include "hotline.h"            /* HTLS_MAGIC, HTLS_MAGIC_LEN */
#include "connect_magic.h"

static void
test_valid_magic (void)
{
    /* Exact 8-byte HTLS_MAGIC ("TRTP\0\0\0\0") passes. */
    const guint8 buf[HTLS_MAGIC_LEN]
        = { 'T', 'R', 'T', 'P', 0, 0, 0, 0 };
    g_assert_true (hx_connect_validate_server_magic (buf, HTLS_MAGIC_LEN));
}

static void
test_short_input (void)
{
    /* Anything shorter than HTLS_MAGIC_LEN is rejected — the
     * receive path reads exactly HTLS_MAGIC_LEN bytes before
     * calling, so a short reply (server closed mid-handshake)
     * shouldn't slip through as valid. */
    const guint8 buf[7] = { 'T', 'R', 'T', 'P', 0, 0, 0 };
    g_assert_false (hx_connect_validate_server_magic (buf, 7));
    g_assert_false (hx_connect_validate_server_magic (buf, 0));
}

static void
test_oversized_input (void)
{
    /* Strict length match: extra trailing bytes are NOT silently
     * ignored. The receive path is byte-exact about how much it
     * reads, so a buf size other than HTLS_MAGIC_LEN means the
     * caller is misusing the helper. Defensive FALSE. */
    const guint8 buf[16] = {
        'T', 'R', 'T', 'P', 0, 0, 0, 0,
        'e', 'x', 't', 'r', 'a', '!', '!', '!'
    };
    g_assert_false (hx_connect_validate_server_magic (buf, 16));
}

static void
test_null_input (void)
{
    g_assert_false (hx_connect_validate_server_magic (NULL, HTLS_MAGIC_LEN));
}

static void
test_strncmp_nul_terminator_bug (void)
{
    /* THE REGRESSION PIN: this is the input that would have
     * passed the pre-extraction strncmp check.
     *
     *   strncmp("TRTP\0\0\0\0", "TRTP\0XYZ", 8):
     *     - position 0..3: T=T, R=R, T=T, P=P — all equal
     *     - position 4: '\0' vs '\0' — equal AND stop (both NUL)
     *     - bytes at [5..7] are NEVER COMPARED
     *     => returns 0 (equal), and the connect path would have
     *        accepted the 8-byte "TRTP\0XYZ" sequence as a
     *        legitimate HTLS server.
     *
     * memcmp doesn't terminate on NUL — it walks all 8 bytes —
     * so the helper correctly returns FALSE here. */
    const guint8 buf[HTLS_MAGIC_LEN]
        = { 'T', 'R', 'T', 'P', 0, 'X', 'Y', 'Z' };
    g_assert_false (hx_connect_validate_server_magic (buf, HTLS_MAGIC_LEN));
}

static void
test_first_byte_wrong (void)
{
    /* Server with completely wrong magic — anything other than
     * a Hotline server (HTTP, SSH, plain garbage). Fails fast. */
    const guint8 buf[HTLS_MAGIC_LEN]
        = { 'H', 'T', 'T', 'P', '/', '1', '.', '1' };
    g_assert_false (hx_connect_validate_server_magic (buf, HTLS_MAGIC_LEN));
}

static void
test_trailing_bytes_wrong (void)
{
    /* "TRTP" prefix matches but trailing bytes are nonzero —
     * a server that's confused about what magic it should send.
     * memcmp catches this; strncmp would have caught it too,
     * since the differing bytes come BEFORE any NUL — but pinning
     * the case keeps the test rig honest. */
    const guint8 buf[HTLS_MAGIC_LEN]
        = { 'T', 'R', 'T', 'P', 0xff, 0xff, 0xff, 0xff };
    g_assert_false (hx_connect_validate_server_magic (buf, HTLS_MAGIC_LEN));
}

static void
test_constants_match_spec (void)
{
    /* HTLS_MAGIC and HTLC_MAGIC are spec-defined byte sequences,
     * not implementation details — pin them so a future "tidy
     * up the macros" can't silently change what we send / accept
     * on the wire. The two magic strings DIFFER: the client sends
     * 12-byte "TRTPHOTL\0\1\0\2" with version+sub-version trailers,
     * the server replies with 8-byte "TRTP\0\0\0\0". An earlier
     * revision of this test got that backwards and pinned the
     * wrong value for HTLC_MAGIC_LEN — pinning explicitly here
     * prevents a future tidy-up from silently homogenising them. */
    g_assert_cmpuint (HTLS_MAGIC_LEN, ==, 8);
    g_assert_cmpmem (HTLS_MAGIC, 8, "TRTP\0\0\0\0", 8);
    g_assert_cmpuint (HTLC_MAGIC_LEN, ==, 12);
    g_assert_cmpmem (HTLC_MAGIC, 12, "TRTPHOTL\0\1\0\2", 12);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/connect_magic/valid",
                     test_valid_magic);
    g_test_add_func ("/connect_magic/short_input",
                     test_short_input);
    g_test_add_func ("/connect_magic/oversized_input",
                     test_oversized_input);
    g_test_add_func ("/connect_magic/null_input",
                     test_null_input);
    g_test_add_func ("/connect_magic/strncmp_nul_terminator_bug",
                     test_strncmp_nul_terminator_bug);
    g_test_add_func ("/connect_magic/first_byte_wrong",
                     test_first_byte_wrong);
    g_test_add_func ("/connect_magic/trailing_bytes_wrong",
                     test_trailing_bytes_wrong);
    g_test_add_func ("/connect_magic/constants_match_spec",
                     test_constants_match_spec);

    return g_test_run ();
}
