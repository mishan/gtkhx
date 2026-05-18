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

#ifdef CONFIG_COMPRESS

#define COMPRESS_DEBUG 0

#if COMPRESS_DEBUG
#include <stdio.h>
#include <unistd.h>

static void
writestuff (const char *str, guint8 type, const guint8 *buf, unsigned int len)
{
    unsigned int i;
    char file[32];
    FILE *fp;

    g_snprintf (file, sizeof (file), "/tmp/compress.%d", getpid ());
    fp = fopen (file, "a");
    if (!fp) {
        return;
    }
    fprintf (fp, str);
    fprintf (fp, "%u: ", type);
    for (i = 0; i < len; i++) {
        fprintf (fp, "%2.2x", buf[i]);
    }
    fprintf (fp, "\n");
    fflush (fp);
    fclose (fp);
}
#endif

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
#ifdef HAVE_LZ4
    if (!strcmp (name, "LZ4")) {
        return COMPRESS_LZ4;
    }
#endif
#ifdef HAVE_ZSTD
    if (!strcmp (name, "ZSTD")) {
        return COMPRESS_ZSTD;
    }
#endif
    return COMPRESS_NONE;
}

/* ---------- GZIP (zlib) ----------
 *
 * The original HOPE compression. Preserved byte-for-byte from the
 * legacy GtkHx implementation — wire-compat with every hx-family
 * server depends on Z_SYNC_FLUSH boundaries and the deflate /
 * inflate context running persistently across all transactions in
 * a connection. */

static int gzip_level = Z_DEFAULT_COMPRESSION;
static int gzip_flush = Z_SYNC_FLUSH;

static guint32
gzip_decode (struct htlc_conn *htlc, struct qbuf *out, struct qbuf *in,
             guint32 max, guint32 *inusedp)
{
    z_stream *stream = &htlc->compress_decode_state.stream;
    unsigned long dstlen, pos;
    guint32 used;
    int err;

#if COMPRESS_DEBUG
    writestuff ("src: ", 0, &in->buf[in->pos], in->len);
#endif
    dstlen = max;
    pos = 0;
    qbuf_set (out, out->pos, max);
    stream->next_in = &in->buf[in->pos];
    stream->avail_in = in->len;

    for (;;) {
        stream->next_out = &out->buf[out->pos + pos];
        stream->avail_out = dstlen;
        err = inflate (stream, gzip_flush);
        pos += dstlen - stream->avail_out;
        dstlen -= dstlen - stream->avail_out;
        if (err != Z_OK) {
            break;
        }
    }
    dstlen = max - dstlen;
    used = in->len - stream->avail_in;
#if COMPRESS_DEBUG
    if (err != Z_OK) {
        hxd_log ("err=%d msg=%s", err, stream->msg);
    }
    writestuff ("dec: ", err, &out->buf[out->pos], dstlen);
    hxd_log ("in: %d out: %d", used, dstlen);
#endif
    htlc->gzip_inflate_total_in += used;
    htlc->gzip_inflate_total_out += dstlen;
    *inusedp = used;

    return dstlen;
}

#define ZLIB_MAX_INPUT 0x1000
#define ZLIB_OUT_BUFSIZE (ZLIB_MAX_INPUT + ZLIB_MAX_INPUT / 1000 + 13)

