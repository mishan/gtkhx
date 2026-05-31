/*
 * Copyright (C) 2000-2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * src/network_decode.c — receive-side decoder (was static in
 * network.c). Carved out so tests/proto/test_network_decode.c can
 * drive the cipher / compress / AEAD branches against canned bytes
 * without dragging in network.c's async-connect + tracker-fetch
 * pile. See network_decode.h for the contract.
 *
 * Behaviour is byte-for-byte unchanged from the previous in-line
 * versions. The two functions still call hx_htlc_close and
 * hx_printf_prefix on the error path; production resolves those
 * to network.c / gtkhx_log.c, the test binary resolves them to
 * test-local stubs.
 */

#include "config.h"

#include <string.h>
#include <glib.h>

#include "protocol.h"
#include "cipher.h"
#include "cipher_aead.h"
#include "compress.h"
#include "debug.h"
#include "network.h"      /* hx_htlc_close forward-decl */
#include "gtkhx_log.h"    /* hx_printf_prefix + INFOPREFIX forward-decls */
#include "network_decode.h"

/* Rust FFI (hxcrypto-stream) — clone/free for Blowfish cipher state rollback. */
extern BlowfishOfb64State *gtkhx_blowfish_ofb64_clone (const BlowfishOfb64State *state);
extern void gtkhx_blowfish_ofb64_free (BlowfishOfb64State *state);

u_int32_t
hx_aead_pump_frames (struct htlc_conn *htlc)
{
    struct qbuf *src = &htlc->read_in;
    struct qbuf *dst = &htlc->aead_plain;

    while (src->len >= CIPHER_AEAD_LENGTH_PREFIX) {
        size_t frame_size = cipher_aead_peek_frame_size (
            &src->buf[src->pos], src->len);
        if (frame_size == 0) {
            /* Oversized or malformed length prefix — non-recoverable.
             * The server's framing is corrupted (or, more likely, we
             * lost cipher sync / disagree on frame layout). Tear
             * down. Log the first up-to-16 bytes the wire actually
             * delivered, so a layout mismatch (LE vs BE prefix,
             * prefix elsewhere in the frame, no prefix at all,
             * ...) is visible to GTKHX_DEBUG=xfer-aead. */
            {
                gsize dump = src->len < 16 ? src->len : 16;
                gchar hex[64];
                gchar *p = hex;
                for (gsize i = 0; i < dump && p + 3 < hex + sizeof (hex);
                     i++) {
                    p += g_snprintf (p, hex + sizeof (hex) - p, "%02x ",
                                     src->buf[src->pos + i]);
                }
                debug_log ("xfer-aead",
                           "frame-size-out-of-range: src->len=%u "
                           "first %zu bytes: %s",
                           src->len, dump, hex);
            }
            hx_printf_prefix (htlc, 0, INFOPREFIX,
                              "AEAD frame size out of range; "
                              "disconnecting\n");
            hx_htlc_close (htlc, 0);
            return 0;
        }
        if (src->len < frame_size) {
            /* Frame not fully buffered yet — wait for more bytes. */
            break;
        }

        /* Reserve aead_plain capacity for the frame's plaintext
         * (frame_size - prefix - tag). qbuf_set is idempotent on
         * existing buffer storage and only g_realloc's when we
         * need more. */
        size_t pt_max = frame_size - CIPHER_AEAD_LENGTH_PREFIX
                                   - CIPHER_AEAD_TAG_SIZE;
        guint32 dst_off = dst->pos + dst->len;
        qbuf_set (dst, dst->pos, dst->len + pt_max);

        size_t pt_len = cipher_aead_open (
            &htlc->cipher_decode_state.chacha,
            &src->buf[src->pos], frame_size,
            &dst->buf[dst_off], pt_max);
        if (pt_len == 0) {
            hx_printf_prefix (htlc, 0, INFOPREFIX,
                              "AEAD authentication failure; "
                              "disconnecting\n");
            /* Rewind the reservation we made above — no plaintext
             * was actually written. */
            qbuf_set (dst, dst->pos, dst->len - pt_max);
            hx_htlc_close (htlc, 0);
            return 0;
        }
        /* aead_plain.len is now (dst_off - dst->pos) + pt_len; correct
         * for the reservation vs. the actual pt_len. */
        dst->len -= pt_max;
        dst->len += pt_len;

        /* Consume the frame from read_in. */
        if (src->len > frame_size) {
            memmove (&src->buf[src->pos],
                     &src->buf[src->pos + frame_size],
                     src->len - frame_size);
        }
        src->len -= frame_size;
    }
    return dst->len;
}

