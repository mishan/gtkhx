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
#include <unistd.h>
#include <sys/types.h>
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


/* AEAD read path: serve from the plaintext accumulator first;
 * refill from socket as needed. Returns bytes copied, or 0/-1
 * for EOF / error matching read() semantics.
 *
 * Refill strategy: drain whatever the socket has into the
 * ciphertext accumulator one read() at a time, peek the length
 * prefix, and Open the frame when fully buffered. The plaintext
 * accumulator is grown to fit the frame's payload. The worker
 * thread blocks on read() (the htxf socket is in blocking mode
 * post-htxf_connect), so a single accumulator-empty refill
 * loop is enough — we don't have to interleave reads from
 * multiple sockets.
 *
 * Returns 0 to mean clean EOF (matches read()), -1 (errno set)
 * for I/O error or Open failure. The MAC-fail / oversized-frame
 * cases set errno = EIO so the caller's existing `(r < 1)`
 * check fires and the worker tears down the transfer cleanly. */
static ssize_t
aead_read (struct htxf_conn *htxf, int fd, void *buf, size_t len)
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

    /* Need to pull and Open another frame. Loop reading from
	 * socket until we have at least the 4-byte length prefix,
	 * then keep reading until we have prefix + ciphertext +
	 * tag. cipher_aead_peek_frame_size returns 0 if we're
	 * still below the prefix or the prefix is malformed. */
    for (;;) {
        gsize need = cipher_aead_peek_frame_size (io->cipher_buf,
                                                  io->cipher_len);
        if (need > 0 && io->cipher_len >= need) {
            break; /* full frame buffered */
        }

        /* Read more bytes from the socket. Grow cipher_buf to
		 * fit either the prefix's announced size or a small
		 * default chunk (so the first read can land the prefix
		 * even before we know the frame length). */
        gsize want = need ? (need - io->cipher_len) : 4096;
        gsize new_cap = io->cipher_len + want;
        if (io->cipher_cap < new_cap) {
            io->cipher_buf = g_realloc (io->cipher_buf, new_cap);
            io->cipher_cap = new_cap;
        }
        ssize_t r = read (fd, io->cipher_buf + io->cipher_len, want);
        if (r == 0) {
            /* Clean EOF mid-frame — surface as EOF only if we
			 * had nothing buffered yet. Otherwise the peer
			 * truncated a frame; surface as I/O error. */
            if (io->cipher_len == 0) {
                return 0;
            }
            errno = EIO;
            return -1;
        }
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        io->cipher_len += (gsize) r;
    }

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
 * write the framed bytes atomically. Each htxf_io_write call
 * produces one frame on the wire — large transfer payloads
 * already loop with a fixed buffer size, so the call rate stays
 * sane and the per-frame overhead (20 bytes: 4 length + 16 tag)
 * is amortised. Returns `len` (matching the read()/write()
 * convention "we accepted all your bytes") on success, -1 on
 * Seal / socket failure with errno set. */
static ssize_t
aead_write (struct htxf_conn *htxf, int fd, const void *buf, size_t len)
{
    if (len == 0) {
        return 0;
    }
    /* Stack-allocate small frames; heap-allocate the rest. The
	 * AEAD spec caps a single frame at 16 MiB minus the 20-byte
	 * envelope; oversized writes are an upstream contract bug
	 * (the file-transfer worker reads in 8 KiB chunks today). */
    if (len > CIPHER_AEAD_MAX_FRAME_SIZE
                  - (CIPHER_AEAD_LENGTH_PREFIX + CIPHER_AEAD_TAG_SIZE)) {
        debug_log ("xfer-aead", "ref=%u oversized write (%zu bytes)",
                   htxf->ref, len);
        errno = EMSGSIZE;
        return -1;
    }
    gsize framed_cap
        = CIPHER_AEAD_LENGTH_PREFIX + len + CIPHER_AEAD_TAG_SIZE;
    /* 16 KiB stack threshold — covers the common per-call sizes
	 * (transfer headers, fork records, the 8 KiB bulk-data loop)
	 * without overflowing the worker thread's stack. */
    guint8 stack_buf[16 * 1024];
    guint8 *out;
    gboolean heap = framed_cap > sizeof (stack_buf);
    if (heap) {
        out = g_malloc (framed_cap);
    } else {
        out = stack_buf;
    }
    gsize n = cipher_aead_seal (&htxf->xfer_encode, buf, len, out, framed_cap);
    if (n == 0) {
        if (heap)
            g_free (out);
        debug_log ("xfer-aead", "ref=%u seal failed (len=%zu)",
                   htxf->ref, len);
        errno = EIO;
        return -1;
    }
    /* Atomic-on-success write loop. */
    gsize wrote = 0;
    while (wrote < n) {
        ssize_t w = write (fd, out + wrote, n - wrote);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (heap)
                g_free (out);
            return -1;
        }
        if (w == 0) {
            if (heap)
                g_free (out);
            errno = EIO;
            return -1;
        }
        wrote += (gsize) w;
    }
    if (heap) {
        g_free (out);
    }
    /* Pretend we accepted the plaintext bytes — the framing
	 * overhead is invisible to xfers.c, just as compression
	 * overhead is invisible to rcv.c on the control channel. */
    return (ssize_t) len;
}


ssize_t
htxf_io_read (struct htxf_conn *htxf, int fd, void *buf, size_t len)
{
    if (htxf && htxf->aead_active) {
        return aead_read (htxf, fd, buf, len);
    }
    return read (fd, buf, len);
}

ssize_t
htxf_io_write (struct htxf_conn *htxf, int fd, const void *buf, size_t len)
{
    if (htxf && htxf->aead_active) {
        return aead_write (htxf, fd, buf, len);
    }
    return write (fd, buf, len);
}
