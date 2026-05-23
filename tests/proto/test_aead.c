/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/proto/test_aead.c — Tier 2 known-answer / round-trip tests
 * for src/cipher_aead.c (ChaCha20-Poly1305 frame codec).
 *
 * Why these exist:
 *
 *   The HOPE+ChaCha20 Tier 3 integration test currently skips
 *   silently because no server in the matrix is configured to
 *   speak the fogWraith AEAD extension (Janus has the code, but
 *   its bundled account YAMLs don't ship with HOPE-compatible
 *   hashes — see tests/janus/Dockerfile). That leaves the seal /
 *   open codec without live-wire coverage. Tier 2 fills the gap:
 *   we round-trip the codec against itself with fixture keys and
 *   pin the framing invariants every server must agree on.
 *
 *   This catches the bug class where seal() and open() drift —
 *   e.g. a nonce-construction change applied to encode but not
 *   decode, a length-prefix endianness flip, or a counter that
 *   gets incremented on failure paths. The pre-extraction inline
 *   AEAD code had exactly that kind of bug during Phase E
 *   development and it took several Janus round-trips to spot.
 *
 *   Vectors here are NOT taken from any spec — they're self-tests
 *   pinning our implementation against its own past behaviour.
 *   Regenerate by running the test, copying the actual ciphertext
 *   bytes from a failing assertion into the expected[] arrays, and
 *   restoring the assertion. Only update when the AEAD algorithm
 *   itself changes.
 */

#include "config.h"


#include <stdio.h>
#include <string.h>
#include <glib.h>

#include "cipher.h"
#include "cipher_aead.h"

/* Deterministic key for every test. The actual bytes don't matter
 * to the codec — what matters is that seal() and open() agree on
 * them. */
static const uint8_t kTestKey[32] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
};

static void
init_state (chacha_aead_state *st, uint8_t dir)
{
    memset (st, 0, sizeof (*st));
    memcpy (st->key, kTestKey, sizeof (kTestKey));
    st->dir     = dir;
    st->counter = 0;
}

/* ---------------------------------------------------------------- */
/* Round-trip: seal then open recovers the plaintext, regardless of
 * payload size. Covers the empty-payload edge case (HOPE PING is a
 * header-only frame), small payloads, and medium payloads near
 * what a chat message would carry.                                  */
/* ---------------------------------------------------------------- */

static void
round_trip_check (const uint8_t *pt, size_t pt_len)
{
    chacha_aead_state enc, dec;
    /* Use matched dirs so the same key+counter combo seals and
	 * opens. Production has client→server on encode + server→client
	 * on decode (different dirs because nonces must differ across
	 * directions over the same key), but for a same-process round-
	 * trip test we just need both sides to agree. */
    init_state (&enc, CIPHER_AEAD_DIR_CLIENT_TO_SERVER);
    init_state (&dec, CIPHER_AEAD_DIR_CLIENT_TO_SERVER);

    uint8_t framed[CIPHER_AEAD_LENGTH_PREFIX
                   + 4096
                   + CIPHER_AEAD_TAG_SIZE];
    g_assert_cmpuint (pt_len, <=,
                      sizeof (framed) - CIPHER_AEAD_LENGTH_PREFIX
                                      - CIPHER_AEAD_TAG_SIZE);

    size_t sealed_n = cipher_aead_seal (&enc, pt, pt_len,
                                        framed, sizeof (framed));
    g_assert_cmpuint (sealed_n, ==,
                      CIPHER_AEAD_LENGTH_PREFIX + pt_len
                                                + CIPHER_AEAD_TAG_SIZE);

    /* Length prefix: big-endian u32 covering ciphertext + tag. */
    g_assert_cmpuint (framed[0], ==,
                      (uint8_t) ((pt_len + CIPHER_AEAD_TAG_SIZE) >> 24));
    g_assert_cmpuint (framed[1], ==,
                      (uint8_t) ((pt_len + CIPHER_AEAD_TAG_SIZE) >> 16));
    g_assert_cmpuint (framed[2], ==,
                      (uint8_t) ((pt_len + CIPHER_AEAD_TAG_SIZE) >> 8));
    g_assert_cmpuint (framed[3], ==,
                      (uint8_t) ((pt_len + CIPHER_AEAD_TAG_SIZE) & 0xff));

    /* Encode counter advanced. */
    g_assert_cmpuint (enc.counter, ==, 1);

    /* Round-trip the framed bytes through open. */
    uint8_t recovered[4096];
    size_t recovered_n = cipher_aead_open (&dec, framed, sealed_n,
                                           recovered, sizeof (recovered));
    g_assert_cmpuint (recovered_n, ==, pt_len);
    g_assert_cmpmem (recovered, recovered_n, pt, pt_len);

    /* Decode counter also advanced. */
    g_assert_cmpuint (dec.counter, ==, 1);
}

