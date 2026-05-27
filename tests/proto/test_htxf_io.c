/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * tests/proto/test_htxf_io.c — round-trip and edge-case coverage for
 * src/htxf_io.c (htxf_io_read / htxf_io_write).
 *
 * Why these exist:
 *
 *   htxf_io_* is the seam between the HTXF worker threads and the
 *   on-wire AEAD frame codec. Phase E of the HTXF GIOStream port
 *   replaced raw read(2)/write(2) loops with g_input_stream_read /
 *   g_output_stream_write_all calls on a GIOStream — and routed the
 *   bulk-data fork through these helpers, where previously the
 *   read()/write() loops in rd_wr bypassed them entirely. The Tier 3
 *   matrix covers the happy path against live mhxd / janus, but a
 *   silent AEAD framing regression (counter desync, length-prefix
 *   endianness flip, EOF-handling bug) would survive Tier 3 if the
 *   server happened to send small enough frames that the bug didn't
 *   trip. Tier 2 catches that class against a deterministic
 *   in-memory transport.
 *
 *   GMemoryInputStream / GMemoryOutputStream wrapped in
 *   GSimpleIOStream gives us a GIOStream we can drive byte-by-byte
 *   without sockets, without docker, without a server. seal-on-one,
 *   open-on-another verifies the frame survives the round-trip;
 *   feeding truncated bytes verifies the error paths surface
 *   errno=EIO the way callers expect.
 */

#include "config.h"

#include <errno.h>
#include <string.h>

#include <gio/gio.h>
#include <glib.h>

#include "compat.h"   /* PACKED — required before protocol.h */
#include "hotline.h"
#include "protocol.h"
#include "cipher.h"
#include "cipher_aead.h"
#include "htxf_io.h"

/* ---------------------------------------------------------------- */
/* GIOStream test fixture: a GSimpleIOStream wrapping a memory
 * input stream over `in_bytes` and a memory output stream that
 * grows as we write to it. Caller owns the bytes the input stream
 * reads from; we just take a borrow for the lifetime of the
 * GIOStream.                                                      */
/* ---------------------------------------------------------------- */

static GIOStream *
make_mem_iostream (const void *in_bytes, gsize in_len,
                   GMemoryOutputStream **out_mem)
{
    GInputStream *in
        = g_memory_input_stream_new_from_data (in_bytes, in_len, NULL);
    GOutputStream *out = g_memory_output_stream_new_resizable ();
    if (out_mem) {
        *out_mem = G_MEMORY_OUTPUT_STREAM (out);
    }
    GIOStream *io = g_simple_io_stream_new (in, out);
    /* The GSimpleIOStream holds its own refs on the inner streams;
	 * drop ours so the io_stream owns the whole graph cleanly. */
    g_object_unref (in);
    g_object_unref (out);
    return io;
}

/* Seed an htxf_conn for an AEAD session against a fixed key. Two
 * peers using the same kTestKey + matching direction will speak
 * the same AEAD wire bytes; that's what makes the round-trip
 * tests possible without spinning a real session.                 */
static const uint8_t kTestKey[32] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
};

static void
arm_aead (struct htxf_conn *xfer, uint8_t dir)
{
    memset (xfer, 0, sizeof (*xfer));
    xfer->aead_active = TRUE;
    xfer->xfer_encode.dir = dir;
    xfer->xfer_decode.dir = dir;
    memcpy (xfer->xfer_encode.key, kTestKey, sizeof (kTestKey));
    memcpy (xfer->xfer_decode.key, kTestKey, sizeof (kTestKey));
    /* Counters start at 0; htxf_io_release will clear the
	 * accumulators on teardown. */
}

/* ---------------------------------------------------------------- */
/* Plaintext mode: with aead_active=FALSE, htxf_io_read /
 * htxf_io_write should behave like a thin g_input_stream_read /
 * g_output_stream_write_all wrapper. Round-trip preserves bytes.   */
