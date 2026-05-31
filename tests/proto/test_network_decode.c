/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/proto/test_network_decode.c — Tier 2 coverage for
 * src/network_decode.c (the receive-side qbuf-to-qbuf decoder that
 * was extracted out of network.c specifically so this test can drive
 * it).
 *
 * The function under test is hx_decode (and the AEAD-only helper
 * hx_aead_pump_frames). htlc_read in network.c calls hx_decode in a
 * loop after every read(); each call drains as many bytes from
 * htlc->read_in into htlc->in as the rcv loop is currently waiting
 * for (htlc->in.len, which the rcv state machine sets to either
 * SIZEOF_HL_HDR for the next header or the body length parsed out
 * of a freshly-decoded header).
 *
 * Three branches in hx_decode worth pinning down end-to-end against
 * canned bytes:
 *
 *   1. Plaintext passthrough          (cipheralg empty, compress none).
 *      Drains whatever's available, splits across rcv-loop iterations
 *      that ask for fewer bytes than read_in is holding.
 *   2. Stream-cipher decode           (RC4 + Blowfish OFB-64).
 *      Symmetric: encrypt plaintext with arcfour_crypt /
 *      blowfish_ofb64_crypt, drop the ciphertext into read_in, call
 *      hx_decode, assert the plaintext comes back out into htlc->in.
 *   3. ChaCha20-Poly1305 AEAD framing (cipher_mode AEAD).
 *      Seal a plaintext payload with cipher_aead_seal into a frame,
 *      drop the framed bytes into read_in, call hx_decode (which
 *      hits hx_aead_pump_frames), assert the plaintext comes back
 *      out. Plus the two AEAD failure paths (oversized length
 *      prefix and tag mismatch) that call hx_htlc_close.
 *
 * Production resolves hx_htlc_close + hx_printf_prefix to network.c
 * / gtkhx_log.c; the test provides minimal stubs that just zero
 * htlc->fd (matching hx_htlc_close's observable side effect from
 * hx_decode's perspective) and record the most recent close
 * invocation for assertion. The compress branch is not exercised
 * here — those code paths are already drilled in test_aead.c +
 * the live Tier 3 HOPE+stream tests.
 */

#include "config.h"

#include <stdio.h>
#include <string.h>
#include <glib.h>
#include <nettle/arcfour.h>

#include "protocol.h"
#include "cipher.h"
#include "cipher_aead.h"
#include "network.h"     /* hx_htlc_close prototype */
#include "gtkhx_log.h"   /* hx_printf_prefix prototype */
#include "network_decode.h"

/* ---- Rust FFI declarations (used by the Blowfish encode side in tests) ---- */

extern BlowfishOfb64State *gtkhx_blowfish_ofb64_new (const uint8_t *key, uint32_t keylen);
extern void gtkhx_blowfish_ofb64_free (BlowfishOfb64State *state);
extern void gtkhx_blowfish_ofb64_crypt (BlowfishOfb64State *state, const uint8_t *src, uint8_t *dst, uint32_t len);

/* ---- Stubs for hx_htlc_close + hx_printf_prefix ---------------- *
 *
 * network_decode.c calls into hx_htlc_close on AEAD failure (and
 * via that, hx_printf_prefix for the user-facing log line). The
 * Tier 2 binary doesn't link network.c or gtkhx_log.c, so we
 * satisfy the references here. The stub mirrors hx_htlc_close's
 * one externally-observable side effect from hx_decode's
 * perspective — zeroing htlc->fd — and remembers the call for
 * the close-on-error tests below to assert against. */

static int  stub_close_calls;
static int  stub_close_last_expected;
static int  stub_printf_calls;

static void
reset_stub_state (void)
{
    stub_close_calls         = 0;
    stub_close_last_expected = -1;
    stub_printf_calls        = 0;
}

void
hx_htlc_close (struct htlc_conn *htlc, int expected)
{
    stub_close_calls++;
    stub_close_last_expected = expected;
    if (htlc) {
        htlc->fd = 0;
    }
}

void
hx_printf_prefix (struct htlc_conn *htlc, guint32 cid, const char *prefix,
                  const char *fmt, ...)
{
    (void) htlc;
    (void) cid;
    (void) prefix;
    (void) fmt;
    stub_printf_calls++;
}

/* INFOPREFIX is referenced as an extern const char * (session.h)
 * from src/gtkhx_log.h's printf-style trace inside the production
 * path. The stub above doesn't care what it points at, but the
 * linker still needs the symbol. */
const char *INFOPREFIX = "";

/* compress_decode and hmac_xxx are referenced from code paths that
 * none of these tests actually traverse — the compress branch in
 * hx_decode only fires when compress_decode_type != COMPRESS_NONE
 * (we always leave it at zero), and the stream-cipher rekey path in
 * cipher.c::cipher_change_*_key only fires when production's random
 * 3/16 marker triggers (we drive the encode side ourselves via raw
 * arcfour_crypt / blowfish_ofb64_crypt and never call cipher_encode).
 * The linker still needs the symbols though; provide aborting stubs
 * so a future test that accidentally trips one of these branches
 * fails loudly instead of silently misbehaving. */
u_int32_t
compress_decode (struct htlc_conn *htlc, struct qbuf *out, struct qbuf *in,
                 u_int32_t max, u_int32_t *inusedp)
{
    (void) htlc; (void) out; (void) in; (void) max; (void) inusedp;
    g_error ("compress_decode called from test — unexpected");
    return 0;
}

u_int16_t
hmac_xxx (u_int8_t *md, const void *key, u_int32_t keylen,
          const void *text, u_int32_t textlen, const char *macalg)
{
    (void) md; (void) key; (void) keylen;
    (void) text; (void) textlen; (void) macalg;
    g_error ("hmac_xxx called from test — unexpected");
    return 0;
}

/* ---- Test fixture ---------------------------------------------- */

/* The struct htlc_conn we drive in these tests is zeroed and only
 * the fields hx_decode + hx_aead_pump_frames actually read are set
 * up. Buffers are caller-owned (g_free in cleanup). */
static struct htlc_conn *
new_test_htlc (gsize in_capacity, gsize read_in_capacity,
               gsize aead_plain_capacity)
{
    struct htlc_conn *h = g_new0 (struct htlc_conn, 1);
    h->fd = 99; /* nonzero so we can detect "got torn down" */
    qbuf_set (&h->in, 0, in_capacity);
    qbuf_set (&h->read_in, 0, read_in_capacity);
    qbuf_set (&h->aead_plain, 0, aead_plain_capacity);
    /* qbuf_set leaves len at the value we asked for, but we want
     * read_in and aead_plain to start empty (capacity reserved,
     * no bytes available); h->in.len is "bytes the rcv loop is
     * waiting for" which the test will reset per call. */
    h->read_in.len    = 0;
    h->aead_plain.len = 0;
    return h;
}

static void
free_test_htlc (struct htlc_conn *h)
{
    if (h->in.buf)         g_free (h->in.buf);
    if (h->read_in.buf)    g_free (h->read_in.buf);
    if (h->aead_plain.buf) g_free (h->aead_plain.buf);
    g_free (h);
}

/* Push `len` bytes into htlc->read_in at the current append point. */
static void
feed_read_in (struct htlc_conn *h, const guint8 *bytes, gsize len)
{
    qbuf_set (&h->read_in, h->read_in.pos, h->read_in.len + len);
    memcpy (&h->read_in.buf[h->read_in.pos + h->read_in.len - len],
            bytes, len);
}

/* ---- Plaintext passthrough ------------------------------------- *
 *
 * No cipher, no compress. read_in holds N bytes, htlc->in.len says
 * the rcv loop wants K — hx_decode should copy min(N, K) into
 * htlc->in and leave the rest in read_in for the next iteration. */

static void
test_plain_full_drain (void)
{
    reset_stub_state ();
    struct htlc_conn *h = new_test_htlc (32, 64, 0);
    h->in.pos = 0;
    h->in.len = 22; /* "rcv loop wants a 22-byte header" */

    static const guint8 msg[22] = {
        0,0,0,0xc8,  /* type      */
        0,0,0,1,     /* trans     */
        0,0,0,0,     /* flag      */
        0,0,0,0,     /* len       */
        0,0,0,0,     /* len2      */
        0,0,         /* hc        */
    };
    feed_read_in (h, msg, sizeof (msg));

    unsigned int r = hx_decode (h);
    g_assert_cmpuint (r, ==, 1); /* in.len went to 0 — packet ready */
    g_assert_cmpuint (h->in.pos, ==, 22);
    g_assert_cmpmem (h->in.buf, 22, msg, 22);
    g_assert_cmpuint (h->read_in.len, ==, 0);

    free_test_htlc (h);
}

static void
test_plain_partial (void)
{
    /* read_in has 30 bytes, rcv loop wants 22 — first call serves
     * the 22, leaves 8 in read_in. Second call (rcv loop reset to
     * want another 22) starves: only 8 bytes left, returns 0
     * (more needed). */
    reset_stub_state ();
    struct htlc_conn *h = new_test_htlc (64, 64, 0);
    h->in.pos = 0;
    h->in.len = 22;

    guint8 msg[30];
    for (gsize i = 0; i < sizeof (msg); i++) {
        msg[i] = (guint8) (i + 1);
    }
    feed_read_in (h, msg, sizeof (msg));

    g_assert_cmpuint (hx_decode (h), ==, 1);
    g_assert_cmpuint (h->in.pos, ==, 22);
    g_assert_cmpmem (h->in.buf, 22, msg, 22);
    g_assert_cmpuint (h->read_in.len, ==, 8);

    /* Second iter: rcv loop reset wanting another 22-byte chunk;
     * only 8 available, hx_decode drains them and reports
     * "need more" (return 0, in.len > 0 still). */
    h->in.pos = 0;
    h->in.len = 22;
    g_assert_cmpuint (hx_decode (h), ==, 0);
    g_assert_cmpuint (h->in.pos, ==, 8);
    g_assert_cmpmem (h->in.buf, 8, msg + 22, 8);
    g_assert_cmpuint (h->read_in.len, ==, 0);

    free_test_htlc (h);
}

static void
test_plain_empty_read_in (void)
{
    /* read_in empty; nothing to do, returns 0 without touching
     * htlc->in. The early-return path. */
    reset_stub_state ();
    struct htlc_conn *h = new_test_htlc (32, 64, 0);
    h->in.pos = 0;
    h->in.len = 22;

    g_assert_cmpuint (hx_decode (h), ==, 0);
    g_assert_cmpuint (h->in.pos, ==, 0);
    g_assert_cmpuint (h->read_in.len, ==, 0);
    g_assert_cmpint (stub_close_calls, ==, 0);

    free_test_htlc (h);
}

/* ---- RC4 stream cipher round-trip ------------------------------ *
 *
 * Encrypt some plaintext with arcfour_crypt directly (mirrors what
 * cipher_encode/do_encode does for RC4), put the ciphertext into
 * read_in, configure htlc->cipher_decode_* and call hx_decode.
 * Verify plaintext is restored.                                    */

static void
test_rc4_round_trip (void)
{
    reset_stub_state ();
    struct htlc_conn *h = new_test_htlc (32, 64, 0);
    h->in.pos = 0;
    h->in.len = 22;

    static const guint8 key[16] = {
        0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    };
    static const guint8 plain[22] = {
        0xab,0xcd,0xef,0x00, 0x12,0x34,0x56,0x78,
        0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0
    };
    guint8 cipher[22];

    /* Two-arcfour-state setup. The receive side lives on htlc; the
     * encode side is local (we don't drive cipher_encode in this
     * test, only the decode half of network_decode). */
    struct arcfour_ctx encode;
    arcfour_set_key (&encode, sizeof (key), key);
    arcfour_crypt (&encode, sizeof (plain), cipher, plain);

    /* Mirror that key on the decode side. */
    strcpy (h->cipheralg, "RC4");
    h->cipher_decode_type    = CIPHER_RC4;
    h->cipher_decode_keylen  = sizeof (key);
    memcpy (h->cipher_decode_key, key, sizeof (key));
    cipher_decode_init (h);

    feed_read_in (h, cipher, sizeof (cipher));

    g_assert_cmpuint (hx_decode (h), ==, 1);
    g_assert_cmpuint (h->in.pos, ==, 22);
    g_assert_cmpmem (h->in.buf, 22, plain, 22);

    free_test_htlc (h);
}

/* ---- Blowfish OFB-64 round-trip -------------------------------- */

static void
test_blowfish_round_trip (void)
{
    reset_stub_state ();
    struct htlc_conn *h = new_test_htlc (32, 64, 0);
    h->in.pos = 0;
    h->in.len = 22;

    static const guint8 key[16] = {
        16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    };
    static const guint8 plain[22] = {
        0xde,0xad,0xbe,0xef, 0xfe,0xed,0xfa,0xce,
        1,2,3,4, 5,6,7,8, 9,10,11,12, 13,14
    };
    guint8 cipher[22];

    /* Use Rust Blowfish OFB-64 to encode the plaintext. */
    BlowfishOfb64State *encode = gtkhx_blowfish_ofb64_new (key, sizeof (key));
    gtkhx_blowfish_ofb64_crypt (encode, plain, cipher, sizeof (plain));
    gtkhx_blowfish_ofb64_free (encode);

    strcpy (h->cipheralg, "BLOWFISH");
    h->cipher_decode_type    = CIPHER_BLOWFISH;
    h->cipher_decode_keylen  = sizeof (key);
    memcpy (h->cipher_decode_key, key, sizeof (key));
    /* cipher_decode_state.stream starts NULL from zero-init;
     * cipher_decode_init will create a fresh Rust state. */
    cipher_decode_init (h);

    feed_read_in (h, cipher, sizeof (cipher));

    g_assert_cmpuint (hx_decode (h), ==, 1);
    g_assert_cmpuint (h->in.pos, ==, 22);
    g_assert_cmpmem (h->in.buf, 22, plain, 22);

    free_test_htlc (h);
}

/* ---- ChaCha20-Poly1305 AEAD framing ---------------------------- *
 *
 * Set the htlc up in AEAD mode with matched encode/decode keys,
 * seal a payload via cipher_aead_seal into a framed buffer, drop
 * the framed bytes into read_in, call hx_decode (which goes
 * through hx_aead_pump_frames), assert htlc->in gets the
 * plaintext back. The aead_plain accumulator is the staging
 * buffer; hx_decode is what bridges it into htlc->in for the
 * rcv loop. */

static void
setup_aead_state (chacha_aead_state *seal_state, chacha_aead_state *open_state)
{
    static const uint8_t key[32] = {
        32,33,34,35,36,37,38,39, 40,41,42,43,44,45,46,47,
        48,49,50,51,52,53,54,55, 56,57,58,59,60,61,62,63,
    };
    memset (seal_state, 0, sizeof (*seal_state));
    memset (open_state, 0, sizeof (*open_state));
    memcpy (seal_state->key, key, sizeof (key));
    memcpy (open_state->key, key, sizeof (key));
    /* Matched directions and counters so the test's seal output
     * is the legit input to open. */
    seal_state->dir     = CIPHER_AEAD_DIR_CLIENT_TO_SERVER;
    open_state->dir     = CIPHER_AEAD_DIR_CLIENT_TO_SERVER;
    seal_state->counter = 0;
    open_state->counter = 0;
}

static void
test_aead_round_trip (void)
{
    reset_stub_state ();
    struct htlc_conn *h = new_test_htlc (64, 256, 256);
    h->in.pos = 0;
    h->in.len = 22;

    h->cipher_mode        = CIPHER_MODE_AEAD;
    h->cipher_decode_type = CIPHER_CHACHA20_POLY1305;
    chacha_aead_state seal_state;
    setup_aead_state (&seal_state, &h->cipher_decode_state.chacha);

    static const guint8 plain[22] = {
        'H','e','l','l','o',' ','A','E','A','D',' ',
        'd','e','c','o','d','e',' ','t','e','s','t'
    };
    guint8 framed[4 + 22 + 16];
    size_t framed_len = cipher_aead_seal (&seal_state, plain, sizeof (plain),
                                          framed, sizeof (framed));
    g_assert_cmpuint (framed_len, ==, sizeof (framed));

    feed_read_in (h, framed, framed_len);

    g_assert_cmpuint (hx_decode (h), ==, 1);
    g_assert_cmpuint (h->in.pos, ==, 22);
    g_assert_cmpmem (h->in.buf, 22, plain, 22);
    /* Frame fully consumed from read_in, plaintext fully drained
     * from aead_plain into in. */
    g_assert_cmpuint (h->read_in.len, ==, 0);
    g_assert_cmpuint (h->aead_plain.len, ==, 0);
    g_assert_cmpint (stub_close_calls, ==, 0);

    free_test_htlc (h);
}

static void
test_aead_split_header_body (void)
{
    /* Same flow as test_aead_round_trip but the rcv loop asks
     * for the SIZEOF_HL_HDR-sized header first, then comes back
     * for a body chunk. hx_decode must drain aead_plain across
     * two iterations — that's the regression the
     * "checked-before-the-empty-read_in-early-return" comment
     * in hx_decode is calling out. */
    reset_stub_state ();
    struct htlc_conn *h = new_test_htlc (256, 256, 256);
    h->cipher_mode        = CIPHER_MODE_AEAD;
    h->cipher_decode_type = CIPHER_CHACHA20_POLY1305;
    chacha_aead_state seal_state;
    setup_aead_state (&seal_state, &h->cipher_decode_state.chacha);

    /* 22-byte header + 18-byte body in one sealed frame. */
    static const guint8 plain[40] = {
        0,0,0,0xd8,  0,0,0,2,  0,0,0,0,  0,0,0,18,
        0,0,0,18,    0,1,
        'b','o','d','y','-','c','o','n','t','e','n','t','-','1','8','b','!','!'
    };
    guint8 framed[4 + 40 + 16];
    size_t framed_len = cipher_aead_seal (&seal_state, plain, sizeof (plain),
                                          framed, sizeof (framed));
    g_assert_cmpuint (framed_len, ==, sizeof (framed));

    feed_read_in (h, framed, framed_len);

    /* Iter 1: rcv loop wants a 22-byte header. The AEAD pump
     * decrypts the whole frame into aead_plain, hx_decode then
     * memcpys 22 of those bytes into htlc->in. */
    h->in.pos = 0;
    h->in.len = 22;
    g_assert_cmpuint (hx_decode (h), ==, 1);
    g_assert_cmpuint (h->in.pos, ==, 22);
    g_assert_cmpmem (h->in.buf, 22, plain, 22);
    g_assert_cmpuint (h->read_in.len, ==, 0); /* frame drained */
    g_assert_cmpuint (h->aead_plain.len, ==, 18); /* body waiting */

    /* Iter 2: rcv loop reset to want the 18-byte body. read_in is
     * empty but aead_plain still has the bytes — this is the path
     * the IMPORTANT comment in hx_decode is guarding. */
    h->in.pos = 0;
    h->in.len = 18;
    g_assert_cmpuint (hx_decode (h), ==, 1);
    g_assert_cmpuint (h->in.pos, ==, 18);
    g_assert_cmpmem (h->in.buf, 18, plain + 22, 18);
    g_assert_cmpuint (h->aead_plain.len, ==, 0);

    free_test_htlc (h);
}

static void
test_aead_partial_frame_waits (void)
{
    /* Feed in less than CIPHER_AEAD_LENGTH_PREFIX worth of bytes:
     * hx_aead_pump_frames should bail out of its while loop without
     * consuming anything; hx_decode then returns 0 (aead_plain
     * empty). htlc->fd must NOT be zeroed — partial frame is the
     * "wait for more bytes" path, not an error. */
    reset_stub_state ();
    struct htlc_conn *h = new_test_htlc (32, 64, 32);
    h->in.pos = 0;
    h->in.len = 22;
    h->cipher_mode        = CIPHER_MODE_AEAD;
    h->cipher_decode_type = CIPHER_CHACHA20_POLY1305;
    chacha_aead_state ignore;
    setup_aead_state (&ignore, &h->cipher_decode_state.chacha);

    static const guint8 partial[3] = { 0, 0, 1 };
    feed_read_in (h, partial, sizeof (partial));

    g_assert_cmpuint (hx_decode (h), ==, 0);
    g_assert_cmpuint (h->in.pos, ==, 0);
    g_assert_cmpuint (h->fd, ==, 99); /* still alive */
    g_assert_cmpint (stub_close_calls, ==, 0);
    /* Partial bytes remain queued for the next read. */
    g_assert_cmpuint (h->read_in.len, ==, sizeof (partial));

    free_test_htlc (h);
}

static void
test_aead_oversized_prefix_tears_down (void)
{
    /* Length prefix larger than CIPHER_AEAD_MAX_FRAME_SIZE — the
     * pump treats this as non-recoverable and calls hx_htlc_close.
     * Our stub records the call and zeros htlc->fd. */
    reset_stub_state ();
    struct htlc_conn *h = new_test_htlc (32, 64, 32);
    h->in.pos = 0;
    h->in.len = 22;
    h->cipher_mode        = CIPHER_MODE_AEAD;
    h->cipher_decode_type = CIPHER_CHACHA20_POLY1305;
    chacha_aead_state ignore;
    setup_aead_state (&ignore, &h->cipher_decode_state.chacha);

    /* Prefix value = 0xffffffff (way past MAX_FRAME_SIZE). */
    static const guint8 bad[4] = { 0xff, 0xff, 0xff, 0xff };
    feed_read_in (h, bad, sizeof (bad));

    g_assert_cmpuint (hx_decode (h), ==, 0);
    g_assert_cmpuint (h->fd, ==, 0);
    g_assert_cmpint (stub_close_calls, ==, 1);

    free_test_htlc (h);
}

static void
test_aead_tampered_tag_tears_down (void)
{
    /* Seal a real frame, flip a byte in the Poly1305 tag, feed
     * the corrupted frame in. cipher_aead_open's verify fails;
     * hx_aead_pump_frames calls hx_htlc_close and bails. */
    reset_stub_state ();
    struct htlc_conn *h = new_test_htlc (32, 256, 64);
    h->in.pos = 0;
    h->in.len = 22;
    h->cipher_mode        = CIPHER_MODE_AEAD;
    h->cipher_decode_type = CIPHER_CHACHA20_POLY1305;
    chacha_aead_state seal_state;
    setup_aead_state (&seal_state, &h->cipher_decode_state.chacha);

    static const guint8 plain[16] = {
        't','a','m','p','e','r',' ','t',
        'e','s','t',' ','0','0','0','1'
    };
    guint8 framed[4 + 16 + 16];
    size_t framed_len = cipher_aead_seal (&seal_state, plain, sizeof (plain),
                                          framed, sizeof (framed));
    g_assert_cmpuint (framed_len, ==, sizeof (framed));

    /* Flip the last byte (inside the tag). */
    framed[framed_len - 1] ^= 0x01;

    feed_read_in (h, framed, framed_len);

    g_assert_cmpuint (hx_decode (h), ==, 0);
    g_assert_cmpuint (h->fd, ==, 0);
    g_assert_cmpint (stub_close_calls, ==, 1);

    free_test_htlc (h);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/network_decode/plain/full_drain",
                     test_plain_full_drain);
    g_test_add_func ("/network_decode/plain/partial",
                     test_plain_partial);
    g_test_add_func ("/network_decode/plain/empty_read_in",
                     test_plain_empty_read_in);
    g_test_add_func ("/network_decode/rc4/round_trip",
                     test_rc4_round_trip);
    g_test_add_func ("/network_decode/blowfish/round_trip",
                     test_blowfish_round_trip);
    g_test_add_func ("/network_decode/aead/round_trip",
                     test_aead_round_trip);
    g_test_add_func ("/network_decode/aead/split_header_body",
                     test_aead_split_header_body);
    g_test_add_func ("/network_decode/aead/partial_frame_waits",
                     test_aead_partial_frame_waits);
    g_test_add_func ("/network_decode/aead/oversized_prefix_tears_down",
                     test_aead_oversized_prefix_tears_down);
    g_test_add_func ("/network_decode/aead/tampered_tag_tears_down",
                     test_aead_tampered_tag_tears_down);

    return g_test_run ();
}