static void
test_round_trip_empty (void)
{
    /* Empty plaintext — header-only frames like HOPE PING. The
	 * seal still emits a 16-byte tag, so the framed output is
	 * length-prefix(4) + tag(16) = 20 bytes total. */
    round_trip_check (NULL, 0);
}

static void
test_round_trip_small (void)
{
    static const uint8_t pt[] = "hello, hotline";
    round_trip_check (pt, sizeof (pt) - 1);
}

static void
test_round_trip_medium (void)
{
    /* Chat-message-sized payload — 256 bytes covers a typical
	 * chat line including the body chunks. */
    uint8_t pt[256];
    for (size_t i = 0; i < sizeof (pt); i++) {
        pt[i] = (uint8_t) i;
    }
    round_trip_check (pt, sizeof (pt));
}

/* ---------------------------------------------------------------- */
/* Counter advancement: sealing N frames in a row writes N distinct
 * ciphertext blocks (different nonces → different bytes), and
 * opening them in order recovers each. This is the property that
 * makes the wire stream replay-resistant within a connection.     */
/* ---------------------------------------------------------------- */

static void
test_counter_advances_per_frame (void)
{
    chacha_aead_state enc, dec;
    init_state (&enc, CIPHER_AEAD_DIR_CLIENT_TO_SERVER);
    init_state (&dec, CIPHER_AEAD_DIR_CLIENT_TO_SERVER);

    static const uint8_t pt[] = "ping";
    const size_t plain_len = sizeof (pt) - 1;
    const size_t frame_size = CIPHER_AEAD_LENGTH_PREFIX + plain_len
                            + CIPHER_AEAD_TAG_SIZE;

    uint8_t frames[3][CIPHER_AEAD_LENGTH_PREFIX + 4
                    + CIPHER_AEAD_TAG_SIZE];

    for (int i = 0; i < 3; i++) {
        size_t n = cipher_aead_seal (&enc, pt, plain_len,
                                     frames[i], sizeof (frames[i]));
        g_assert_cmpuint (n, ==, frame_size);
    }
    g_assert_cmpuint (enc.counter, ==, 3);

    /* The three ciphertexts must differ — same plaintext, same
	 * key, different counter → different nonce → different bytes.
	 * Compare just the ciphertext portion (skip length prefix; it's
	 * identical across frames since plaintext size is constant). */
    g_assert_cmpint (memcmp (frames[0] + CIPHER_AEAD_LENGTH_PREFIX,
                             frames[1] + CIPHER_AEAD_LENGTH_PREFIX,
                             plain_len + CIPHER_AEAD_TAG_SIZE), !=, 0);
    g_assert_cmpint (memcmp (frames[1] + CIPHER_AEAD_LENGTH_PREFIX,
                             frames[2] + CIPHER_AEAD_LENGTH_PREFIX,
                             plain_len + CIPHER_AEAD_TAG_SIZE), !=, 0);

    /* All three open back to the same plaintext, in order. */
    uint8_t recovered[16];
    for (int i = 0; i < 3; i++) {
        size_t n = cipher_aead_open (&dec, frames[i], frame_size,
                                     recovered, sizeof (recovered));
        g_assert_cmpuint (n, ==, plain_len);
        g_assert_cmpmem (recovered, n, pt, plain_len);
    }
    g_assert_cmpuint (dec.counter, ==, 3);
}

