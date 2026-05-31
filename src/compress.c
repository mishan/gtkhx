/*
 * compress.c — HOPE transport compression dispatcher.
 *
 * All compression/decompression is now implemented in Rust
 * (rust/crates/hxcompress/). This file is a thin C dispatcher
 * that bridges the htlc_conn fields to the Rust FFI.
 */

#include "config.h"
#include <sys/time.h>
#include <time.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <glib.h>
#include <gtk/gtk.h>
#include "hx.h"
#include "compress.h"


/* ---- Rust FFI declarations (hxcompress) ---- */

typedef struct CompressEncoder CompressEncoder;
typedef struct CompressDecoder CompressDecoder;

extern CompressEncoder *gtkhx_compress_encoder_new (uint16_t algo);
extern void gtkhx_compress_encoder_free (CompressEncoder *enc);
extern uint32_t gtkhx_compress_encode (CompressEncoder *enc,
                                       const uint8_t *input, uint32_t input_len,
                                       uint8_t *out, uint32_t out_cap);

extern CompressDecoder *gtkhx_compress_decoder_new (uint16_t algo);
extern void gtkhx_compress_decoder_free (CompressDecoder *dec);
extern uint32_t gtkhx_compress_decode (CompressDecoder *dec,
                                       const uint8_t *input, uint32_t input_len,
                                       uint8_t *out, uint32_t out_cap,
                                       uint32_t *in_used);

extern uint16_t gtkhx_compress_id_from_name (const char *name);


u_int16_t
compress_id_from_name (const char *name)
{
    return gtkhx_compress_id_from_name (name);
}

/* ---------- Decode dispatcher ---------- */

guint32
compress_decode (struct htlc_conn *htlc, struct qbuf *out, struct qbuf *in,
                 guint32 max, guint32 *inusedp)
{
    CompressDecoder *dec = (CompressDecoder *)htlc->compress_decode_state.ctx;
    if (!dec) {
        *inusedp = 0;
        return 0;
    }

    qbuf_set (out, out->pos, max);

    uint32_t in_used = 0;
    uint32_t produced = gtkhx_compress_decode (
        dec,
        &in->buf[in->pos], in->len,
        &out->buf[out->pos], max,
        &in_used);

    if (htlc->compress_decode_type == COMPRESS_GZIP) {
        htlc->gzip_inflate_total_in += in_used;
        htlc->gzip_inflate_total_out += produced;
    }

    *inusedp = in_used;
    return produced;
}

/* ---------- Encode dispatcher ---------- */

#define COMPRESS_OUT_BUFSIZE(len) ((len) * 2 + 64)

guint32
compress_encode (struct htlc_conn *htlc, guint32 pos, guint32 len)
{
    CompressEncoder *enc = (CompressEncoder *)htlc->compress_encode_state.ctx;
    if (!enc) {
        return 0;
    }

    guint32 out_cap = COMPRESS_OUT_BUFSIZE (len);
    guint8 *buf = g_malloc (out_cap);

    guint32 dstlen = gtkhx_compress_encode (
        enc,
        &htlc->out.buf[pos], len,
        buf, out_cap);

    if (dstlen == 0) {
        g_free (buf);
        return 0;
    }

    htlc->out.len -= len;
    qbuf_set (&htlc->out, htlc->out.pos, htlc->out.len + dstlen);
    memcpy (&htlc->out.buf[pos], buf, dstlen);

    if (htlc->compress_encode_type == COMPRESS_GZIP) {
        htlc->gzip_deflate_total_in += len;
        htlc->gzip_deflate_total_out += dstlen;
    }

    g_free (buf);
    return dstlen;
}

/* ---------- Init / End ---------- */

void
compress_encode_init (struct htlc_conn *htlc)
{
    htlc->compress_encode_state.ctx =
        gtkhx_compress_encoder_new (htlc->compress_encode_type);
}

void
compress_decode_init (struct htlc_conn *htlc)
{
    htlc->compress_decode_state.ctx =
        gtkhx_compress_decoder_new (htlc->compress_decode_type);
}

void
compress_encode_end (struct htlc_conn *htlc)
{
    if (htlc->compress_encode_state.ctx) {
        gtkhx_compress_encoder_free (
            (CompressEncoder *)htlc->compress_encode_state.ctx);
        htlc->compress_encode_state.ctx = NULL;
    }
}

void
compress_decode_end (struct htlc_conn *htlc)
{
    if (htlc->compress_decode_state.ctx) {
        gtkhx_compress_decoder_free (
            (CompressDecoder *)htlc->compress_decode_state.ctx);
        htlc->compress_decode_state.ctx = NULL;
    }
}
