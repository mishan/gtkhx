/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * tests/unit/test_compress_bufsize.c — Tier 1 pin on the
 * compress_encode_bufsize overflow guard.
 *
 * compress.c::compress_encode allocates `compress_encode_bufsize(len)`
 * bytes to hold the encoded output before handing the buffer to the
 * Rust gtkhx_compress_encode FFI. Pre-Phase-R1-round-3 the bufsize
 * was `2 * len + 1024` in u32 arithmetic; for sufficiently large
 * `len` it would wrap, the malloc would be undersized, and the
 * encoder would either fail silently or (worse, with a future Rust
 * impl that didn't bounds-check internally) write past the buffer.
 *
 * The fixed function computes in u64 and returns 0 on overflow.
 * This test pins:
 *   - Small / typical inputs return the expected size.
 *   - Inputs near the overflow boundary still fit (last successful
 *     value).
 *   - Inputs past the boundary return 0, NOT a wrapped-around
 *     undersized value.
 *
 * No GLib / GTK dependency in the unit under test — compress.h's
 * static inline definition is the whole implementation.
 */

#include "config.h"

#include <glib.h>
#include <stdint.h>

#include "compress.h"

static void
test_small_inputs_compute_expected_size (void)
{
    /* len=0 → 1024 (the constant overhead). */
    g_assert_cmpuint (compress_encode_bufsize (0), ==, 1024);

    /* len=1024 → 2*1024 + 1024 = 3072. */
    g_assert_cmpuint (compress_encode_bufsize (1024), ==, 3072);

    /* A typical Hotline transaction: 4 KB. */
    g_assert_cmpuint (compress_encode_bufsize (4096), ==, 4096u * 2 + 1024);
}

static void
test_near_overflow_boundary_still_fits (void)
{
    /* Largest `len` for which `2*len + 1024` fits in u32. Walking
     * the math:
     *
     *   max possible u32 result = 0xffffffff
     *   max possible (2*len)    = 0xffffffff - 1024 = 0xfffffbff
     *   integer-divide by 2     = 0x7ffffdff
     *
     * The integer divide TRUNCATES because 0xfffffbff is odd, so
     * the largest representable `len` falls one short of yielding
     * a result of exactly UINT32_MAX. Concretely:
     *
     *   2 * 0x7ffffdff = 0xfffffbfe
     *   +1024          = 0xfffffffe  (UINT32_MAX - 1)
     *
     * So `compress_encode_bufsize(0x7ffffdff)` returns 0xfffffffe,
     * not 0xffffffff. (A reader expecting equality with UINT32_MAX
     * is forgetting the odd-divide truncation; the function itself
     * allows the equality case — it only returns 0 when
     * `bound > UINT32_MAX` — but no `len` value actually hits the
     * equality.) */
    guint32 max_safe_len = (0xffffffffu - 1024u) / 2u; /* 0x7ffffdff */
    guint32 result = compress_encode_bufsize (max_safe_len);
    g_assert_cmpuint (result, !=, 0);
    g_assert_cmpuint (result, ==, 0xfffffffeu);
}

static void
test_overflow_returns_zero (void)
{
    /* Past the boundary. Use +2 (not +1): with +1, the pre-fix u32
     * arithmetic `2 * (0x7ffffdff + 1) + 1024 = 0x100000000` would
     * WRAP to 0x0 — which is indistinguishable from the fixed
     * code's overflow sentinel return value. +2 wraps to 0x2 under
     * the buggy code, so the test only passes against the
     * overflow-checking implementation:
     *
     *   pre-fix:   compress_encode_bufsize(0x7ffffe01) = 0x2 (wrap)
     *   post-fix:  compress_encode_bufsize(0x7ffffe01) = 0   (overflow)
     */
    guint32 just_over = ((0xffffffffu - 1024u) / 2u) + 2u; /* 0x7ffffe01 */
    g_assert_cmpuint (compress_encode_bufsize (just_over), ==, 0);

    /* Way past the boundary: u32::MAX. 2*MAX + 1024 in u64 is well
     * over 8 GiB; in u32 it would wrap to 0x3fe. The fix must
     * surface the overflow as 0, not let the wrapped value pass
     * through. */
    g_assert_cmpuint (compress_encode_bufsize (0xffffffffu), ==, 0);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/compress_bufsize/small_inputs",
                     test_small_inputs_compute_expected_size);
    g_test_add_func ("/compress_bufsize/near_overflow_boundary",
                     test_near_overflow_boundary_still_fits);
    g_test_add_func ("/compress_bufsize/overflow_returns_zero",
                     test_overflow_returns_zero);

    return g_test_run ();
}