/* ---------------------------------------------------------------- */

static void
test_plain_round_trip (void)
{
    struct htxf_conn xfer;
    memset (&xfer, 0, sizeof (xfer));
    /* aead_active stays FALSE — plaintext path. */

    static const uint8_t payload[] = "hello, hotline";
    const gsize payload_len = sizeof (payload) - 1;

    /* Write half: feed an empty input + capture the output. */
    GMemoryOutputStream *out_mem = NULL;
    GIOStream *write_io = make_mem_iostream (NULL, 0, &out_mem);

    ssize_t w = htxf_io_write (&xfer, write_io, payload, payload_len);
    g_assert_cmpint (w, ==, (ssize_t) payload_len);

    /* Pull the bytes out of the output sink so we can feed them
	 * back through the read side. */
    gsize captured_len = g_memory_output_stream_get_data_size (out_mem);
    g_assert_cmpuint (captured_len, ==, payload_len);
    g_autofree guint8 *captured = g_memdup2 (
        g_memory_output_stream_get_data (out_mem), captured_len);

    g_object_unref (write_io);

    /* Read half: feed the captured bytes through a fresh
	 * GIOStream. */
    GIOStream *read_io = make_mem_iostream (captured, captured_len, NULL);
    uint8_t buf[64];
    ssize_t r = htxf_io_read (&xfer, read_io, buf, sizeof (buf));
    g_assert_cmpint (r, ==, (ssize_t) payload_len);
    g_assert_cmpmem (buf, r, payload, payload_len);

    g_object_unref (read_io);
}

static void
test_plain_eof (void)
{
    struct htxf_conn xfer;
    memset (&xfer, 0, sizeof (xfer));

    GIOStream *io = make_mem_iostream (NULL, 0, NULL);
    uint8_t buf[16];
    ssize_t r = htxf_io_read (&xfer, io, buf, sizeof (buf));
    g_assert_cmpint (r, ==, 0); /* clean EOF */
    g_object_unref (io);
}

/* ---------------------------------------------------------------- */
/* AEAD mode: seal-then-open against the same kTestKey recovers the
 * plaintext through the GIOStream wrappers. Counter advances on
 * both sides. Multiple frames in sequence work too — htxf_io_read
 * serves out of the plaintext accumulator and refills frame by
 * frame.                                                          */
/* ---------------------------------------------------------------- */

static void
aead_round_trip_check (const uint8_t *pt, size_t pt_len)
{
    struct htxf_conn enc_xfer;
    struct htxf_conn dec_xfer;
    arm_aead (&enc_xfer, CIPHER_AEAD_DIR_CLIENT_TO_SERVER);
    arm_aead (&dec_xfer, CIPHER_AEAD_DIR_CLIENT_TO_SERVER);

    /* Seal via htxf_io_write. */
    GMemoryOutputStream *out_mem = NULL;
    GIOStream *write_io = make_mem_iostream (NULL, 0, &out_mem);
    ssize_t w = htxf_io_write (&enc_xfer, write_io, pt, pt_len);
    g_assert_cmpint (w, ==, (ssize_t) pt_len);
    g_assert_cmpuint (enc_xfer.xfer_encode.counter, ==, 1);

    gsize sealed_len = g_memory_output_stream_get_data_size (out_mem);
    /* Framed length = 4 (length prefix) + pt_len + 16 (Poly1305 tag). */
    g_assert_cmpuint (sealed_len, ==,
                      CIPHER_AEAD_LENGTH_PREFIX + pt_len
                          + CIPHER_AEAD_TAG_SIZE);
    g_autofree guint8 *sealed = g_memdup2 (
        g_memory_output_stream_get_data (out_mem), sealed_len);
    g_object_unref (write_io);

    /* Open via htxf_io_read against the same bytes. */
    GIOStream *read_io = make_mem_iostream (sealed, sealed_len, NULL);
    g_autofree uint8_t *recovered = g_malloc (pt_len + 16);
    ssize_t r = htxf_io_read (&dec_xfer, read_io, recovered,
                              pt_len + 16);
    g_assert_cmpint (r, ==, (ssize_t) pt_len);
    if (pt_len > 0) {
        g_assert_cmpmem (recovered, r, pt, pt_len);
    }
    g_assert_cmpuint (dec_xfer.xfer_decode.counter, ==, 1);

    g_object_unref (read_io);
    htxf_io_release (&enc_xfer);
    htxf_io_release (&dec_xfer);
}