static guint32
gzip_encode (struct htlc_conn *htlc, guint32 pos, guint32 len)
{
    z_stream *stream = &htlc->compress_encode_state.stream;
    guint8 *buf, autobuf[ZLIB_OUT_BUFSIZE];
    guint32 dstlen, inpos, thislen;
    int err;

    buf = autobuf;
    inpos = 0;
    dstlen = 0;
    for (;;) {
        stream->next_in = &htlc->out.buf[pos + inpos];
        thislen = len - inpos;
        if (thislen > ZLIB_MAX_INPUT) {
            thislen = ZLIB_MAX_INPUT;
        }
        stream->avail_in = thislen;
        stream->next_out = &buf[dstlen];
        stream->avail_out = ZLIB_OUT_BUFSIZE;
        err = deflate (stream, gzip_flush);
        if (err == Z_OK) {
            dstlen += ZLIB_OUT_BUFSIZE - stream->avail_out;
            inpos += thislen - stream->avail_in;
            if (len == inpos) {
                break;
            }
            if (buf == autobuf) {
                buf = g_malloc (dstlen + ZLIB_OUT_BUFSIZE);
                memcpy (buf, autobuf, dstlen);
            } else {
                buf = g_realloc (buf, dstlen + ZLIB_OUT_BUFSIZE);
            }
        } else {
            break;
        }
    }
    htlc->out.len -= len;
    qbuf_set (&htlc->out, htlc->out.pos, htlc->out.len + dstlen);
    memcpy (&htlc->out.buf[pos], buf, dstlen);
    htlc->gzip_deflate_total_in += len;
    htlc->gzip_deflate_total_out += dstlen;
#if COMPRESS_DEBUG
    writestuff ("enc: ", err, &htlc->out.buf[pos], dstlen);
#endif
    if (buf != autobuf) {
        g_free (buf);
    }

    return dstlen;
}

#ifdef HAVE_LZ4

/* ---------- LZ4 (frame format) ----------
 *
 * Per HOPE-Secure-Login: "LZ4 frame (spec v1.6) — Fast compression
 * with low CPU overhead. Janus/Klein extension."
 *
 * LZ4F's context is persistent across multiple
 * compressBegin/compressUpdate/compressEnd cycles when each
 * transaction is treated as its own logical frame and flushed at
 * the end. compressBound returns a safe maximum output size for
 * a given input length. */

static guint32
lz4_decode (struct htlc_conn *htlc, struct qbuf *out, struct qbuf *in,
            guint32 max, guint32 *inusedp)
{
    LZ4F_dctx *dctx = htlc->compress_decode_state.lz4_dctx;
    size_t src_size = in->len;
    size_t dst_size = max;
    size_t r;

    qbuf_set (out, out->pos, max);
    r = LZ4F_decompress (dctx, &out->buf[out->pos], &dst_size,
                        &in->buf[in->pos], &src_size, NULL);
    if (LZ4F_isError (r)) {
#if COMPRESS_DEBUG
        hxd_log ("LZ4F_decompress: %s", LZ4F_getErrorName (r));
#endif
        *inusedp = 0;
        return 0;
    }
    *inusedp = (guint32)src_size;
    return (guint32)dst_size;
}

static guint32
lz4_encode (struct htlc_conn *htlc, guint32 pos, guint32 len)
{
    LZ4F_cctx *cctx = htlc->compress_encode_state.lz4_cctx;
    size_t bound;
    size_t hdr_written, body_written, end_written;
    guint8 *buf;
    LZ4F_preferences_t prefs;

    memset (&prefs, 0, sizeof prefs);
    bound = LZ4F_compressBound (len, &prefs);
    /* LZ4F_HEADER_SIZE_MAX is the upper bound on header bytes; */
    /* allocate enough to also cover compressBegin + compressEnd. */
    buf = g_malloc (bound + LZ4F_HEADER_SIZE_MAX + 32);

    hdr_written = LZ4F_compressBegin (cctx, buf,
                                      bound + LZ4F_HEADER_SIZE_MAX + 32, &prefs);
    if (LZ4F_isError (hdr_written)) {
        g_free (buf);
        return 0;
    }
    body_written = LZ4F_compressUpdate (
        cctx, buf + hdr_written, bound + LZ4F_HEADER_SIZE_MAX + 32 - hdr_written,
        &htlc->out.buf[pos], len, NULL);
    if (LZ4F_isError (body_written)) {
        g_free (buf);
        return 0;
    }
    end_written = LZ4F_compressEnd (
        cctx, buf + hdr_written + body_written,
        bound + LZ4F_HEADER_SIZE_MAX + 32 - hdr_written - body_written, NULL);
    if (LZ4F_isError (end_written)) {
        g_free (buf);
        return 0;
    }

    guint32 dstlen = (guint32) (hdr_written + body_written + end_written);
    htlc->out.len -= len;
    qbuf_set (&htlc->out, htlc->out.pos, htlc->out.len + dstlen);
    memcpy (&htlc->out.buf[pos], buf, dstlen);
    g_free (buf);
    return dstlen;
}

