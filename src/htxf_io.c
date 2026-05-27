/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#include "config.h"
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <gio/gio.h>
#include <glib.h>
#include "compat.h" /* PACKED — required before hotline.h / protocol.h */
#include "hotline.h"
#include "protocol.h"
#include "cipher.h"
#include "cipher_aead.h"
#include "htxf_io.h"
#include "debug.h"

void
htxf_io_init (struct htxf_conn *htxf)
{
    if (!htxf) {
        return;
    }
    /* Caller usually memsets the parent struct already; this is
	 * idempotent and just makes the intent visible in xfers.c's
	 * xfer_new for grep-ability. */
    memset (&htxf->aead_io, 0, sizeof (htxf->aead_io));
}

void
htxf_io_release (struct htxf_conn *htxf)
{
    if (!htxf) {
        return;
    }
    g_free (htxf->aead_io.plain_buf);
    g_free (htxf->aead_io.cipher_buf);
    memset (&htxf->aead_io, 0, sizeof (htxf->aead_io));
}


/* Hex-dump up to 32 bytes through debug_log. Cheap; only fires when
 * GTKHX_DEBUG includes xfer-aead. Used to bisect AEAD-HTXF interop
 * bugs by showing the raw bytes on the wire alongside the framing
 * decisions we make over them. */
static void
hexdump_for_debug (const char *label, const uint8_t *p, gsize n, guint32 ref)
{
    gsize dump = n < 32 ? n : 32;
    gchar hex[3 * 32 + 4];
    gchar *q = hex;
    for (gsize i = 0; i < dump; i++) {
        q += g_snprintf (q, hex + sizeof (hex) - q, "%02x ", p[i]);
    }
    debug_log ("xfer-aead", "ref=%u %s len=%zu first %zu: %s",
               ref, label, n, dump, hex);
}

/* AEAD read path: serve from the plaintext accumulator first;
 * refill from the input stream as needed. Returns bytes copied,
 * or 0 / -1 for EOF / error matching read() semantics.
 *
 * Refill strategy: drain whatever the stream has into the
 * ciphertext accumulator one g_input_stream_read at a time, peek
 * the length prefix, and Open the frame when fully buffered. The
 * plaintext accumulator is grown to fit the frame's payload. The
 * worker thread blocks on g_input_stream_read (GSocketConnection
 * is blocking by default), so a single accumulator-empty refill
 * loop is enough — we don't have to interleave reads from
 * multiple streams.
 *
 * Returns 0 to mean clean EOF (matches read()), -1 with errno =
 * EIO for I/O error or Open failure. EINTR no longer surfaces
 * here — GIO handles partial-interrupt recovery internally. The
 * MAC-fail / oversized-frame cases also set errno = EIO so the
 * caller's existing `(r < 1)` check fires and the worker tears
 * down the transfer cleanly. */
