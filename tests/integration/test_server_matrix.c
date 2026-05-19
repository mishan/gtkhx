/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * Unit-style coverage for the multi-server matrix helpers in
 * server_matrix.c. This is the only test in the "integration" suite
 * that does not actually talk to a Hotline server — it exercises
 * hx_test_servers_with()'s filter logic and hx_test_server_default()'s
 * env-var overrides in-process. Lives in the integration tests
 * directory because it tests integration-harness code, but it's
 * deterministic and runs in milliseconds.
 *
 * Each subtest sets GTKHX_TEST_SERVERS / GTKHX_TEST_HOST /
 * GTKHX_TEST_PORT, calls the helper, asserts the outcome, and
 * unsets the env vars. We cannot reset the cached env filter inside
 * server_matrix.c — load_env_filter() is a once-only — so each
 * test that depends on the cache being fresh runs in a forked
 * subprocess via g_test_trap_subprocess.
 */

#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "server_matrix.h"

static void
test_matrix_has_entries (void)
{
    /* Sanity: the static matrix is non-empty. */
    g_assert_cmpuint (hx_test_server_matrix_count, >, 0);
    g_assert_nonnull (hx_test_server_matrix);
    /* The first entry has a non-empty name and a sensible port. */
    g_assert_nonnull (hx_test_server_matrix[0].name);
    g_assert_cmpstr (hx_test_server_matrix[0].name, !=, "");
    g_assert_cmpuint (hx_test_server_matrix[0].port, >, 0);
}

static void
test_servers_with_zero_caps_returns_all (void)
{
    if (g_test_subprocess ()) {
        g_unsetenv ("GTKHX_TEST_SERVERS");
        GPtrArray *all = hx_test_servers_with (0);
        g_assert_nonnull (all);
        g_assert_cmpuint (all->len, ==, hx_test_server_matrix_count);
        g_ptr_array_unref (all);
        return;
    }
    g_test_trap_subprocess (NULL, 0, G_TEST_SUBPROCESS_INHERIT_STDOUT);
    g_test_trap_assert_passed ();
}

static void
test_servers_with_unknown_cap_returns_empty (void)
{
    if (g_test_subprocess ()) {
        g_unsetenv ("GTKHX_TEST_SERVERS");
        /* Use the top bit of the cap bitmask as a synthetic
         * "no entry will ever advertise this" sentinel. The
         * earlier version of this test used CAP_CHAT_HISTORY,
         * which was a fine sentinel until Phase C landed Janus
         * in the matrix advertising exactly that bit. Filtering
         * by a never-allocated bit gives a stable empty-result
         * assertion that doesn't move when real caps appear. */
        GPtrArray *r = hx_test_servers_with (0x80000000u);
        g_assert_nonnull (r);
        g_assert_cmpuint (r->len, ==, 0);
        g_ptr_array_unref (r);
        return;
    }
    g_test_trap_subprocess (NULL, 0, G_TEST_SUBPROCESS_INHERIT_STDOUT);
    g_test_trap_assert_passed ();
}

static void
test_servers_with_chat_history_cap (void)
{
    if (g_test_subprocess ()) {
        g_unsetenv ("GTKHX_TEST_SERVERS");
        /* Phase C: Janus advertises HX_TEST_CAP_CHAT_HISTORY.
         * The filter should find exactly that one entry; mhxd
         * does not claim chat-history. When Mobius eventually
         * adds the extension this assertion's expected count
         * goes up — that's the whole point of the matrix. */
        GPtrArray *r = hx_test_servers_with (HX_TEST_CAP_CHAT_HISTORY);
        g_assert_nonnull (r);
        g_assert_cmpuint (r->len, ==, 1);
        const hx_test_server *s = g_ptr_array_index (r, 0);
        g_assert_cmpstr (s->name, ==, "janus");
        g_ptr_array_unref (r);
        return;
    }
    g_test_trap_subprocess (NULL, 0, G_TEST_SUBPROCESS_INHERIT_STDOUT);
    g_test_trap_assert_passed ();
}

static void
test_env_filter_named_match (void)
{
    if (g_test_subprocess ()) {
        /* Filter to only "mhxd" — should match the matrix entry. */
        g_setenv ("GTKHX_TEST_SERVERS", "mhxd", TRUE);
        GPtrArray *r = hx_test_servers_with (0);
        g_assert_nonnull (r);
        g_assert_cmpuint (r->len, ==, 1);
        const hx_test_server *s = g_ptr_array_index (r, 0);
        g_assert_cmpstr (s->name, ==, "mhxd");
        g_ptr_array_unref (r);
        return;
    }
    g_test_trap_subprocess (NULL, 0, G_TEST_SUBPROCESS_INHERIT_STDOUT);
    g_test_trap_assert_passed ();
}