#endif /* HAVE_LZ4 */

#ifdef HAVE_ZSTD

/* ---------- ZSTD ----------
 *
 * Per HOPE-Secure-Login: "Zstandard (RFC 8878). Best compression
 * ratio. Preferred for modern AEAD clients. Janus/Klein extension."
 *
 * Both contexts are persistent. We use ZSTD_e_flush per transaction
 * so the decoder on the other side can drain a full output before
 * we hand back control. */

#define ZSTD_FALLBACK_OUT_BUFSIZE (64 * 1024)

static guint32
zstd_decode (struct htlc_conn *htlc, struct qbuf *out, struct qbuf *in,
             guint32 max, guint32 *inusedp)
{
    ZSTD_DCtx *dctx = htlc->compress_decode_state.zstd_dctx;
    ZSTD_inBuffer ib = { &in->buf[in->pos], in->len, 0 };
    ZSTD_outBuffer ob;
    size_t r;

    qbuf_set (out, out->pos, max);
    ob.dst = &out->buf[out->pos];
    ob.size = max;
    ob.pos = 0;

    r = ZSTD_decompressStream (dctx, &ob, &ib);
    if (ZSTD_isError (r)) {
#if COMPRESS_DEBUG
        hxd_log ("ZSTD_decompressStream: %s", ZSTD_getErrorName (r));
#endif
        *inusedp = 0;
        return 0;
    }
    *inusedp = (guint32)ib.pos;
    return (guint32)ob.pos;
}

static guint32
zstd_encode (struct htlc_conn *htlc, guint32 pos, guint32 len)
{
    ZSTD_CCtx *cctx = htlc->compress_encode_state.zstd_cctx;
    size_t bound = ZSTD_compressBound (len);
    guint8 *buf = g_malloc (bound);
    ZSTD_inBuffer ib = { &htlc->out.buf[pos], len, 0 };
    ZSTD_outBuffer ob = { buf, bound, 0 };
    size_t r;

    /* Sync-flush at every transaction boundary so the decoder
	 * can drain immediately, matching the per-transaction
	 * Z_SYNC_FLUSH boundary GZIP uses. */
    do {
        r = ZSTD_compressStream2 (cctx, &ob, &ib, ZSTD_e_flush);
        if (ZSTD_isError (r)) {
            g_free (buf);
            return 0;
        }
    } while (ib.pos < ib.size || r > 0);

    guint32 dstlen = (guint32)ob.pos;
    htlc->out.len -= len;
    qbuf_set (&htlc->out, htlc->out.pos, htlc->out.len + dstlen);
    memcpy (&htlc->out.buf[pos], buf, dstlen);
    g_free (buf);
    return dstlen;
}

#endif /* HAVE_ZSTD */

/* ---------- Dispatch ---------- */

guint32
compress_decode (struct htlc_conn *htlc, struct qbuf *out, struct qbuf *in,
                 guint32 max, guint32 *inusedp)
{
    switch (htlc->compress_decode_type) {
    case COMPRESS_GZIP:
        return gzip_decode (htlc, out, in, max, inusedp);
#ifdef HAVE_LZ4
    case COMPRESS_LZ4:
        return lz4_decode (htlc, out, in, max, inusedp);
#endif
#ifdef HAVE_ZSTD
    case COMPRESS_ZSTD:
        return zstd_decode (htlc, out, in, max, inusedp);
#endif
    default:
        *inusedp = 0;
        return 0;
    }
}

guint32
compress_encode (struct htlc_conn *htlc, guint32 pos, guint32 len)
{
    switch (htlc->compress_encode_type) {
    case COMPRESS_GZIP:
        return gzip_encode (htlc, pos, len);
#ifdef HAVE_LZ4
    case COMPRESS_LZ4:
        return lz4_encode (htlc, pos, len);
#endif
#ifdef HAVE_ZSTD
    case COMPRESS_ZSTD:
        return zstd_encode (htlc, pos, len);
#endif
    default:
        return 0;
    }
}

