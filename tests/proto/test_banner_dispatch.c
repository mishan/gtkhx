/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/proto/test_banner_dispatch.c — Tier 2 coverage for the
 * pure helpers carved out of banner.c into src/banner_dispatch.c.
 *
 * Two contracts under test:
 *
 *   hx_banner_type_is_url — the URL-vs-file mode classifier.
 *       Earlier revisions of banner.c keyed dispatch on whether
 *       the HTLS_DATA_BANNER_URL chunk was present, not the TYPE
 *       chunk; mhxd in file mode still sends a URL chunk and the
 *       old logic mis-routed those transfers into URL mode. The
 *       single source of truth lives here now — pin every input
 *       shape we expect on the wire so a future "simplification"
 *       doesn't quietly recreate the bug.
 *
 *   hx_banner_validate_htxf_reply — the size-cap that stands
 *       between a hostile server and an unbounded g_malloc.
 *       Pin every reject reason as a distinct enum value so a
 *       future logging layer can attribute drops correctly,
 *       and pin the boundary at HX_BANNER_MAX_HTXF_SIZE
 *       (currently 1 MiB) explicitly.
 *
 * No GTK, no libsoup, no pthread — the rest of banner.c covers
 * the UI side, exercised end-to-end by the Tier 3 banner tests
 * against mhxd + Janus.
 */

#include "config.h"

#include <glib.h>
#include "banner_dispatch.h"

/* ---- hx_banner_type_is_url ------------------------------------ */

/* Each (input, expected) row is one test. Picking the canonical
 * production shapes ("URL " from mhxd, "URL\0" from servers that
 * NUL-terminate at 3, lowercase variants, GIFf/JPEG/PICT in file
 * mode) plus the defensive NULL / "" / leading-space inputs. */

typedef struct {
    const char *input;
    gboolean    expected;
    const char *label;
} type_case;

static const type_case kTypeCases[] = {
    /* URL-mode shapes the spec / servers produce. */
    { "URL ",   TRUE,  "mhxd space-padded \"URL \"" },
    { "URL",    TRUE,  "NUL-terminated \"URL\\0\"" },
    { "url ",   TRUE,  "lowercase, space-padded" },
    { "Url",    TRUE,  "mixed case" },
    { "URL\0X", TRUE,  "embedded NUL stops scan" },
    /* File-mode codes — anything not URL. */
    { "GIFf",   FALSE, "GIFf file mode" },
    { "JPEG",   FALSE, "JPEG file mode" },
    { "PICT",   FALSE, "PICT file mode" },
    { "PNG ",   FALSE, "PNG  file mode" },
    /* Defensive inputs. NULL and empty string both fall to FALSE
     * (file mode); the caller in banner.c separately checks for
     * a missing URL chunk and shows a friendly caption. */
    { "",       FALSE, "empty string" },
    /* Edge: pure space prefix means the trimmed code is empty,
     * which is not "URL", so we treat it as file mode. The
     * production helper used to scan up to 8 chars for a non-
     * space; this pinning ensures a future refactor keeps the
     * same defensive shape. */
    { " URL ",  FALSE, "leading space defeats match" },
    /* Long string without a space terminator. The helper copies
     * up to 7 chars (scratch[8] − NUL slot), so the trimmed
     * comparison string is "URLABCD" — which is NOT "URL", so
     * the result is FALSE. Pins the contract that an unterminated
     * long code does NOT silently match "URL" via a prefix-only
     * compare. */
    { "URLABCDEFGH", FALSE, "long unterminated prefix doesn't match" },
};

static void
test_type_is_url_table (void)
{
    for (gsize i = 0; i < G_N_ELEMENTS (kTypeCases); i++) {
        const type_case *c = &kTypeCases[i];
        gboolean got = hx_banner_type_is_url (c->input);
        if (got != c->expected) {
            g_error ("case %zu (%s): input=\"%s\" expected=%d got=%d",
                     i, c->label, c->input ? c->input : "(null)",
                     c->expected, got);
        }
    }
}