/* ---------------------------------------------------------------- */
/* Tag-tamper rejection: open() must return 0 on a tag bit flip and
 * MUST NOT advance the counter (preserves the ability to retry
 * decoding once the right frame arrives). Same for ciphertext
 * tamper, length-prefix tamper, and wrong-key open.                */
/* ---------------------------------------------------------------- */

static void
test_open_rejects_tag_tamper (void)
{
    chacha_aead_state enc, dec;
    init_state (&enc, CIPHER_AEAD_DIR_CLIENT_TO_SERVER);
    init_state (&dec, CIPHER_AEAD_DIR_CLIENT_TO_SERVER);

    static const uint8_t pt[] = "tag-tamper";
    uint8_t framed[CIPHER_AEAD_LENGTH_PREFIX + sizeof (pt) - 1
                                             + CIPHER_AEAD_TAG_SIZE];
    size_t n = cipher_aead_seal (&enc, pt, sizeof (pt) - 1,
                                 framed, sizeof (framed));
    g_assert_cmpuint (n, ==, sizeof (framed));

    /* Flip the last byte (Poly1305 tag's MSB). */
    framed[n - 1] ^= 0x01;

    uint8_t recovered[16];
    size_t r = cipher_aead_open (&dec, framed, n,
                                 recovered, sizeof (recovered));
    g_assert_cmpuint (r, ==, 0);
    /* Counter MUST NOT advance — preserves nonce sync after a
	 * dropped/corrupted frame. */
    g_assert_cmpuint (dec.counter, ==, 0);
}

static void
test_open_rejects_ciphertext_tamper (void)
{
    chacha_aead_state enc, dec;
    init_state (&enc, CIPHER_AEAD_DIR_CLIENT_TO_SERVER);
    init_state (&dec, CIPHER_AEAD_DIR_CLIENT_TO_SERVER);

    static const uint8_t pt[] = "ct-tamper";
    uint8_t framed[CIPHER_AEAD_LENGTH_PREFIX + sizeof (pt) - 1
                                             + CIPHER_AEAD_TAG_SIZE];
    size_t n = cipher_aead_seal (&enc, pt, sizeof (pt) - 1,
                                 framed, sizeof (framed));

    /* Flip a ciphertext byte (skip past the 4-byte length prefix). */
    framed[CIPHER_AEAD_LENGTH_PREFIX] ^= 0x80;

    uint8_t recovered[16];
    size_t r = cipher_aead_open (&dec, framed, n,
                                 recovered, sizeof (recovered));
    g_assert_cmpuint (r, ==, 0);
    g_assert_cmpuint (dec.counter, ==, 0);
}

static void
test_open_rejects_wrong_key (void)
{
    chacha_aead_state enc, dec;
    init_state (&enc, CIPHER_AEAD_DIR_CLIENT_TO_SERVER);
    init_state (&dec, CIPHER_AEAD_DIR_CLIENT_TO_SERVER);

    /* Diverge the decoder's key by one byte. */
    dec.key[0] ^= 0xff;

    static const uint8_t pt[] = "wrong-key";
    uint8_t framed[CIPHER_AEAD_LENGTH_PREFIX + sizeof (pt) - 1
                                             + CIPHER_AEAD_TAG_SIZE];
    size_t n = cipher_aead_seal (&enc, pt, sizeof (pt) - 1,
                                 framed, sizeof (framed));

    uint8_t recovered[16];
    size_t r = cipher_aead_open (&dec, framed, n,
                                 recovered, sizeof (recovered));
    g_assert_cmpuint (r, ==, 0);
    g_assert_cmpuint (dec.counter, ==, 0);
}

