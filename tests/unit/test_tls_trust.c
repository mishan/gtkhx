/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/unit/test_tls_trust.c — Tier 1 coverage for src/tls_trust.c
 * (the SHA-256 fingerprint cache backing the Phase 3 TOFU prompt).
 *
 * Drives the trust DB against a tmpdir-scoped known-hosts file
 * via the GTKHX_KNOWN_HOSTS env override. Pure GLib/GIO; no UI.
 *
 * Subtests:
 *   pin_then_lookup                — pin + immediate-lookup
 *                                    round-trip
 *   unknown_host                   — UNKNOWN for unpinned host
 *   wrong_port                     — UNKNOWN when port differs
 *                                    from pin
 *   mismatch_detected              — pinned host:port + different
 *                                    fp returns MISMATCH (not
 *                                    UNKNOWN)
 *   rewrite_replaces_old           — pinning a new fp for an
 *                                    existing host:port drops
 *                                    the old line (no MISMATCH-
 *                                    on-second-lookup)
 *   preserves_comments_blanks      — # comments and blank lines
 *                                    round-trip a pin call
 *                                    without being dropped or
 *                                    reordered
 *   hostname_only_match            — hand-edited entry without
 *                                    :port matches every port
 *                                    for that host
 *   host_has_fingerprint_cross_port — the any-port helper used
 *                                    by network.c to suppress
 *                                    the second prompt when an
 *                                    HTXF subchannel presents
 *                                    the same cert as the
 *                                    control channel
 *   malformed_port_does_not_widen_trust
 *                                  — hand-edited entries like
 *                                    "host:foo" or
 *                                    "host:5500garbage" must
 *                                    be rejected outright, not
 *                                    silently turned into
 *                                    hostname-only pins
 *   leading_whitespace_in_entry    — hand-edited lines with
 *                                    leading spaces / tabs
 *                                    parse correctly (offset
 *                                    bug fix)
 *   missing_file_is_unknown        — lookup against a non-
 *                                    existent known-hosts file
 *                                    returns UNKNOWN cleanly
 *                                    (first-run path)
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "tls_trust.h"

/* Forward-decl matches the one in src/tls_trust.c so we can link
 * the test binary without dragging gtkhx.c's full pile. The stub
 * below provides the implementation. */
const char *gtkhx_config_dir (void);

static char *test_config_dir = NULL;

const char *
gtkhx_config_dir (void)
{
    return test_config_dir;
}

/* Per-test setup: build a fresh tmpdir + point GTKHX_KNOWN_HOSTS
 * at <tmpdir>/known_hosts. We use the env-var override (not the
 * gtkhx_config_dir() route) so each subtest gets a completely
 * isolated file even though the stub above is process-wide. */
static char *
setup_tmp_known_hosts (void)
{
    GError *err = NULL;
    char *tmpdir = g_dir_make_tmp ("hx-tls-trust-XXXXXX", &err);
    g_assert_no_error (err);
    g_assert_nonnull (tmpdir);
    char *path = g_build_filename (tmpdir, "known_hosts", NULL);
    g_setenv ("GTKHX_KNOWN_HOSTS", path, TRUE);
    g_free (path);
    return tmpdir;
}

static void
teardown_tmp_known_hosts (char *tmpdir)
{
    char *path = g_build_filename (tmpdir, "known_hosts", NULL);
    g_unlink (path);
    g_free (path);
    g_rmdir (tmpdir);
    g_free (tmpdir);
    g_unsetenv ("GTKHX_KNOWN_HOSTS");
}

static const char *FP1 =
    "sha256:0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20";
static const char *FP2 =
    "sha256:aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899";

/* ---- Tests ----------------------------------------------------- */

static void
test_pin_then_lookup (void)
{
    char *tmpdir = setup_tmp_known_hosts ();
    g_assert_true (hx_tls_trust_pin ("localhost", 5610, FP1));
    g_assert_cmpint (hx_tls_trust_lookup ("localhost", 5610, FP1),
                     ==, HX_TLS_TRUST_TRUSTED);
    teardown_tmp_known_hosts (tmpdir);
}

static void
test_unknown_host (void)
{
    char *tmpdir = setup_tmp_known_hosts ();
    g_assert_true (hx_tls_trust_pin ("localhost", 5610, FP1));
    g_assert_cmpint (hx_tls_trust_lookup ("other.example", 5610, FP1),
                     ==, HX_TLS_TRUST_UNKNOWN);
    teardown_tmp_known_hosts (tmpdir);
}