unsigned int
hx_decode (struct htlc_conn *htlc)
{
    struct qbuf *in = &htlc->read_in;
    struct qbuf *out = in;
    u_int32_t len, max, inused, r = in->len;
    struct qbuf cipher_out;
    struct qbuf compress_out;

    memset (&cipher_out, 0, sizeof (struct qbuf));
    memset (&compress_out, 0, sizeof (struct qbuf));

    /* Phase 5+ (HOPE-ChaCha20-Poly1305): AEAD-framed path. Pump
     * complete frames from read_in into aead_plain, then bulk
     * memcpy from aead_plain into htlc->in based on how many
     * bytes the rcv parser is waiting for (htlc->in.len).
     * Compression is not used in AEAD mode (spec), so we don't
     * touch the compress branch here.
     *
     * This path completely bypasses the byte-stream cipher_decode
     * + compress_decode plumbing below — those still operate on
     * the legacy stream-cipher RC4/Blowfish path.
     *
     * IMPORTANT: this branch is checked BEFORE the `if (!r)` early
     * return below, because the AEAD path's plaintext accumulator
     * (htlc->aead_plain) can hold data even when read_in is
     * empty. The htlc_read while-loop drives multiple decode()
     * calls per Hotline transaction (one for the 22-byte header,
     * one for the body) — read_in is typically drained in full
     * during the first call's aead_pump, so iter 2 must be able
     * to serve from aead_plain alone. Bypassing the early return
     * here is what makes that drain-across-iterations work; an
     * earlier revision had the early return first and trans=2
     * replies hung indefinitely after their header iter. */
    if (htlc->cipher_mode == CIPHER_MODE_AEAD
        && htlc->cipher_decode_type == CIPHER_CHACHA20_POLY1305) {
        hx_aead_pump_frames (htlc);
        /* hx_htlc_close zeroes htlc->fd; bail if pump tore us down. */
        if (!htlc->fd) {
            return 0;
        }
        struct qbuf *plain = &htlc->aead_plain;
        if (plain->len == 0) {
            return 0;
        }
        u_int32_t want = htlc->in.len;
        u_int32_t avail = plain->len;
        u_int32_t take = want < avail ? want : avail;
        memcpy (&htlc->in.buf[htlc->in.pos],
                &plain->buf[plain->pos], take);
        htlc->in.pos += take;
        htlc->in.len -= take;
        if (take == plain->len) {
            plain->pos = 0;
            plain->len = 0;
        } else {
            memmove (&plain->buf[plain->pos],
                     &plain->buf[plain->pos + take],
                     plain->len - take);
            plain->len -= take;
        }
        return (htlc->in.len == 0);
    }

    /* Below here the legacy stream/plaintext path needs at least
     * one byte buffered in read_in to do anything; bail otherwise.
     * The AEAD branch above has its own data-availability check
     * against aead_plain. */
    if (!r) {
        return 0;
    }

    inused = 0;
    len = r;
    in->pos = 0;

    if (htlc->compressalg[0] && htlc->compress_decode_type != COMPRESS_NONE) {
        max = 0xffffffff;
    } else
        max = htlc->in.len;
    void *saved_bf_stream = NULL;
    union cipher_state saved_rc4_state;
    if (htlc->cipheralg[0] && htlc->cipher_decode_type != CIPHER_NONE) {
        /* Save cipher state before decode in case we need to roll back
         * (happens when compress consumes fewer bytes than the cipher
         * decoded). RC4 uses an inline Nettle arcfour_ctx (memcpy is
         * sufficient); Blowfish uses an opaque Rust-allocated state
         * that must be cloned. */
        if (htlc->cipher_decode_type == CIPHER_RC4) {
            memcpy (&saved_rc4_state, &htlc->cipher_decode_state,
                    sizeof (saved_rc4_state));
        } else if (htlc->cipher_decode_type == CIPHER_BLOWFISH) {
            saved_bf_stream = gtkhx_blowfish_ofb64_clone (
                (BlowfishOfb64State *)htlc->cipher_decode_state.stream);
        }
        out = &cipher_out;
        len = cipher_decode (htlc, out, in, max, &inused);
    } else
        if (htlc->compress_decode_type == COMPRESS_NONE)
    {
        max = htlc->in.len;
        out = in;
        if (r > max) {
            inused = max;
            len = max;
        } else {
            inused = r;
            len = r;
        }
    }
    if (htlc->compress_decode_type != COMPRESS_NONE) {
        max = htlc->in.len;
        out = &compress_out;
        len = compress_decode (
            htlc, out,
            htlc->cipher_decode_type == CIPHER_NONE ? in : &cipher_out,
            max, &inused);
    }
    memcpy (&htlc->in.buf[htlc->in.pos], &out->buf[out->pos], len);
    if (r != inused) {
        if (htlc->cipher_decode_type != CIPHER_NONE) {
            /* Roll back cipher state, then re-decode with the
             * correct byte count. */
            if (htlc->cipher_decode_type == CIPHER_RC4) {
                memcpy (&htlc->cipher_decode_state, &saved_rc4_state,
                        sizeof (saved_rc4_state));
            } else if (htlc->cipher_decode_type == CIPHER_BLOWFISH
                       && saved_bf_stream) {
                gtkhx_blowfish_ofb64_free (
                    (BlowfishOfb64State *)htlc->cipher_decode_state.stream);
                htlc->cipher_decode_state.stream = saved_bf_stream;
                saved_bf_stream = NULL;
            }
            cipher_decode (htlc, &cipher_out, in, inused, &inused);
        }
        memmove (&in->buf[0], &in->buf[inused], r - inused);
    }
    /* Free the saved Blowfish snapshot if we didn't need it for rollback. */
    if (saved_bf_stream) {
        gtkhx_blowfish_ofb64_free ((BlowfishOfb64State *)saved_bf_stream);
    }
    in->pos = r - inused;
    in->len -= inused;
    htlc->in.pos += len;
    htlc->in.len -= len;

    if (compress_out.buf) {
        g_free (compress_out.buf);
    }
    if (cipher_out.buf) {
        g_free (cipher_out.buf);
    }

    return (htlc->in.len == 0);
}