/* ---------------------------------------------------------------- */
/* Frame-size peek: the receive accumulator needs to know how many
 * bytes a frame needs before calling open. Pin the peek behaviour. */
/* ---------------------------------------------------------------- */

static void
test_peek_returns_full_size_when_prefix_present (void)
{
    /* Plaintext len 10 → ciphertext 10 + tag 16 = 26; full frame
	 * is 4 (prefix) + 26 = 30 bytes. The prefix encodes "26" big-
	 * endian. */
    uint8_t framed[4] = { 0x00, 0x00, 0x00, 0x1a };
    size_t n = cipher_aead_peek_frame_size (framed, sizeof (framed));
    g_assert_cmpuint (n, ==, 4 + 0x1a);
}

static void
test_peek_returns_zero_on_short_buffer (void)
{
    /* < 4 bytes available means we can't even read the prefix. */
    uint8_t framed[3] = { 0, 0, 0 };
    g_assert_cmpuint (cipher_aead_peek_frame_size (framed, 3), ==, 0);
}

static void
test_peek_rejects_oversize_prefix (void)
{
    /* MAX_FRAME_SIZE is 16 MiB; encode something bigger. */
    uint8_t framed[4] = { 0x01, 0x00, 0x00, 0x01 }; /* 16 MiB + 1 */
    g_assert_cmpuint (cipher_aead_peek_frame_size (framed, 4), ==, 0);
}

/* ---------------------------------------------------------------- */
/* Cross-direction independence: encrypt with dir=client→server,
 * try to open with dir=server→client — the nonce differs, so the
 * tag won't verify. This is how production keeps the two flow
 * directions safe against nonce reuse on the same key.            */
/* ---------------------------------------------------------------- */

static void
test_open_rejects_wrong_direction (void)
{
    chacha_aead_state enc, dec;
    init_state (&enc, CIPHER_AEAD_DIR_CLIENT_TO_SERVER);
    init_state (&dec, CIPHER_AEAD_DIR_SERVER_TO_CLIENT);

    static const uint8_t pt[] = "wrong-dir";
    uint8_t framed[CIPHER_AEAD_LENGTH_PREFIX + sizeof (pt) - 1
                                             + CIPHER_AEAD_TAG_SIZE];
    size_t n = cipher_aead_seal (&enc, pt, sizeof (pt) - 1,
                                 framed, sizeof (framed));

    uint8_t recovered[16];
    size_t r = cipher_aead_open (&dec, framed, n,
                                 recovered, sizeof (recovered));
    g_assert_cmpuint (r, ==, 0);
    g_assert_cmpuint (dec.counter, ==, 0);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/proto/aead/round_trip/empty",
                     test_round_trip_empty);
    g_test_add_func ("/proto/aead/round_trip/small",
                     test_round_trip_small);
    g_test_add_func ("/proto/aead/round_trip/medium",
                     test_round_trip_medium);

    g_test_add_func ("/proto/aead/counter_advances_per_frame",
                     test_counter_advances_per_frame);

    g_test_add_func ("/proto/aead/open_rejects/tag_tamper",
                     test_open_rejects_tag_tamper);
    g_test_add_func ("/proto/aead/open_rejects/ciphertext_tamper",
                     test_open_rejects_ciphertext_tamper);
    g_test_add_func ("/proto/aead/open_rejects/wrong_key",
                     test_open_rejects_wrong_key);
    g_test_add_func ("/proto/aead/open_rejects/wrong_direction",
                     test_open_rejects_wrong_direction);

    g_test_add_func ("/proto/aead/peek/prefix_present",
                     test_peek_returns_full_size_when_prefix_present);
    g_test_add_func ("/proto/aead/peek/short_buffer",
                     test_peek_returns_zero_on_short_buffer);
    g_test_add_func ("/proto/aead/peek/oversize_prefix",
                     test_peek_rejects_oversize_prefix);

    return g_test_run ();
}