static void
test_wrong_port (void)
{
    char *tmpdir = setup_tmp_known_hosts ();
    g_assert_true (hx_tls_trust_pin ("localhost", 5610, FP1));
    g_assert_cmpint (hx_tls_trust_lookup ("localhost", 5611, FP1),
                     ==, HX_TLS_TRUST_UNKNOWN);
    teardown_tmp_known_hosts (tmpdir);
}

static void
test_mismatch_detected (void)
{
    char *tmpdir = setup_tmp_known_hosts ();
    g_assert_true (hx_tls_trust_pin ("localhost", 5610, FP1));
    /* Same host:port, different fingerprint — exactly the "cert
     * rotation or MITM" scenario the dialog needs to warn about. */
    g_assert_cmpint (hx_tls_trust_lookup ("localhost", 5610, FP2),
                     ==, HX_TLS_TRUST_MISMATCH);
    teardown_tmp_known_hosts (tmpdir);
}

static void
test_rewrite_replaces_old (void)
{
    char *tmpdir = setup_tmp_known_hosts ();
    g_assert_true (hx_tls_trust_pin ("localhost", 5610, FP1));
    /* User accepted a cert rotation — repin with the new
     * fingerprint. The old entry should be gone, so a subsequent
     * lookup with the old fp returns UNKNOWN (no longer MISMATCH:
     * we're not holding state about it). */
    g_assert_true (hx_tls_trust_pin ("localhost", 5610, FP2));
    g_assert_cmpint (hx_tls_trust_lookup ("localhost", 5610, FP2),
                     ==, HX_TLS_TRUST_TRUSTED);
    /* The old fp is now "wrong fp for an entry we have", so it's
     * a MISMATCH against the new pin — not UNKNOWN. That's the
     * correct semantic: if a user sees the OLD cert again after
     * accepting a rotation, that's a downgrade attack worth
     * warning about. */
    g_assert_cmpint (hx_tls_trust_lookup ("localhost", 5610, FP1),
                     ==, HX_TLS_TRUST_MISMATCH);
    teardown_tmp_known_hosts (tmpdir);
}

static void
test_preserves_comments_blanks (void)
{
    char *tmpdir = setup_tmp_known_hosts ();

    /* Hand-author a file with comments + blank lines + an
     * unrelated entry, then pin our entry. After the pin, the
     * comments / blanks should still be present and the
     * unrelated entry untouched. */
    g_autofree char *path = g_build_filename (tmpdir, "known_hosts", NULL);
    const char *initial =
        "# GtkHx known-hosts file\n"
        "\n"
        "other.example:5500 sha256:dead0000000000000000000000000000"
        "00000000000000000000000000000000 # added 2026-01-01\n"
        "\n";
    g_assert_true (g_file_set_contents (path, initial, -1, NULL));

    g_assert_true (hx_tls_trust_pin ("localhost", 5610, FP1));

    g_autofree char *after = NULL;
    g_assert_true (g_file_get_contents (path, &after, NULL, NULL));

    /* The original three lines (header comment, blank, unrelated
     * entry, blank) should survive verbatim, and our new entry
     * appended at the end. */
    g_assert_nonnull (strstr (after, "# GtkHx known-hosts file"));
    g_assert_nonnull (strstr (after, "other.example:5500"));
    g_assert_nonnull (strstr (after, "localhost:5610"));
    g_assert_nonnull (strstr (after, FP1));

    /* Comment should still come BEFORE the unrelated entry which
     * should still come BEFORE the appended one. */
    const char *p_comment = strstr (after, "# GtkHx");
    const char *p_other = strstr (after, "other.example");
    const char *p_pin = strstr (after, "localhost:5610");
    g_assert_cmpint (p_comment < p_other ? 1 : 0, ==, 1);
    g_assert_cmpint (p_other < p_pin ? 1 : 0, ==, 1);

    teardown_tmp_known_hosts (tmpdir);
}

static void
test_hostname_only_match (void)
{
    char *tmpdir = setup_tmp_known_hosts ();
    /* Hand-authored line without :port — SSH known_hosts
     * convention is to match every port. Same semantic for
     * GtkHx so a user can pin once for a server that bounces
     * across multiple ports. */
    g_autofree char *path = g_build_filename (tmpdir, "known_hosts", NULL);
    g_autofree char *contents = g_strdup_printf (
        "myserver %s # hand-pinned\n", FP1);
    g_assert_true (g_file_set_contents (path, contents, -1, NULL));

    g_assert_cmpint (hx_tls_trust_lookup ("myserver", 5500, FP1),
                     ==, HX_TLS_TRUST_TRUSTED);
    g_assert_cmpint (hx_tls_trust_lookup ("myserver", 5600, FP1),
                     ==, HX_TLS_TRUST_TRUSTED);
    /* Wrong fingerprint still trips MISMATCH on a host-only
     * entry — the relaxed port match doesn't relax fingerprint
     * matching. */
    g_assert_cmpint (hx_tls_trust_lookup ("myserver", 5500, FP2),
                     ==, HX_TLS_TRUST_MISMATCH);
    teardown_tmp_known_hosts (tmpdir);
}