static ssize_t
aead_read (struct htxf_conn *htxf, GInputStream *in, void *buf, size_t len)
{
    struct htxf_aead_io *io = &htxf->aead_io;

    /* If we have buffered plaintext from a previous Open, serve
	 * from it first. */
    if (io->plain_pos < io->plain_len) {
        gsize avail = io->plain_len - io->plain_pos;
        gsize give = (avail < len) ? avail : len;
        memcpy (buf, io->plain_buf + io->plain_pos, give);
        io->plain_pos += give;
        if (io->plain_pos == io->plain_len) {
            /* Drained — reset positions so the next refill starts
			 * from offset 0 (we keep the buffer allocated). */
            io->plain_pos = 0;
            io->plain_len = 0;
        }
        return (ssize_t) give;
    }

    /* Need to pull and Open another frame. Loop reading from the
	 * input stream until we have at least the 4-byte length
	 * prefix, then keep reading until we have prefix + ciphertext
	 * + tag. cipher_aead_peek_frame_size returns 0 if we're still
	 * below the prefix or the prefix is malformed. */
    for (;;) {
        gsize need = cipher_aead_peek_frame_size (io->cipher_buf,
                                                  io->cipher_len);
        if (need > 0 && io->cipher_len >= need) {
            break; /* full frame buffered */
        }

        /* Read more bytes from the stream. Grow cipher_buf to
		 * fit either the prefix's announced size or a small
		 * default chunk (so the first read can land the prefix
		 * even before we know the frame length). */
        gsize want = need ? (need - io->cipher_len) : 4096;
        gsize new_cap = io->cipher_len + want;
        if (io->cipher_cap < new_cap) {
            io->cipher_buf = g_realloc (io->cipher_buf, new_cap);
            io->cipher_cap = new_cap;
        }
        GError *err = NULL;
        gssize r = g_input_stream_read (in, io->cipher_buf + io->cipher_len,
                                        want, NULL, &err);
        if (r == 0) {
            /* Clean EOF mid-frame — surface as EOF only if we
			 * had nothing buffered yet. Otherwise the peer
			 * truncated a frame; surface as I/O error. */
            debug_log ("xfer-aead",
                       "ref=%u stream EOF: cipher_len=%zu need=%zu",
                       htxf->ref, io->cipher_len, need);
            if (io->cipher_len == 0) {
                return 0;
            }
            errno = EIO;
            return -1;
        }
        if (r < 0) {
            debug_log ("xfer-aead", "ref=%u read failed: %s",
                       htxf->ref, err ? err->message : "(no error info)");
            g_clear_error (&err);
            errno = EIO;
            return -1;
        }
        io->cipher_len += (gsize) r;
        hexdump_for_debug ("read", io->cipher_buf + io->cipher_len - (gsize) r,
                           (gsize) r, htxf->ref);
    }
    debug_log ("xfer-aead",
               "ref=%u frame ready: total=%zu cipher_len=%zu decode_counter=%"
               G_GUINT64_FORMAT,
               htxf->ref,
               cipher_aead_peek_frame_size (io->cipher_buf, io->cipher_len),
               io->cipher_len, htxf->xfer_decode.counter);

    /* We have a full frame at the head of io->cipher_buf. Open it
	 * into a fresh slot in io->plain_buf. */
    gsize frame_total
        = cipher_aead_peek_frame_size (io->cipher_buf, io->cipher_len);
    /* Payload (plaintext) size = framed - 4 (prefix) - 16 (tag) */
    if (frame_total
        < (gsize) (CIPHER_AEAD_LENGTH_PREFIX + CIPHER_AEAD_TAG_SIZE)) {
        errno = EIO;
        return -1;
    }
    gsize pt_cap = frame_total - CIPHER_AEAD_LENGTH_PREFIX
                   - CIPHER_AEAD_TAG_SIZE;
    if (io->plain_cap < pt_cap) {
        io->plain_buf = g_realloc (io->plain_buf, pt_cap);
        io->plain_cap = pt_cap;
    }
    gsize pt_len = cipher_aead_open (&htxf->xfer_decode, io->cipher_buf,
                                     frame_total, io->plain_buf, pt_cap);
    if (pt_len == 0) {
        debug_log ("xfer-aead", "ref=%u open failed (frame=%zu pt_cap=%zu)",
                   htxf->ref, frame_total, pt_cap);
        errno = EIO;
        return -1;
    }
    /* Shift the consumed bytes off the front of the ciphertext
	 * buffer. cipher_buf is small (one frame at a time) so memmove
	 * is fine — the alternative is a ring with two head/tail
	 * positions, more code for no measurable gain. */
    if (io->cipher_len > frame_total) {
        memmove (io->cipher_buf, io->cipher_buf + frame_total,
                 io->cipher_len - frame_total);
    }
    io->cipher_len -= frame_total;
    io->plain_len = pt_len;
    io->plain_pos = 0;

    /* Serve out of the fresh plaintext. */
    gsize give = (pt_len < len) ? pt_len : len;
    memcpy (buf, io->plain_buf, give);
    io->plain_pos = give;
    if (io->plain_pos == io->plain_len) {
        io->plain_pos = 0;
        io->plain_len = 0;
    }
    return (ssize_t) give;
}

/* AEAD write path: Seal the buffer as exactly one frame, then
 * push the framed bytes through the output stream with
 * g_output_stream_write_all. Each htxf_io_write call produces
 * one frame on the wire — large transfer payloads already loop
 * with a fixed buffer size, so the call rate stays sane and the
 * per-frame overhead (20 bytes: 4 length + 16 tag) is amortised.
 * Returns `len` on success (matches read()/write()'s "we accepted
 * all your bytes" convention), -1 with errno = EIO / EMSGSIZE on
 * Seal / stream failure. */
