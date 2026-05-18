/*
 * tests/unit/test_hmac.c — verify hmac_xxx() against canonical
 * test vectors.
 *
 * The function dispatches on the macalg string:
 *
 *   "MD5"       → plain key||text MD5 (pre-HOPE challenge construction;
 *                 NOT RFC 2104 HMAC, just GChecksum over key concatenated
 *                 with text)
 *   "SHA1"      → plain key||text SHA1 (same idea)
 *   "HMAC-MD5"  → RFC 2104 HMAC-MD5 (GHmac wrapper around GLib)
 *   "HMAC-SHA1" → RFC 2104 HMAC-SHA1 (GHmac)
 *
 * The two prefixed variants are easy to test against published
 * vectors:
 *   - RFC 2104 §3 "Test Vectors" for HMAC-MD5
 *   - RFC 2202 §2 / RFC 4231 for HMAC-SHA1
 *
 * The unprefixed "concatenate then hash" variants don't have an
 * official vector list — they're the original Hotline client's
 * pre-HOPE construction. We test those by computing the expected
 * hash by hand via GChecksum (so we're really cross-checking the
 * dispatch glue, not GLib itself).
 *
 * Don't "fix" the unprefixed branches to be RFC HMAC. The wire
 * format expects the broken-by-modern-standards key||text path
 * for older servers, and we negotiate compatibility, not
 * correctness. See hmac.c for the full rationale.
 */

#include "config.h"
#include <string.h>
#include <stdio.h>
#include <glib.h>
#include <sys/types.h>
#include "protocol.h"

/* hmac_xxx prototype lives in protocol.h. */

/* ---------- Helpers ---------- */

static void
hex_to_bytes (const char *hex, guint8 *out, gsize out_len)
{
    for (gsize i = 0; i < out_len; i++) {
        unsigned int v;
        sscanf (hex + i * 2, "%2x", &v);
        out[i] = (guint8)v;
    }
}

static void
assert_md_equals_hex (const guint8 *md, gsize md_len, const char *expected_hex)
{
    g_assert_cmpuint (strlen (expected_hex), ==, md_len * 2);
    guint8 expected[64];
    g_assert_cmpuint (md_len, <=, sizeof (expected));
    hex_to_bytes (expected_hex, expected, md_len);
    g_assert_cmpmem (md, md_len, expected, md_len);
}

/* ---------- HMAC-MD5: RFC 2104 §3 vectors ---------- */

static void
test_hmac_md5_rfc2104_vector_1 (void)
{
    /* Test Case 1:
	 *   key  = 16 × 0x0b
	 *   data = "Hi There"
	 *   HMAC-MD5 = 0x9294727a3638bb1c13f48ef8158bfc9d */
    guint8 key[16];
    memset (key, 0x0b, sizeof (key));
    const char *data = "Hi There";
    guint8 md[16] = { 0 };

    guint16 mdlen
        = hmac_xxx (md, key, sizeof (key), data, strlen (data), "HMAC-MD5");
    g_assert_cmpuint (mdlen, ==, 16);
    assert_md_equals_hex (md, 16, "9294727a3638bb1c13f48ef8158bfc9d");
}

static void
test_hmac_md5_rfc2104_vector_2 (void)
{
    /* Test Case 2:
	 *   key  = "Jefe"
	 *   data = "what do ya want for nothing?"
	 *   HMAC-MD5 = 0x750c783e6ab0b503eaa86e310a5db738 */
    const char *key = "Jefe";
    const char *data = "what do ya want for nothing?";
    guint8 md[16] = { 0 };

    guint16 mdlen
        = hmac_xxx (md, key, strlen (key), data, strlen (data), "HMAC-MD5");
    g_assert_cmpuint (mdlen, ==, 16);
    assert_md_equals_hex (md, 16, "750c783e6ab0b503eaa86e310a5db738");
}

static void
test_hmac_md5_rfc2104_vector_3 (void)
{
    /* Test Case 3:
	 *   key  = 16 × 0xaa
	 *   data = 50 × 0xdd
	 *   HMAC-MD5 = 0x56be34521d144c88dbb8c733f0e8b3f6 */
    guint8 key[16];
    memset (key, 0xaa, sizeof (key));
    guint8 data[50];
    memset (data, 0xdd, sizeof (data));
    guint8 md[16] = { 0 };

    guint16 mdlen
        = hmac_xxx (md, key, sizeof (key), data, sizeof (data), "HMAC-MD5");
    g_assert_cmpuint (mdlen, ==, 16);
    assert_md_equals_hex (md, 16, "56be34521d144c88dbb8c733f0e8b3f6");
}

/* ---------- HMAC-SHA1: RFC 2202 §3 / RFC 4231 vectors ---------- */

