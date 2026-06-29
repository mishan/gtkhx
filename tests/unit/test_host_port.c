/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

/*
 * test_host_port.c — Tier 1 coverage for gtkhx_parse_host_port /
 * gtkhx_join_host_port. The IPv6 cases are the load-bearing ones: the
 * hand-rolled strrchr(':') splits these replaced mis-parsed "::1" as
 * host "::" port "1" and double-bracketed "[::1]:5500".
 */

#include "config.h"

#include <glib.h>

#include "host_port.h"

/* host:port → expected host + port + had_port. */
static void
expect_ok (const char *in, guint16 def, const char *want_host,
           guint16 want_port, gboolean want_had)
{
    char *host = NULL;
    guint16 port = 0xABCD;
    gboolean had = !want_had;
    gboolean ok = gtkhx_parse_host_port (in, def, &host, &port, &had);
    g_assert_true (ok);
    g_assert_nonnull (host);
    g_assert_cmpstr (host, ==, want_host);
    g_assert_cmpuint (port, ==, want_port);
    g_assert_cmpint (had, ==, want_had);
    g_free (host);
}

static void
expect_fail (const char *in)
{
    char *host = (char *) 0x1; /* poison — must be cleared to NULL */
    gboolean ok = gtkhx_parse_host_port (in, 5500, &host, NULL, NULL);
    g_assert_false (ok);
    g_assert_null (host);
}

static void
test_parse_plain (void)
{
    expect_ok ("hotline.example.com", 5500, "hotline.example.com", 5500,
               FALSE);
    expect_ok ("hotline.example.com:1234", 5500, "hotline.example.com", 1234,
               TRUE);
    expect_ok ("1.2.3.4", 5500, "1.2.3.4", 5500, FALSE);
    expect_ok ("1.2.3.4:5600", 5500, "1.2.3.4", 5600, TRUE);
    /* default_port = 0 is allowed (callers that treat 0 as unset). */
    expect_ok ("host", 0, "host", 0, FALSE);
}

static void
test_parse_ipv6 (void)
{
    /* Bracketed: with and without a port. */
    expect_ok ("[::1]:5600", 5500, "::1", 5600, TRUE);
    expect_ok ("[::1]", 5500, "::1", 5500, FALSE);
    expect_ok ("[2001:db8::1]:1080", 5500, "2001:db8::1", 1080, TRUE);
    expect_ok ("[fe80::1]", 9999, "fe80::1", 9999, FALSE);
    /* Unbracketed IPv6 literal (2+ colons) → host only, NOT host:port.
     * This is the bug the helper exists to kill: strrchr(':') used to
     * split "::1" as host "::" port "1". */
    expect_ok ("::1", 5500, "::1", 5500, FALSE);
    expect_ok ("2001:db8::1", 5500, "2001:db8::1", 5500, FALSE);
    expect_ok ("fe80::1", 5500, "fe80::1", 5500, FALSE);
}

static void
test_parse_rejects (void)
{
    expect_fail (NULL);
    expect_fail ("");
    expect_fail (":5500");            /* empty host */
    expect_fail ("host:");            /* empty port */
    expect_fail ("host:0");           /* port out of 1..65535 */
    expect_fail ("host:65536");       /* port out of range */
    expect_fail ("host:99999");       /* port out of range */
    expect_fail ("host:12ab");        /* trailing garbage */
    expect_fail ("host:notaport");    /* non-numeric */
    expect_fail ("[::1");             /* unterminated bracket */
    expect_fail ("[::1]x");           /* junk after bracket */
    expect_fail ("[::1]:");           /* empty port after bracket */
    expect_fail ("[::1]:bad");        /* bad port after bracket */
    expect_fail ("[]:5500");          /* empty bracketed host */
}

static void
test_join (void)
{
    char *s;

    s = gtkhx_join_host_port ("host", 5500);
    g_assert_cmpstr (s, ==, "host:5500");
    g_free (s);

    s = gtkhx_join_host_port ("1.2.3.4", 5500);
    g_assert_cmpstr (s, ==, "1.2.3.4:5500");
    g_free (s);

    s = gtkhx_join_host_port ("::1", 5600);
    g_assert_cmpstr (s, ==, "[::1]:5600");
    g_free (s);

    s = gtkhx_join_host_port ("2001:db8::1", 1080);
    g_assert_cmpstr (s, ==, "[2001:db8::1]:1080");
    g_free (s);
}

/* Round-trip: join then parse gets the original host + port back. */
static void
test_roundtrip (void)
{
    const char *hosts[] = { "host", "1.2.3.4", "::1", "2001:db8::1",
                            "fe80::dead:beef" };
    for (gsize i = 0; i < G_N_ELEMENTS (hosts); i++) {
        char *joined = gtkhx_join_host_port (hosts[i], 5512);
        char *host = NULL;
        guint16 port = 0;
        gboolean had = FALSE;
        g_assert_true (
            gtkhx_parse_host_port (joined, 1, &host, &port, &had));
        g_assert_cmpstr (host, ==, hosts[i]);
        g_assert_cmpuint (port, ==, 5512);
        g_assert_true (had);
        g_free (host);
        g_free (joined);
    }
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/host_port/parse/plain", test_parse_plain);
    g_test_add_func ("/host_port/parse/ipv6", test_parse_ipv6);
    g_test_add_func ("/host_port/parse/rejects", test_parse_rejects);
    g_test_add_func ("/host_port/join", test_join);
    g_test_add_func ("/host_port/roundtrip", test_roundtrip);
    return g_test_run ();
}
