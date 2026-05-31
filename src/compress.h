#ifndef __compress_h
#define __compress_h


#include "config.h"

#include <sys/types.h> /* u_int32_t */

/* HOPE-Secure-Login transport compression algorithms.
 *
 *   GZIP — zlib (RFC 1950). Despite the name, NOT gzip (RFC 1952).
 *          Uses deflateInit/inflateInit with Z_SYNC_FLUSH per chunk.
 *          The original HOPE algorithm, supported by every hx-family
 *          server.
 *   LZ4  — LZ4 frame format (spec v1.6). Janus/Klein extension; fast
 *          compression with low CPU overhead. Persistent streaming
 *          codec per chunk.
 *   ZSTD — Zstandard (RFC 8878). Janus/Klein extension; best
 *          compression ratio. Preferred for modern AEAD clients.
 *          Persistent streaming codec per chunk.
 *
 * IDs match the order they were added to the protocol family — the
 * value isn't observable on the wire (the name string is what we
 * negotiate), so renumber-safe. */
#define COMPRESS_NONE 0
#define COMPRESS_GZIP 1
#define COMPRESS_LZ4 2
#define COMPRESS_ZSTD 3

/* Opaque Rust-allocated encoder/decoder state. Created via
 * gtkhx_compress_encoder_new / gtkhx_compress_decoder_new and
 * freed via the corresponding _free functions. The union is kept
 * only for sizing; only the pointer member is used. */
union compress_state {
    void *ctx;  /* opaque Rust CompressEncoder* or CompressDecoder* */
};

struct htlc_conn;
struct qbuf;

extern u_int32_t compress_decode (struct htlc_conn *htlc, struct qbuf *out,
                                  struct qbuf *in, u_int32_t max,
                                  u_int32_t *inusedp);
extern u_int32_t compress_encode (struct htlc_conn *htlc, u_int32_t pos,
                                  u_int32_t len);
extern void compress_encode_init (struct htlc_conn *htlc);
extern void compress_decode_init (struct htlc_conn *htlc);
extern void compress_encode_end (struct htlc_conn *htlc);
extern void compress_decode_end (struct htlc_conn *htlc);

/* Map a HOPE algorithm name ("GZIP" / "LZ4" / "ZSTD") to its
 * COMPRESS_* numeric id. Returns COMPRESS_NONE for "NONE", the
 * empty string, or any name not recognised in this build. Used
 * by rcv.c when applying the server's algorithm selection from
 * the HOPE Step 2 reply. */
extern u_int16_t compress_id_from_name (const char *name);


#endif