static void
test_aead_round_trip_small (void)
{
    static const uint8_t pt[] = "ping";
    aead_round_trip_check (pt, sizeof (pt) - 1);
}

static void
test_aead_round_trip_medium (void)
{
    /* 256 bytes — typical chat-message-sized payload. */
    uint8_t pt[256];
    for (size_t i = 0; i < sizeof (pt); i++) {
        pt[i] = (uint8_t) i;
    }
    aead_round_trip_check (pt, sizeof (pt));
}

static void
test_aead_round_trip_empty (void)
{
    /* Empty plaintext — htxf_io_write returns 0 without producing
	 * a frame on the wire. Matches the AEAD aead_write contract
	 * that returns 0 on len==0 without advancing the counter. */
    struct htxf_conn enc_xfer;
    arm_aead (&enc_xfer, CIPHER_AEAD_DIR_CLIENT_TO_SERVER);

    GMemoryOutputStream *out_mem = NULL;
    GIOStream *write_io = make_mem_iostream (NULL, 0, &out_mem);
    ssize_t w = htxf_io_write (&enc_xfer, write_io, NULL, 0);
    g_assert_cmpint (w, ==, 0);
    g_assert_cmpuint (g_memory_output_stream_get_data_size (out_mem),
                      ==, 0);
    g_assert_cmpuint (enc_xfer.xfer_encode.counter, ==, 0);

    g_object_unref (write_io);
    htxf_io_release (&enc_xfer);
}

/* Three frames in sequence: serve them through a single
 * htxf_io_read at a time, then confirm the accumulator-then-refill
 * loop in aead_read handles back-to-back frames cleanly. This is
 * the exact code path the file-transfer bulk loop hits. */
static void
test_aead_multi_frame_sequence (void)
{
    struct htxf_conn enc_xfer;
    struct htxf_conn dec_xfer;
    arm_aead (&enc_xfer, CIPHER_AEAD_DIR_CLIENT_TO_SERVER);
    arm_aead (&dec_xfer, CIPHER_AEAD_DIR_CLIENT_TO_SERVER);

    static const uint8_t pt[] = "ping";
    const gsize pt_len = sizeof (pt) - 1;

    /* Seal three frames into one buffer. */
    GMemoryOutputStream *out_mem = NULL;
    GIOStream *write_io = make_mem_iostream (NULL, 0, &out_mem);
    for (int i = 0; i < 3; i++) {
        ssize_t w = htxf_io_write (&enc_xfer, write_io, pt, pt_len);
        g_assert_cmpint (w, ==, (ssize_t) pt_len);
    }
    g_assert_cmpuint (enc_xfer.xfer_encode.counter, ==, 3);
    gsize total_sealed = g_memory_output_stream_get_data_size (out_mem);
    g_autofree guint8 *sealed = g_memdup2 (
        g_memory_output_stream_get_data (out_mem), total_sealed);
    g_object_unref (write_io);

    /* Open through the same decoder — each htxf_io_read should
	 * yield exactly pt_len bytes from one frame, then refill. */
    GIOStream *read_io = make_mem_iostream (sealed, total_sealed, NULL);
    for (int i = 0; i < 3; i++) {
        uint8_t buf[16];
        ssize_t r = htxf_io_read (&dec_xfer, read_io, buf, sizeof (buf));
        g_assert_cmpint (r, ==, (ssize_t) pt_len);
        g_assert_cmpmem (buf, r, pt, pt_len);
    }
    g_assert_cmpuint (dec_xfer.xfer_decode.counter, ==, 3);

    /* After all three are consumed, the next read sees clean EOF. */
    uint8_t tail[8];
    ssize_t r_eof = htxf_io_read (&dec_xfer, read_io, tail, sizeof (tail));
    g_assert_cmpint (r_eof, ==, 0);

    g_object_unref (read_io);
    htxf_io_release (&enc_xfer);
    htxf_io_release (&dec_xfer);
}