static void
test_hmac_sha1_rfc2202_vector_1 (void)
{
    /* Test Case 1:
	 *   key  = 20 × 0x0b
	 *   data = "Hi There"
	 *   HMAC-SHA1 = 0xb617318655057264e28bc0b6fb378c8ef146be00 */
    guint8 key[20];
    memset (key, 0x0b, sizeof (key));
    const char *data = "Hi There";
    guint8 md[20] = { 0 };

    guint16 mdlen
        = hmac_xxx (md, key, sizeof (key), data, strlen (data), "HMAC-SHA1");
    g_assert_cmpuint (mdlen, ==, 20);
    assert_md_equals_hex (md, 20, "b617318655057264e28bc0b6fb378c8ef146be00");
}

static void
test_hmac_sha1_rfc2202_vector_2 (void)
{
    /* Test Case 2:
	 *   key  = "Jefe"
	 *   data = "what do ya want for nothing?"
	 *   HMAC-SHA1 = 0xeffcdf6ae5eb2fa2d27416d5f184df9c259a7c79 */
    const char *key = "Jefe";
    const char *data = "what do ya want for nothing?";
    guint8 md[20] = { 0 };

    guint16 mdlen
        = hmac_xxx (md, key, strlen (key), data, strlen (data), "HMAC-SHA1");
    g_assert_cmpuint (mdlen, ==, 20);
    assert_md_equals_hex (md, 20, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
}

static void
test_hmac_sha1_rfc2202_vector_3 (void)
{
    /* Test Case 3:
	 *   key  = 20 × 0xaa
	 *   data = 50 × 0xdd
	 *   HMAC-SHA1 = 0x125d7342b9ac11cd91a39af48aa17b4f63f175d3 */
    guint8 key[20];
    memset (key, 0xaa, sizeof (key));
    guint8 data[50];
    memset (data, 0xdd, sizeof (data));
    guint8 md[20] = { 0 };

    guint16 mdlen
        = hmac_xxx (md, key, sizeof (key), data, sizeof (data), "HMAC-SHA1");
    g_assert_cmpuint (mdlen, ==, 20);
    assert_md_equals_hex (md, 20, "125d7342b9ac11cd91a39af48aa17b4f63f175d3");
}

/* ---------- Plain key||text MD5 / SHA1 (pre-HOPE construction) ----------
 *
 * These are NOT RFC 2104 HMAC. The hmac_xxx dispatcher computes
 * GChecksum(key concatenated with text). Cross-check the dispatch
 * glue against a hand-computed expected digest derived the same way.
 */

static void
test_unprefixed_md5_concatenates_key_and_text (void)
{
    const char *key = "secret";
    const char *data = "payload";
    guint8 md[16] = { 0 };

    /* Compute expected via GChecksum directly. */
    guint8 expected[16];
    gsize expected_len = sizeof (expected);
    GChecksum *cs = g_checksum_new (G_CHECKSUM_MD5);
    g_checksum_update (cs, (const guchar *)key, strlen (key));
    g_checksum_update (cs, (const guchar *)data, strlen (data));
    g_checksum_get_digest (cs, expected, &expected_len);
    g_checksum_free (cs);

    guint16 mdlen
        = hmac_xxx (md, key, strlen (key), data, strlen (data), "MD5");
    g_assert_cmpuint (mdlen, ==, 16);
    g_assert_cmpmem (md, mdlen, expected, expected_len);

    /* And it must NOT equal the RFC 2104 HMAC-MD5 of the same key /
	 * text — that's the whole point of the two branches. */
    guint8 hmac_md[16] = { 0 };
    hmac_xxx (hmac_md, key, strlen (key), data, strlen (data), "HMAC-MD5");
    g_assert_false (memcmp (md, hmac_md, 16) == 0);
}

static void
test_unprefixed_sha1_concatenates_key_and_text (void)
{
    const char *key = "secret";
    const char *data = "payload";
    guint8 md[20] = { 0 };

    guint8 expected[20];
    gsize expected_len = sizeof (expected);
    GChecksum *cs = g_checksum_new (G_CHECKSUM_SHA1);
    g_checksum_update (cs, (const guchar *)key, strlen (key));
    g_checksum_update (cs, (const guchar *)data, strlen (data));
    g_checksum_get_digest (cs, expected, &expected_len);
    g_checksum_free (cs);

    guint16 mdlen
        = hmac_xxx (md, key, strlen (key), data, strlen (data), "SHA1");
    g_assert_cmpuint (mdlen, ==, 20);
    g_assert_cmpmem (md, mdlen, expected, expected_len);
}

/* ---------- Unknown algorithm returns 0 ---------- */

static void
test_unknown_algorithm_returns_zero (void)
{
    guint8 md[64] = { 0 };
    guint8 key[8] = { 0 };
    guint8 data[8] = { 0 };

    /* HAVAL was deleted (advertised by no server, computed by no
	 * client). Anything we don't know goes to the "return 0" arm.
	 * SHA256 / HMAC-SHA256 used to be in this list — they're now
	 * supported (see test_hmac_sha256_*) for the HOPE-Secure-Login
	 * preferred-strongest path and the AEAD key-derivation
	 * requirement in HOPE-ChaCha20-Poly1305. */
    g_assert_cmpuint (
        hmac_xxx (md, key, sizeof (key), data, sizeof (data), "HAVAL"), ==, 0);
    g_assert_cmpuint (
        hmac_xxx (md, key, sizeof (key), data, sizeof (data), "HMAC-HAVAL"), ==,
        0);
    g_assert_cmpuint (hmac_xxx (md, key, sizeof (key), data, sizeof (data), ""),
                      ==, 0);
}

/* ---------- SHA256 / HMAC-SHA256 ---------- */

/* RFC 4231 test case 1: hmac-sha256(key="0b" x 20, data="Hi There")
 *   = b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7 */
static void
test_hmac_sha256_rfc4231_vector_1 (void)
{
    guint8 md[32] = { 0 };
    guint8 key[20];
    memset (key, 0x0b, sizeof key);
    const char *data = "Hi There";
    const guint8 expected[32] = {
        0xb0, 0x34, 0x4c, 0x61, 0xd8, 0xdb, 0x38, 0x53,
        0x5c, 0xa8, 0xaf, 0xce, 0xaf, 0x0b, 0xf1, 0x2b,
        0x88, 0x1d, 0xc2, 0x00, 0xc9, 0x83, 0x3d, 0xa7,
        0x26, 0xe9, 0x37, 0x6c, 0x2e, 0x32, 0xcf, 0xf7,
    };

    g_assert_cmpuint (hmac_xxx (md, key, sizeof key, data, strlen (data),
                                "HMAC-SHA256"),
                      ==, 32);
    g_assert_cmpmem (md, 32, expected, 32);
}

/* RFC 4231 test case 2: hmac-sha256(key="Jefe", data="what do ya...")
 *   = 5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843 */
static void
test_hmac_sha256_rfc4231_vector_2 (void)
{
    guint8 md[32] = { 0 };
    const char *key = "Jefe";
    const char *data = "what do ya want for nothing?";
    const guint8 expected[32] = {
        0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e,
        0x6a, 0x04, 0x24, 0x26, 0x08, 0x95, 0x75, 0xc7,
        0x5a, 0x00, 0x3f, 0x08, 0x9d, 0x27, 0x39, 0x83,
        0x9d, 0xec, 0x58, 0xb9, 0x64, 0xec, 0x38, 0x43,
    };

    g_assert_cmpuint (hmac_xxx (md, key, strlen (key), data, strlen (data),
                                "HMAC-SHA256"),
                      ==, 32);
    g_assert_cmpmem (md, 32, expected, 32);
}

/* Plain SHA-256 of empty input matches the published constant. */
static void
test_sha256_empty_input (void)
{
    guint8 md[32] = { 0 };
    const guint8 expected[32] = {
        0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14,
        0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
        0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c,
        0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55,
    };
    /* Plain (non-HMAC) SHA256 over key||text. With both empty the
	 * digest is just SHA-256 of the empty string. */
    g_assert_cmpuint (hmac_xxx (md, "", 0, "", 0, "SHA256"), ==, 32);
    g_assert_cmpmem (md, 32, expected, 32);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/hmac/md5_rfc2104_vector_1",
                     test_hmac_md5_rfc2104_vector_1);
    g_test_add_func ("/hmac/md5_rfc2104_vector_2",
                     test_hmac_md5_rfc2104_vector_2);
    g_test_add_func ("/hmac/md5_rfc2104_vector_3",
                     test_hmac_md5_rfc2104_vector_3);

    g_test_add_func ("/hmac/sha1_rfc2202_vector_1",
                     test_hmac_sha1_rfc2202_vector_1);
    g_test_add_func ("/hmac/sha1_rfc2202_vector_2",
                     test_hmac_sha1_rfc2202_vector_2);
    g_test_add_func ("/hmac/sha1_rfc2202_vector_3",
                     test_hmac_sha1_rfc2202_vector_3);

    g_test_add_func ("/hmac/unprefixed_md5_concatenates_key_and_text",
                     test_unprefixed_md5_concatenates_key_and_text);
    g_test_add_func ("/hmac/unprefixed_sha1_concatenates_key_and_text",
                     test_unprefixed_sha1_concatenates_key_and_text);

    g_test_add_func ("/hmac/sha256_rfc4231_vector_1",
                     test_hmac_sha256_rfc4231_vector_1);
    g_test_add_func ("/hmac/sha256_rfc4231_vector_2",
                     test_hmac_sha256_rfc4231_vector_2);
    g_test_add_func ("/hmac/sha256_empty_input", test_sha256_empty_input);

    g_test_add_func ("/hmac/unknown_algorithm_returns_zero",
                     test_unknown_algorithm_returns_zero);

    return g_test_run ();
}
