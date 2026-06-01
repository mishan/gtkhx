/*
 * compress.c — HOPE transport compression dispatcher.
 *
 * After Phase R1 this file is a thin C wrapper over the Rust
 * hxcompress crate. The wire-side responsibilities that stay on
 * the C side:
 *
 *   - per-htlc_conn lifecycle (alloc on init, free on end)
 *   - resizing htlc->out around an encoded payload
 *   - the gzip total_in / total_out counters that network.c's
 *     close-time stats log reads
 *   - mapping numeric COMPRESS_* IDs through to algorithm names
 *
 * Streaming compress/decompress, the per-algo flush semantics,
 * and the codec context layouts all live in rust/crates/hxcompress
 * now.
 */

#include "config.h"
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <glib.h>
#include <gtk/gtk.h>
#include "hx.h"
#include "compress.h"


/* ---- Rust FFI declarations (hxcompress) ----
 *
 * Opaque codec handles. The Rust enum carries the algorithm tag
 * internally, so a single pointer type covers GZIP / LZ4 / ZSTD on
 * each side. The C-side union compress_state stores void * and we
 * just cast to the appropriate type at the FFI boundary.
 */

typedef struct CompressEncoder CompressEncoder;
typedef struct CompressDecoder CompressDecoder;

extern CompressEncoder *gtkhx_compress_encoder_new (uint16_t algo);
extern void gtkhx_compress_encoder_free (CompressEncoder *enc);
extern uint32_t gtkhx_compress_encode (CompressEncoder *enc,
                                       const uint8_t *input,
                                       uint32_t input_len,
                                       uint8_t *out, uint32_t out_cap);

extern CompressDecoder *gtkhx_compress_decoder_new (uint16_t algo);
extern void gtkhx_compress_decoder_free (CompressDecoder *dec);
extern uint32_t gtkhx_compress_decode (CompressDecoder *dec,
                                       const uint8_t *input,
                                       uint32_t input_len,
                                       uint8_t *out, uint32_t out_cap,
                                       uint32_t *in_used);

/* Forward-declared rather than #include'd from network.h — same
 * "keep compress.c free of GTK / hx.h transitives" pattern cipher.c
 * uses. Only used by the fail-closed teardown path in
 * compress_encode below. */
extern void hx_htlc_close (struct htlc_conn *htlc, int expected);


u_int16_t
compress_id_from_name (const char *name)
{
    if (!name || !*name) {
        return COMPRESS_NONE;
    }
    if (!strcmp (name, "NONE")) {
        return COMPRESS_NONE;
    }
    if (!strcmp (name, "GZIP")) {
        return COMPRESS_GZIP;
    }
    if (!strcmp (name, "LZ4")) {
        return COMPRESS_LZ4;
    }
    if (!strcmp (name, "ZSTD")) {
        return COMPRESS_ZSTD;
    }
    return COMPRESS_NONE;
}

/* compress_encode_bufsize is `static inline` in compress.h so the
 * Tier 1 test (tests/unit/test_compress_bufsize.c) can call it
 * directly without linking this TU. Production callers below pick
 * up the inlined body from the header include above. */

guint32
compress_decode (struct htlc_conn *htlc, struct qbuf *out, struct qbuf *in,
                 guint32 max, guint32 *inusedp)
{
    CompressDecoder *dec = (CompressDecoder *) htlc->compress_decode_state.ctx;
    uint32_t in_used = 0;
    uint32_t produced;

    if (!dec) {
        *inusedp = 0;
        return 0;
    }

    qbuf_set (out, out->pos, max);
    produced = gtkhx_compress_decode (dec,
                                      &in->buf[in->pos], in->len,
                                      &out->buf[out->pos], max,
                                      &in_used);
    *inusedp = in_used;

    /* Maintain the legacy gzip-direction counters for network.c's
     * "GZIP inflate: in: ... out: ..." close-time stats line. The
     * counter names are gzip-specific because that's the only algo
     * the pre-port code tracked; LZ4 / ZSTD never had stats and
     * still don't. */
    if (htlc->compress_decode_type == COMPRESS_GZIP) {
        htlc->gzip_inflate_total_in  += in_used;
        htlc->gzip_inflate_total_out += produced;
    }

    return produced;
}

guint32
compress_encode (struct htlc_conn *htlc, guint32 pos, guint32 len)
{
    CompressEncoder *enc = (CompressEncoder *) htlc->compress_encode_state.ctx;
    guint32 cap;
    uint8_t *buf;
    uint32_t produced;

    /* Any encode-side failure is fail-closed: the caller in
     * network.c (hlwrite / hlwrite_chunks) calls cipher_encode right
     * after this with the returned length. If we returned 0 without
     * teardown, cipher_encode would encrypt 0 bytes and the socket-
     * write loop would flush the (un-encrypted, un-compressed)
     * plaintext sitting at htlc->out[pos..pos+len] on a connection
     * the user believes is HOPE-secured. Tear the connection down
     * here so the in-memory plaintext never reaches the wire; the
     * caller checks htlc->fd after compress_encode and skips the
     * downstream cipher_encode + socket flush. */
    if (!enc) {
        hx_htlc_close (htlc, 0);
        return 0;
    }

    cap = compress_encode_bufsize (len);
    if (cap == 0) {
        /* len was big enough that 2*len + 1024 overflowed u32. No
         * sane Hotline transaction is anywhere near 2 GiB; surface
         * as a compression failure rather than allocate a wrapped-
         * around (undersized) buffer — and tear down for the same
         * fail-open reason as above. */
        hx_htlc_close (htlc, 0);
        return 0;
    }
    buf = g_malloc (cap);
    produced = gtkhx_compress_encode (enc, &htlc->out.buf[pos], len, buf, cap);
    if (produced == 0) {
        g_free (buf);
        hx_htlc_close (htlc, 0);
        return 0;
    }

    /* Splice the encoded bytes back into htlc->out at the same
     * position the plaintext occupied. Same shape as the pre-port
     * code: shrink len off out.len, qbuf_set to grow the buffer to
     * its new (smaller-or-bigger) size, memcpy the encoded payload
     * into place. */
    htlc->out.len -= len;
    qbuf_set (&htlc->out, htlc->out.pos, htlc->out.len + produced);
    memcpy (&htlc->out.buf[pos], buf, produced);
    g_free (buf);

    if (htlc->compress_encode_type == COMPRESS_GZIP) {
        htlc->gzip_deflate_total_in  += len;
        htlc->gzip_deflate_total_out += produced;
    }

    return produced;
}

void
compress_encode_init (struct htlc_conn *htlc)
{
    htlc->compress_encode_state.ctx
        = gtkhx_compress_encoder_new ((uint16_t) htlc->compress_encode_type);
}

void
compress_decode_init (struct htlc_conn *htlc)
{
    htlc->compress_decode_state.ctx
        = gtkhx_compress_decoder_new ((uint16_t) htlc->compress_decode_type);
}

void
compress_encode_end (struct htlc_conn *htlc)
{
    if (htlc->compress_encode_state.ctx) {
        gtkhx_compress_encoder_free (
            (CompressEncoder *) htlc->compress_encode_state.ctx);
        htlc->compress_encode_state.ctx = NULL;
    }
}

void
compress_decode_end (struct htlc_conn *htlc)
{
    if (htlc->compress_decode_state.ctx) {
        gtkhx_compress_decoder_free (
            (CompressDecoder *) htlc->compress_decode_state.ctx);
        htlc->compress_decode_state.ctx = NULL;
    }
}