static void
test_env_filter_named_no_match (void)
{
    if (g_test_subprocess ()) {
        /* Filter to a non-existent name — empty result. */
        g_setenv ("GTKHX_TEST_SERVERS", "nonsuch", TRUE);
        GPtrArray *r = hx_test_servers_with (0);
        g_assert_nonnull (r);
        g_assert_cmpuint (r->len, ==, 0);
        g_ptr_array_unref (r);

        /* And the default-target lookup should return NULL —
         * caller's signal to skip the test. */
        const hx_test_server *def = hx_test_server_default ();
        g_assert_null (def);
        return;
    }
    g_test_trap_subprocess (NULL, 0, G_TEST_SUBPROCESS_INHERIT_STDOUT);
    g_test_trap_assert_passed ();
}

static void
test_env_filter_case_insensitive (void)
{
    if (g_test_subprocess ()) {
        g_setenv ("GTKHX_TEST_SERVERS", "MHXD", TRUE);
        GPtrArray *r = hx_test_servers_with (0);
        g_assert_cmpuint (r->len, ==, 1);
        g_ptr_array_unref (r);
        return;
    }
    g_test_trap_subprocess (NULL, 0, G_TEST_SUBPROCESS_INHERIT_STDOUT);
    g_test_trap_assert_passed ();
}

static void
test_env_filter_whitespace_and_commas (void)
{
    if (g_test_subprocess ()) {
        /* Commas and surrounding whitespace handled; empty tokens
         * dropped silently. */
        g_setenv ("GTKHX_TEST_SERVERS", " mhxd ,, ,nonsuch ", TRUE);
        GPtrArray *r = hx_test_servers_with (0);
        g_assert_cmpuint (r->len, ==, 1);
        const hx_test_server *s = g_ptr_array_index (r, 0);
        g_assert_cmpstr (s->name, ==, "mhxd");
        g_ptr_array_unref (r);
        return;
    }
    g_test_trap_subprocess (NULL, 0, G_TEST_SUBPROCESS_INHERIT_STDOUT);
    g_test_trap_assert_passed ();
}

static void
test_default_legacy_host_port_override (void)
{
    if (g_test_subprocess ()) {
        g_unsetenv ("GTKHX_TEST_SERVERS");
        g_setenv ("GTKHX_TEST_HOST", "203.0.113.42", TRUE);
        g_setenv ("GTKHX_TEST_PORT", "12345", TRUE);

        const hx_test_server *def = hx_test_server_default ();
        g_assert_nonnull (def);
        g_assert_cmpstr (def->host, ==, "203.0.113.42");
        g_assert_cmpuint (def->port, ==, 12345);
        /* xfer_port defaults to port + 1 when only HOST/PORT are
         * set without GTKHX_TEST_XFER_PORT. */
        g_assert_cmpuint (def->xfer_port, ==, 12346);
        return;
    }
    g_test_trap_subprocess (NULL, 0, G_TEST_SUBPROCESS_INHERIT_STDOUT);
    g_test_trap_assert_passed ();
}

static void
test_default_xfer_port_explicit_override (void)
{
    if (g_test_subprocess ()) {
        g_unsetenv ("GTKHX_TEST_SERVERS");
        g_setenv ("GTKHX_TEST_HOST", "203.0.113.42", TRUE);
        g_setenv ("GTKHX_TEST_PORT", "12345", TRUE);
        g_setenv ("GTKHX_TEST_XFER_PORT", "55501", TRUE);

        const hx_test_server *def = hx_test_server_default ();
        g_assert_nonnull (def);
        g_assert_cmpuint (def->port, ==, 12345);
        g_assert_cmpuint (def->xfer_port, ==, 55501);
        return;
    }
    g_test_trap_subprocess (NULL, 0, G_TEST_SUBPROCESS_INHERIT_STDOUT);
    g_test_trap_assert_passed ();
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/integration/server-matrix/has-entries",
                     test_matrix_has_entries);
    g_test_add_func ("/integration/server-matrix/zero-caps-all",
                     test_servers_with_zero_caps_returns_all);
    g_test_add_func ("/integration/server-matrix/unknown-cap-empty",
                     test_servers_with_unknown_cap_returns_empty);
    g_test_add_func ("/integration/server-matrix/chat-history-cap",
                     test_servers_with_chat_history_cap);
    g_test_add_func ("/integration/server-matrix/env-named-match",
                     test_env_filter_named_match);
    g_test_add_func ("/integration/server-matrix/env-named-no-match",
                     test_env_filter_named_no_match);
    g_test_add_func ("/integration/server-matrix/env-case-insensitive",
                     test_env_filter_case_insensitive);
    g_test_add_func ("/integration/server-matrix/env-whitespace",
                     test_env_filter_whitespace_and_commas);
    g_test_add_func ("/integration/server-matrix/legacy-host-port",
                     test_default_legacy_host_port_override);
    g_test_add_func ("/integration/server-matrix/xfer-port-override",
                     test_default_xfer_port_explicit_override);
    return g_test_run ();
}
