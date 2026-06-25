#ifndef __compress_h
#define __compress_h


#include "config.h"

#include <sys/types.h> /* u_int32_t */

/* HOPE-Secure-Login transport compression algorithms.
 *
 * Phase R1 moved the codec implementations to Rust
 * (rust/crates/hxcompress). This header keeps the numeric IDs
 * aligned with the wire-protocol negotiation and exposes a single
 * opaque pointer so htlc_conn can carry the per-direction codec
 * state without naming the Rust types.
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
 * negotiate), so renumber-safe. The Rust crate has matching
 * constants; the FFI takes the algorithm ID by value rather than
 * reaching into a shared enum. */
#define COMPRESS_NONE 0
#define COMPRESS_GZIP 1
#define COMPRESS_LZ4 2
#define COMPRESS_ZSTD 3

/* Per-direction codec state. The pointer is opaque on the C side
 * — gtkhx_compress_{encoder,decoder}_new returns it, the
 * encode/decode FFI takes it, and gtkhx_compress_{encoder,decoder}
 * _free destroys it. compress.c knows the actual Rust type
 * (CompressEncoder / CompressDecoder) via the cast inside the
 * dispatch. */
union compress_state {
    void *ctx;
};

struct htlc_conn;
struct qbuf;

/* The C compression dispatch (compress.c — compress_encode / decode /
 * _init / _end / id_from_name) was retired once the hxnet orchestrator
 * took over the control-channel transport: zlib/gzip compression now
 * runs inside the Rust `hxcompress` crate. Only the shared types above
 * (COMPRESS_* ids + the compress_state union the htlc_conn fields use)
 * and the compress_encode_bufsize helper below survive here. */

/* Worst-case output buffer size for a gzip-deflate of an input of `len`
 * bytes. Computes `2 * len + 1024` in u64 internally; returns 0 if the
 * u32 result would overflow (any len > ~2 GiB). Defined `static inline`
 * here so the Tier 1 test in tests/unit/test_compress_bufsize.c can drive
 * the overflow guard without linking any compression backend. */
#include <stdint.h>
static inline u_int32_t
compress_encode_bufsize (u_int32_t len)
{
    /* 64-bit accumulator so the u32 result's overflow case is
     * detectable rather than wrap-around-silently. */
    uint64_t bound = 2ULL * (uint64_t) len + 1024ULL;
    if (bound > (uint64_t) 0xffffffffu) {
        return 0;
    }
    return (u_int32_t) bound;
}


#endif