/* ---------------------------------------------------------------- */
/* Error paths: oversized write, truncated frame mid-stream.       */
/* ---------------------------------------------------------------- */

static void
test_aead_oversized_write_rejected (void)
{
    struct htxf_conn enc_xfer;
    arm_aead (&enc_xfer, CIPHER_AEAD_DIR_CLIENT_TO_SERVER);

    /* CIPHER_AEAD_MAX_FRAME_SIZE is the cap on the length-prefixed
	 * body (ciphertext + tag) — the 4-byte length prefix sits in
	 * front of the cap, not inside it. So the maximum acceptable
	 * plaintext is (MAX - TAG); anything larger must surface as
	 * -1 with errno=EMSGSIZE. cipher_aead_seal enforces the same
	 * boundary internally; this test pins the htxf_io_write
	 * wrapper to the same value so the two can't drift apart. */
    const size_t max_pt = CIPHER_AEAD_MAX_FRAME_SIZE
                          - CIPHER_AEAD_TAG_SIZE;
    g_autofree uint8_t *pt = g_malloc (max_pt + 1);
    memset (pt, 0xAA, max_pt + 1);

    GIOStream *write_io = make_mem_iostream (NULL, 0, NULL);
    errno = 0;
    ssize_t w = htxf_io_write (&enc_xfer, write_io, pt, max_pt + 1);
    g_assert_cmpint (w, ==, -1);
    g_assert_cmpint (errno, ==, EMSGSIZE);
    /* Counter must NOT advance on rejected writes. */
    g_assert_cmpuint (enc_xfer.xfer_encode.counter, ==, 0);

    g_object_unref (write_io);
    htxf_io_release (&enc_xfer);
}

/* Sibling to the rejection test: a plaintext of exactly MAX - TAG
 * bytes (the largest value cipher_aead_seal will accept) must NOT
 * be rejected by the wrapper. Catches an off-by-N regression in
 * the htxf_io_write guard — earlier the guard subtracted both
 * the 4-byte length prefix AND the tag, which locked out a
 * 4-byte sliver right at the spec boundary. We don't actually
 * exercise the round-trip (allocating 32 MiB of memory and
 * letting Poly1305 chew on it is slow for Tier 2); just confirm
 * the wrapper accepts the size and produces a frame. */
static void
test_aead_max_write_accepted (void)
{
    struct htxf_conn enc_xfer;
    arm_aead (&enc_xfer, CIPHER_AEAD_DIR_CLIENT_TO_SERVER);

    const size_t max_pt = CIPHER_AEAD_MAX_FRAME_SIZE
                          - CIPHER_AEAD_TAG_SIZE;
    g_autofree uint8_t *pt = g_malloc0 (max_pt);

    GMemoryOutputStream *out_mem = NULL;
    GIOStream *write_io = make_mem_iostream (NULL, 0, &out_mem);
    errno = 0;
    ssize_t w = htxf_io_write (&enc_xfer, write_io, pt, max_pt);
    g_assert_cmpint (w, ==, (ssize_t) max_pt);
    g_assert_cmpuint (enc_xfer.xfer_encode.counter, ==, 1);
    /* The on-wire framed size = 4 (prefix) + max_pt + 16 (tag). */
    gsize sealed_len = g_memory_output_stream_get_data_size (out_mem);
    g_assert_cmpuint (sealed_len, ==,
                      CIPHER_AEAD_LENGTH_PREFIX + max_pt
                          + CIPHER_AEAD_TAG_SIZE);

    g_object_unref (write_io);
    htxf_io_release (&enc_xfer);
}