static ssize_t
aead_write (struct htxf_conn *htxf, GOutputStream *out, const void *buf,
            size_t len)
{
    if (len == 0) {
        return 0;
    }
    /* Stack-allocate small frames; heap-allocate the rest. The
	 * AEAD spec caps a single frame's BODY (ciphertext + tag) at
	 * 16 MiB — the 4-byte length prefix sits in front of the cap,
	 * NOT inside it. So the maximum acceptable plaintext is
	 * (MAX - TAG); cipher_aead_seal enforces the same boundary,
	 * and matching the check here keeps htxf_io_write and seal in
	 * lockstep (no 4-byte gap where the wrapper rejects bytes the
	 * primitive would accept). Oversized writes are still an
	 * upstream contract bug — the file-transfer worker reads in
	 * 8 KiB chunks — this is just defensive. */
    if (len > CIPHER_AEAD_MAX_FRAME_SIZE - CIPHER_AEAD_TAG_SIZE) {
        debug_log ("xfer-aead", "ref=%u oversized write (%zu bytes)",
                   htxf->ref, len);
        errno = EMSGSIZE;
        return -1;
    }
    gsize framed_cap
        = CIPHER_AEAD_LENGTH_PREFIX + len + CIPHER_AEAD_TAG_SIZE;
    /* 16 KiB stack threshold — covers the common per-call sizes
	 * (transfer headers, fork records, the 8 KiB bulk-data loop)
	 * without overflowing the worker thread's stack. g_autofree
	 * on heap_buf releases on every return path so we don't
	 * mirror an `if (heap) g_free` into each error site. */
    guint8 stack_buf[16 * 1024];
    g_autofree guint8 *heap_buf = NULL;
    guint8 *frame;
    if (framed_cap > sizeof (stack_buf)) {
        heap_buf = g_malloc (framed_cap);
        frame = heap_buf;
    } else {
        frame = stack_buf;
    }
    /* Snapshot the encode counter BEFORE seal advances it — useful
     * for matching this frame to a corresponding server-side decode
     * failure in protocol traces. */
    guint64 enc_counter_before = htxf->xfer_encode.counter;
    gsize n = cipher_aead_seal (&htxf->xfer_encode, buf, len, frame, framed_cap);
    if (n == 0) {
        debug_log ("xfer-aead", "ref=%u seal failed (len=%zu)",
                   htxf->ref, len);
        errno = EIO;
        return -1;
    }
    debug_log ("xfer-aead",
               "ref=%u seal: pt_len=%zu framed=%zu counter=%" G_GUINT64_FORMAT
               " key[0..3]=%02x%02x%02x%02x dir=%u",
               htxf->ref, len, n, enc_counter_before,
               htxf->xfer_encode.key[0], htxf->xfer_encode.key[1],
               htxf->xfer_encode.key[2], htxf->xfer_encode.key[3],
               (unsigned) htxf->xfer_encode.dir);
    hexdump_for_debug ("write", frame, n, htxf->ref);
    /* g_output_stream_write_all loops over partial writes
	 * internally — short writes can't surface, EINTR is handled
	 * inside GIO. Either the whole frame lands or we get an
	 * error. */
    GError *err = NULL;
    if (!g_output_stream_write_all (out, frame, n, NULL, NULL, &err)) {
        debug_log ("xfer-aead", "ref=%u write failed: %s",
                   htxf->ref, err ? err->message : "(no error info)");
        g_clear_error (&err);
        errno = EIO;
        return -1;
    }
    /* Pretend we accepted the plaintext bytes — the framing
	 * overhead is invisible to xfers.c, just as compression
	 * overhead is invisible to rcv.c on the control channel. */
    return (ssize_t) len;
}


ssize_t
htxf_io_read (struct htxf_conn *htxf, GIOStream *io, void *buf, size_t len)
{
    /* Reject NULL io explicitly — keeps the contract close to
     * read(2)/write(2): a bad descriptor surfaces as -1 with errno,
     * not a segfault. Callers (xfers.c, banner.c) shouldn't pass
     * NULL today, but a future TLS-handshake-failed path could
     * easily leave a stream slot empty. */
    if (!io) {
        errno = EINVAL;
        return -1;
    }
    GInputStream *in = g_io_stream_get_input_stream (io);
    if (htxf && htxf->aead_active) {
        return aead_read (htxf, in, buf, len);
    }
    /* Plaintext: read directly off the stream. GIO blocks until
	 * at least one byte, EOF, or error — same shape as read(2). */
    GError *err = NULL;
    gssize r = g_input_stream_read (in, buf, len, NULL, &err);
    if (r < 0) {
        debug_log ("xfer", "ref=%u plain read failed: %s",
                   htxf ? htxf->ref : 0,
                   err ? err->message : "(no error info)");
        g_clear_error (&err);
        errno = EIO;
        return -1;
    }
    return (ssize_t) r;
}

ssize_t
htxf_io_write (struct htxf_conn *htxf, GIOStream *io, const void *buf,
               size_t len)
{
    /* Reject NULL io explicitly — same rationale as htxf_io_read. */
    if (!io) {
        errno = EINVAL;
        return -1;
    }
    GOutputStream *out = g_io_stream_get_output_stream (io);
    if (htxf && htxf->aead_active) {
        return aead_write (htxf, out, buf, len);
    }
    /* Plaintext: write_all loops over partial writes internally,
	 * so the caller's `if (htxf_io_write (...) != n)` check
	 * either succeeds with `len` returned or fails with -1. */
    GError *err = NULL;
    if (!g_output_stream_write_all (out, buf, len, NULL, NULL, &err)) {
        debug_log ("xfer", "ref=%u plain write failed: %s",
                   htxf ? htxf->ref : 0,
                   err ? err->message : "(no error info)");
        g_clear_error (&err);
        errno = EIO;
        return -1;
    }
    return (ssize_t) len;
}
