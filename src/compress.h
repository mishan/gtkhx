#ifndef GTKHX_COMPRESS_H
#define GTKHX_COMPRESS_H

#include "config.h"

#include <stdint.h> /* uint32_t */


// NOTE: This is currently only used in tests.

/* Worst-case output buffer size for a gzip-deflate of an input of `len`
 * bytes. Computes `2 * len + 1024` in u64 internally; returns 0 if the
 * u32 result would overflow (any len > ~2 GiB). Defined `static inline`
 * here so the Tier 1 test in tests/unit/test_compress_bufsize.c can drive
 * the overflow guard without linking any compression backend. */
static inline uint32_t
compress_encode_bufsize (uint32_t len)
{
    /* 64-bit accumulator so the u32 result's overflow case is
     * detectable rather than wrap-around-silently. */
    uint64_t bound = 2ULL * (uint64_t) len + 1024ULL;
    if (bound > (uint64_t) 0xffffffffu) {
        return 0;
    }
    return (uint32_t) bound;
}

#endif /* GTKHX_COMPRESS_H */