/* NULL io rejection: the new pointer-based API is sharper than the
 * old fd-based one — passing -1 as an fd would have surfaced
 * EBADF through the kernel; passing NULL as a GIOStream would
 * crash on the unconditional dereference in
 * g_io_stream_get_input_stream. Reject NULL up front with
 * errno=EINVAL so callers get a predictable shape, and pin that
 * contract here so a future "simplify the NULL check away"
 * refactor doesn't reintroduce the segfault. */
static void
test_null_io_read_returns_einval (void)
{
    struct htxf_conn xfer;
    memset (&xfer, 0, sizeof (xfer));
    uint8_t buf[8];
    errno = 0;
    ssize_t r = htxf_io_read (&xfer, NULL, buf, sizeof (buf));
    g_assert_cmpint (r, ==, -1);
    g_assert_cmpint (errno, ==, EINVAL);
}

static void
test_null_io_write_returns_einval (void)
{
    struct htxf_conn xfer;
    memset (&xfer, 0, sizeof (xfer));
    /* AEAD-active or plain doesn't matter — the NULL check must
	 * fire BEFORE we ever touch htxf->aead_active. Exercise both
	 * shapes to confirm there's no branch that sneaks past the
	 * NULL guard. */
    uint8_t buf[8] = { 0 };
    errno = 0;
    g_assert_cmpint (htxf_io_write (&xfer, NULL, buf, sizeof (buf)), ==, -1);
    g_assert_cmpint (errno, ==, EINVAL);

    arm_aead (&xfer, CIPHER_AEAD_DIR_CLIENT_TO_SERVER);
    errno = 0;
    g_assert_cmpint (htxf_io_write (&xfer, NULL, buf, sizeof (buf)), ==, -1);
    g_assert_cmpint (errno, ==, EINVAL);
    /* Counter must NOT advance on rejected writes. */
    g_assert_cmpuint (xfer.xfer_encode.counter, ==, 0);
    htxf_io_release (&xfer);
}

/* Truncated frame: feed only the length prefix to the decoder.
 * htxf_io_read should read the prefix, try to refill the body,
 * see EOF mid-frame, and return -1 with errno=EIO (not 0, which
 * would mislead the caller into thinking the transfer ended
 * cleanly).                                                       */
static void
test_aead_truncated_frame_surfaces_eio (void)
{
    struct htxf_conn enc_xfer;
    struct htxf_conn dec_xfer;
    arm_aead (&enc_xfer, CIPHER_AEAD_DIR_CLIENT_TO_SERVER);
    arm_aead (&dec_xfer, CIPHER_AEAD_DIR_CLIENT_TO_SERVER);

    /* Seal something so we have a real frame to truncate. */
    static const uint8_t pt[] = "truncated";
    const gsize pt_len = sizeof (pt) - 1;
    GMemoryOutputStream *out_mem = NULL;
    GIOStream *write_io = make_mem_iostream (NULL, 0, &out_mem);
    g_assert_cmpint (htxf_io_write (&enc_xfer, write_io, pt, pt_len),
                     ==, (ssize_t) pt_len);
    gsize sealed_len = g_memory_output_stream_get_data_size (out_mem);
    g_autofree guint8 *sealed = g_memdup2 (
        g_memory_output_stream_get_data (out_mem), sealed_len);
    g_object_unref (write_io);

    /* Deliver only the 4-byte length prefix + a couple body
	 * bytes — enough to convince the reader a frame is incoming
	 * but not enough to finish it. */
    gsize delivered = CIPHER_AEAD_LENGTH_PREFIX + 2;
    g_assert_cmpuint (delivered, <, sealed_len);

    GIOStream *read_io = make_mem_iostream (sealed, delivered, NULL);
    uint8_t buf[64];
    errno = 0;
    ssize_t r = htxf_io_read (&dec_xfer, read_io, buf, sizeof (buf));
    g_assert_cmpint (r, ==, -1);
    g_assert_cmpint (errno, ==, EIO);

    g_object_unref (read_io);
    htxf_io_release (&enc_xfer);
    htxf_io_release (&dec_xfer);
}