static void
test_type_is_url_null (void)
{
    /* Separate from the table since NULL needs a different print
     * format in the assertion. The defensive NULL guard is the
     * first line of the helper and exists to keep callers from
     * having to wrap every passthrough. */
    g_assert_false (hx_banner_type_is_url (NULL));
}

/* ---- hx_banner_validate_htxf_reply ---------------------------- */

static void
test_htxf_validate_ok (void)
{
    /* Real-world sizes from Janus's banner.gif fixture (~2 KB)
     * and a typical 468x60 JPEG (~30 KB). Both should fly. */
    g_assert_cmpint (hx_banner_validate_htxf_reply (1, 2266), ==,
                     HX_BANNER_HTXF_OK);
    g_assert_cmpint (hx_banner_validate_htxf_reply (0xdeadbeef, 30000), ==,
                     HX_BANNER_HTXF_OK);
    /* Smallest legal pair: ref=1, size=1 — both nonzero. */
    g_assert_cmpint (hx_banner_validate_htxf_reply (1, 1), ==,
                     HX_BANNER_HTXF_OK);
}

static void
test_htxf_validate_zero_ref (void)
{
    /* ref=0 is the unallocated sentinel — server didn't actually
     * queue a transfer for us. Reject regardless of size. */
    g_assert_cmpint (hx_banner_validate_htxf_reply (0, 1000), ==,
                     HX_BANNER_HTXF_ZERO_REF);
    /* ref=0 takes priority over size=0 because banner.c's order
     * of checks does. Pin it so a refactor that reorders the
     * branches surfaces here. */
    g_assert_cmpint (hx_banner_validate_htxf_reply (0, 0), ==,
                     HX_BANNER_HTXF_ZERO_REF);
}

static void
test_htxf_validate_zero_size (void)
{
    g_assert_cmpint (hx_banner_validate_htxf_reply (42, 0), ==,
                     HX_BANNER_HTXF_ZERO_SIZE);
}

static void
test_htxf_validate_too_large (void)
{
    /* Exactly at the cap is fine — strict greater-than rejects. */
    g_assert_cmpint (hx_banner_validate_htxf_reply (1, HX_BANNER_MAX_HTXF_SIZE),
                     ==, HX_BANNER_HTXF_OK);
    /* One byte over → reject. Pinning the boundary catches an
     * off-by-one in the comparison. */
    g_assert_cmpint (hx_banner_validate_htxf_reply (1,
                                                    HX_BANNER_MAX_HTXF_SIZE + 1),
                     ==, HX_BANNER_HTXF_TOO_LARGE);
    /* Adversarial: a server that wants us to allocate ~4 GiB. */
    g_assert_cmpint (hx_banner_validate_htxf_reply (1, 0xffffffffu), ==,
                     HX_BANNER_HTXF_TOO_LARGE);
}

static void
test_htxf_validate_cap_value (void)
{
    /* The cap is a public protocol-policy number, not an
     * implementation detail. Pin it so a future bump shows up in
     * the test diff and gets a deliberate decision. */
    g_assert_cmpuint (HX_BANNER_MAX_HTXF_SIZE, ==, 1024u * 1024u);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/banner_dispatch/type_is_url/table",
                     test_type_is_url_table);
    g_test_add_func ("/banner_dispatch/type_is_url/null",
                     test_type_is_url_null);
    g_test_add_func ("/banner_dispatch/htxf_validate/ok",
                     test_htxf_validate_ok);
    g_test_add_func ("/banner_dispatch/htxf_validate/zero_ref",
                     test_htxf_validate_zero_ref);
    g_test_add_func ("/banner_dispatch/htxf_validate/zero_size",
                     test_htxf_validate_zero_size);
    g_test_add_func ("/banner_dispatch/htxf_validate/too_large",
                     test_htxf_validate_too_large);
    g_test_add_func ("/banner_dispatch/htxf_validate/cap_value",
                     test_htxf_validate_cap_value);

    return g_test_run ();
}