void
compress_encode_init (struct htlc_conn *htlc)
{
    switch (htlc->compress_encode_type) {
    case COMPRESS_GZIP: {
        z_stream *stream = &htlc->compress_encode_state.stream;
        int err;
        stream->zalloc = 0;
        stream->zfree = 0;
        stream->opaque = 0;
        err = deflateInit (stream, gzip_level);
#if COMPRESS_DEBUG
        writestuff ("di: ", err, 0, 0);
#else
        (void)err;
#endif
        break;
    }
#ifdef HAVE_LZ4
    case COMPRESS_LZ4: {
        LZ4F_cctx *cctx = NULL;
        LZ4F_createCompressionContext (&cctx, LZ4F_VERSION);
        htlc->compress_encode_state.lz4_cctx = cctx;
        break;
    }
#endif
#ifdef HAVE_ZSTD
    case COMPRESS_ZSTD: {
        ZSTD_CCtx *cctx = ZSTD_createCCtx ();
        htlc->compress_encode_state.zstd_cctx = cctx;
        break;
    }
#endif
    default:
        break;
    }
}

void
compress_decode_init (struct htlc_conn *htlc)
{
    switch (htlc->compress_decode_type) {
    case COMPRESS_GZIP: {
        z_stream *stream = &htlc->compress_decode_state.stream;
        int err;
        stream->zalloc = 0;
        stream->zfree = 0;
        err = inflateInit (stream);
#if COMPRESS_DEBUG
        writestuff ("ii: ", err, 0, 0);
#else
        (void)err;
#endif
        break;
    }
#ifdef HAVE_LZ4
    case COMPRESS_LZ4: {
        LZ4F_dctx *dctx = NULL;
        LZ4F_createDecompressionContext (&dctx, LZ4F_VERSION);
        htlc->compress_decode_state.lz4_dctx = dctx;
        break;
    }
#endif
#ifdef HAVE_ZSTD
    case COMPRESS_ZSTD: {
        ZSTD_DCtx *dctx = ZSTD_createDCtx ();
        htlc->compress_decode_state.zstd_dctx = dctx;
        break;
    }
#endif
    default:
        break;
    }
}

void
compress_encode_end (struct htlc_conn *htlc)
{
    switch (htlc->compress_encode_type) {
    case COMPRESS_GZIP:
        deflateEnd (&htlc->compress_encode_state.stream);
        break;
#ifdef HAVE_LZ4
    case COMPRESS_LZ4:
        if (htlc->compress_encode_state.lz4_cctx) {
            LZ4F_freeCompressionContext (htlc->compress_encode_state.lz4_cctx);
            htlc->compress_encode_state.lz4_cctx = NULL;
        }
        break;
#endif
#ifdef HAVE_ZSTD
    case COMPRESS_ZSTD:
        if (htlc->compress_encode_state.zstd_cctx) {
            ZSTD_freeCCtx (htlc->compress_encode_state.zstd_cctx);
            htlc->compress_encode_state.zstd_cctx = NULL;
        }
        break;
#endif
    default:
        break;
    }
}

void
compress_decode_end (struct htlc_conn *htlc)
{
    switch (htlc->compress_decode_type) {
    case COMPRESS_GZIP:
        inflateEnd (&htlc->compress_decode_state.stream);
        break;
#ifdef HAVE_LZ4
    case COMPRESS_LZ4:
        if (htlc->compress_decode_state.lz4_dctx) {
            LZ4F_freeDecompressionContext (htlc->compress_decode_state.lz4_dctx);
            htlc->compress_decode_state.lz4_dctx = NULL;
        }
        break;
#endif
#ifdef HAVE_ZSTD
    case COMPRESS_ZSTD:
        if (htlc->compress_decode_state.zstd_dctx) {
            ZSTD_freeDCtx (htlc->compress_decode_state.zstd_dctx);
            htlc->compress_decode_state.zstd_dctx = NULL;
        }
        break;
#endif
    default:
        break;
    }
}

#endif /* CONFIG_COMPRESS */