/* Tag tamper: bit-flip the tail of a sealed frame and confirm
 * the decoder rejects it with -1 / errno=EIO rather than silently
 * surfacing garbage. cipher_aead_open already has its own tamper
 * tests (test_aead.c), but this test pins the contract at the
 * htxf_io_read seam — the wrapper must NOT swallow the failure. */
static void
test_aead_tag_tamper_surfaces_eio (void)
{
    struct htxf_conn enc_xfer;
    struct htxf_conn dec_xfer;
    arm_aead (&enc_xfer, CIPHER_AEAD_DIR_CLIENT_TO_SERVER);
    arm_aead (&dec_xfer, CIPHER_AEAD_DIR_CLIENT_TO_SERVER);

    static const uint8_t pt[] = "tampered";
    const gsize pt_len = sizeof (pt) - 1;
    GMemoryOutputStream *out_mem = NULL;
    GIOStream *write_io = make_mem_iostream (NULL, 0, &out_mem);
    g_assert_cmpint (htxf_io_write (&enc_xfer, write_io, pt, pt_len),
                     ==, (ssize_t) pt_len);
    gsize sealed_len = g_memory_output_stream_get_data_size (out_mem);
    g_autofree guint8 *sealed = g_memdup2 (
        g_memory_output_stream_get_data (out_mem), sealed_len);
    g_object_unref (write_io);

    /* Flip the last tag byte. */
    sealed[sealed_len - 1] ^= 0x01;

    GIOStream *read_io = make_mem_iostream (sealed, sealed_len, NULL);
    uint8_t buf[64];
    errno = 0;
    ssize_t r = htxf_io_read (&dec_xfer, read_io, buf, sizeof (buf));
    g_assert_cmpint (r, ==, -1);
    g_assert_cmpint (errno, ==, EIO);

    g_object_unref (read_io);
    htxf_io_release (&enc_xfer);
    htxf_io_release (&dec_xfer);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/htxf_io/plain_round_trip",
                     test_plain_round_trip);
    g_test_add_func ("/htxf_io/plain_eof", test_plain_eof);
    g_test_add_func ("/htxf_io/aead_round_trip_empty",
                     test_aead_round_trip_empty);
    g_test_add_func ("/htxf_io/aead_round_trip_small",
                     test_aead_round_trip_small);
    g_test_add_func ("/htxf_io/aead_round_trip_medium",
                     test_aead_round_trip_medium);
    g_test_add_func ("/htxf_io/aead_multi_frame_sequence",
                     test_aead_multi_frame_sequence);
    g_test_add_func ("/htxf_io/aead_oversized_write_rejected",
                     test_aead_oversized_write_rejected);
    g_test_add_func ("/htxf_io/aead_max_write_accepted",
                     test_aead_max_write_accepted);
    g_test_add_func ("/htxf_io/aead_truncated_frame_surfaces_eio",
                     test_aead_truncated_frame_surfaces_eio);
    g_test_add_func ("/htxf_io/aead_tag_tamper_surfaces_eio",
                     test_aead_tag_tamper_surfaces_eio);
    g_test_add_func ("/htxf_io/null_io_read_returns_einval",
                     test_null_io_read_returns_einval);
    g_test_add_func ("/htxf_io/null_io_write_returns_einval",
                     test_null_io_write_returns_einval);

    return g_test_run ();
}