/* A hand-edited entry with a bad port suffix (non-numeric, or
 * empty, or out of range) must NOT be accepted as a
 * hostname-only pin. Pre-fix, parse_host_field would split on
 * the last colon, find atoi("foo")=0, leave *port_out at 0,
 * and host_port_match's "line_port == 0 → match any" branch
 * would silently turn the malformed line into a wildcard pin
 * covering every port on that host. The fix rejects the line
 * outright. */
static void
test_malformed_port_does_not_widen_trust (void)
{
    char *tmpdir = setup_tmp_known_hosts ();
    g_autofree char *path = g_build_filename (tmpdir, "known_hosts", NULL);

    /* host:<non-numeric> should not match any port. */
    g_autofree char *bad1 = g_strdup_printf (
        "bad1.example.org:foo %s # hand-typo\n", FP1);
    g_assert_true (g_file_set_contents (path, bad1, -1, NULL));
    g_assert_cmpint (hx_tls_trust_lookup ("bad1.example.org", 5500, FP1),
                     ==, HX_TLS_TRUST_UNKNOWN);
    g_assert_cmpint (hx_tls_trust_lookup ("bad1.example.org", 5600, FP1),
                     ==, HX_TLS_TRUST_UNKNOWN);
    g_assert_false (hx_tls_trust_host_has_fingerprint (
        "bad1.example.org", FP1));

    /* Empty port after colon — same handling. */
    g_autofree char *bad2 = g_strdup_printf (
        "bad2.example.org: %s # hand-typo\n", FP1);
    g_assert_true (g_file_set_contents (path, bad2, -1, NULL));
    g_assert_cmpint (hx_tls_trust_lookup ("bad2.example.org", 5500, FP1),
                     ==, HX_TLS_TRUST_UNKNOWN);

    /* Out-of-range port. */
    g_autofree char *bad3 = g_strdup_printf (
        "bad3.example.org:99999 %s # hand-typo\n", FP1);
    g_assert_true (g_file_set_contents (path, bad3, -1, NULL));
    g_assert_cmpint (hx_tls_trust_lookup ("bad3.example.org", 5500, FP1),
                     ==, HX_TLS_TRUST_UNKNOWN);

    /* Partial parse: "5500garbage" used to silently become 5500
     * because atoi accepted the digit prefix. g_ascii_strtoll
     * with endptr validation must reject the whole line so the
     * lookup misses on every port (including 5500, which used
     * to be the silently-accepted port). */
    g_autofree char *bad4 = g_strdup_printf (
        "bad4.example.org:5500garbage %s # hand-typo\n", FP1);
    g_assert_true (g_file_set_contents (path, bad4, -1, NULL));
    g_assert_cmpint (hx_tls_trust_lookup ("bad4.example.org", 5500, FP1),
                     ==, HX_TLS_TRUST_UNKNOWN);
    g_assert_cmpint (hx_tls_trust_lookup ("bad4.example.org", 0, FP1),
                     ==, HX_TLS_TRUST_UNKNOWN);

    /* Negative port (parses as a negative gint64, fails the
     * range check). */
    g_autofree char *bad5 = g_strdup_printf (
        "bad5.example.org:-1 %s # hand-typo\n", FP1);
    g_assert_true (g_file_set_contents (path, bad5, -1, NULL));
    g_assert_cmpint (hx_tls_trust_lookup ("bad5.example.org", 5500, FP1),
                     ==, HX_TLS_TRUST_UNKNOWN);

    /* Sanity check: a legitimate hostname-only entry (no
     * colon at all) still works — the rejection above only
     * fires when a colon is present. */
    g_autofree char *good = g_strdup_printf (
        "good.example.org %s # hand-pinned\n", FP1);
    g_assert_true (g_file_set_contents (path, good, -1, NULL));
    g_assert_cmpint (hx_tls_trust_lookup ("good.example.org", 5500, FP1),
                     ==, HX_TLS_TRUST_TRUSTED);

    teardown_tmp_known_hosts (tmpdir);
}

/* Cross-port fingerprint match: if cert C is pinned for
 * host:5600 (control channel) and the HTXF subchannel offers
 * the bit-identical C at host:5601, hx_tls_trust_host_has_
 * fingerprint should report TRUE so the accept-certificate
 * handler can silently accept the second port without a
 * second prompt. */
static void
test_host_has_fingerprint_cross_port (void)
{
    char *tmpdir = setup_tmp_known_hosts ();

    /* Pin the cert for the control port. */
    g_assert_true (hx_tls_trust_pin ("multi.example.org", 5600, FP1));

    /* Strict lookup for the xfer port misses — we never pinned
     * 5601. */
    g_assert_cmpint (
        hx_tls_trust_lookup ("multi.example.org", 5601, FP1),
        ==, HX_TLS_TRUST_UNKNOWN);

    /* But the any-port helper reports TRUE, so the accept
     * handler can fast-path it. */
    g_assert_true (
        hx_tls_trust_host_has_fingerprint ("multi.example.org", FP1));

    /* A different fingerprint on the same host stays FALSE —
     * we don't auto-trust by host alone. */
    g_assert_false (
        hx_tls_trust_host_has_fingerprint ("multi.example.org", FP2));

    /* Different host stays FALSE — even with the right
     * fingerprint, a different identity isn't covered. */
    g_assert_false (
        hx_tls_trust_host_has_fingerprint ("other.example.org", FP1));

    teardown_tmp_known_hosts (tmpdir);
}

/* Regression: a hand-edited entry with leading whitespace (a
 * tab or spaces before the host token) used to silently fail to
 * match because parse_host_field skipped the whitespace
 * internally but returned a length measured from the post-skip
 * position. The caller (locate_fingerprint) offset back from
 * the original line pointer and landed mid-host-token, so the
 * fingerprint walk fell off the entry. The fix returns the
 * offset from the original `line`, not from the trimmed start. */
static void
test_leading_whitespace_in_entry (void)
{
    char *tmpdir = setup_tmp_known_hosts ();
    g_autofree char *path = g_build_filename (tmpdir, "known_hosts", NULL);
    /* Two spaces + a tab before the host. */
    g_autofree char *contents = g_strdup_printf (
        "  \tspaced.example.org:5500 %s # hand-pinned\n", FP1);
    g_assert_true (g_file_set_contents (path, contents, -1, NULL));

    g_assert_cmpint (hx_tls_trust_lookup ("spaced.example.org", 5500, FP1),
                     ==, HX_TLS_TRUST_TRUSTED);
    teardown_tmp_known_hosts (tmpdir);
}

static void
test_missing_file_is_unknown (void)
{
    char *tmpdir = setup_tmp_known_hosts ();
    /* Don't pin anything — first-run state. Lookup should be
     * UNKNOWN without printing a warning or aborting. */
    g_assert_cmpint (hx_tls_trust_lookup ("localhost", 5610, FP1),
                     ==, HX_TLS_TRUST_UNKNOWN);
    teardown_tmp_known_hosts (tmpdir);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    /* The gtkhx_config_dir stub is process-wide, but every test
     * goes through GTKHX_KNOWN_HOSTS which takes precedence — so
     * a stub returning NULL is harmless. We point it at a junk
     * path defensively so if the env override is somehow not in
     * effect a test fails loudly rather than touching the
     * developer's real $HOME/.config/gtkhx/known_hosts. */
    test_config_dir = g_strdup ("/nonexistent-test-dir");

    g_test_add_func ("/tls_trust/pin_then_lookup", test_pin_then_lookup);
    g_test_add_func ("/tls_trust/unknown_host", test_unknown_host);
    g_test_add_func ("/tls_trust/wrong_port", test_wrong_port);
    g_test_add_func ("/tls_trust/mismatch_detected", test_mismatch_detected);
    g_test_add_func ("/tls_trust/rewrite_replaces_old",
                     test_rewrite_replaces_old);
    g_test_add_func ("/tls_trust/preserves_comments_blanks",
                     test_preserves_comments_blanks);
    g_test_add_func ("/tls_trust/hostname_only_match",
                     test_hostname_only_match);
    g_test_add_func ("/tls_trust/host_has_fingerprint_cross_port",
                     test_host_has_fingerprint_cross_port);
    g_test_add_func ("/tls_trust/malformed_port_does_not_widen_trust",
                     test_malformed_port_does_not_widen_trust);
    g_test_add_func ("/tls_trust/leading_whitespace_in_entry",
                     test_leading_whitespace_in_entry);
    g_test_add_func ("/tls_trust/missing_file_is_unknown",
                     test_missing_file_is_unknown);

    int rc = g_test_run ();
    g_free (test_config_dir);
    return rc;
}
