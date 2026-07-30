/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

/*
 * tests/unit/test_hotline_url.c — Tier 1 pin for the hotline://
 * URL parser. The parser feeds two user actions in gtkurl.c's
 * right-click popup (Connect to Server / Save Bookmark), so the
 * shapes locked here match real paste-from-chat inputs.
 *
 * hotline_url.c is pure-GLib; no GTK dependency in the module under
 * test, so this binary links only hotline_url.c + the test source.
 */

#include "config.h"

#include <glib.h>
#include <string.h>

#include "hotline_url.h"

static void
test_host_only (void)
{
    HotlineUrlParts p;
    g_assert_true (hotline_url_parse ("hotline://hotline.example.com", &p));
    g_assert_cmpstr (p.host, ==, "hotline.example.com");
    g_assert_cmpstr (p.login, ==, "");
    g_assert_cmpstr (p.pass, ==, "");
    g_assert_cmpuint (p.port, ==, 0); /* 0 == caller defaults to 5500 */
}

static void
test_host_port (void)
{
    HotlineUrlParts p;
    g_assert_true (hotline_url_parse ("hotline://hl.example.com:6000", &p));
    g_assert_cmpstr (p.host, ==, "hl.example.com");
    g_assert_cmpuint (p.port, ==, 6000);
}

static void
test_login_host (void)
{
    HotlineUrlParts p;
    g_assert_true (hotline_url_parse ("hotline://alice@hl.example.com", &p));
    g_assert_cmpstr (p.host, ==, "hl.example.com");
    g_assert_cmpstr (p.login, ==, "alice");
    g_assert_cmpstr (p.pass, ==, "");
}

static void
test_login_pass_host_port (void)
{
    HotlineUrlParts p;
    g_assert_true (
        hotline_url_parse ("hotline://alice:secret@hl.example.com:5501", &p));
    g_assert_cmpstr (p.host, ==, "hl.example.com");
    g_assert_cmpstr (p.login, ==, "alice");
    g_assert_cmpstr (p.pass, ==, "secret");
    g_assert_cmpuint (p.port, ==, 5501);
}

static void
test_trailing_slash (void)
{
    HotlineUrlParts p;
    g_assert_true (hotline_url_parse ("hotline://hl.example.com/", &p));
    g_assert_cmpstr (p.host, ==, "hl.example.com");
    g_assert_cmpuint (p.port, ==, 0);
}

static void
test_trailing_query (void)
{
    HotlineUrlParts p;
    g_assert_true (
        hotline_url_parse ("hotline://hl.example.com:7000/?ref=foo", &p));
    g_assert_cmpstr (p.host, ==, "hl.example.com");
    g_assert_cmpuint (p.port, ==, 7000);
}

static void
test_url_decoding (void)
{
    /* %20 in login decodes to space; %40 in password is a literal
     * '@' that doesn't re-trigger the userinfo split. */
    HotlineUrlParts p;
    g_assert_true (
        hotline_url_parse ("hotline://alice%20a:p%40ss@hl.example.com", &p));
    g_assert_cmpstr (p.host, ==, "hl.example.com");
    g_assert_cmpstr (p.login, ==, "alice a");
    g_assert_cmpstr (p.pass, ==, "p@ss");
}

static void
test_ipv4_literal (void)
{
    HotlineUrlParts p;
    g_assert_true (hotline_url_parse ("hotline://192.0.2.10:5500", &p));
    g_assert_cmpstr (p.host, ==, "192.0.2.10");
    g_assert_cmpuint (p.port, ==, 5500);
}

static void
test_ipv6_literal (void)
{
    HotlineUrlParts p;
    g_assert_true (hotline_url_parse ("hotline://[2001:db8::1]:5500", &p));
    g_assert_cmpstr (p.host, ==, "2001:db8::1");
    g_assert_cmpuint (p.port, ==, 5500);

    /* Bracketed IPv6 with no port — host only. */
    g_assert_true (hotline_url_parse ("hotline://[2001:db8::1]", &p));
    g_assert_cmpstr (p.host, ==, "2001:db8::1");
    g_assert_cmpuint (p.port, ==, 0);
}

static void
test_rejects_ipv6_trailing_junk (void)
{
    /* "[::1]foo" — anything after the closing bracket that isn't
     * ":port" gets rejected. The earlier permissive form would
     * silently drop the trailing bytes and connect to "::1", which
     * isn't what the user typed. */
    HotlineUrlParts p;
    g_assert_false (hotline_url_parse ("hotline://[2001:db8::1]foo", &p));
    g_assert_false (hotline_url_parse ("hotline://[2001:db8::1]5500", &p));
    /* Unclosed bracket is also rejected. */
    g_assert_false (hotline_url_parse ("hotline://[2001:db8::1", &p));
}

static void
test_rejects_unbracketed_ipv6 (void)
{
    /* "2001:db8::1" has multiple ':' and no brackets — ambiguous
     * between an IPv6 literal and host+port. The last-':' rule
     * would silently misread the IPv6 as host="2001:db8::" port="1".
     * Reject and require brackets for IPv6 literals. */
    HotlineUrlParts p;
    g_assert_false (hotline_url_parse ("hotline://2001:db8::1", &p));
    g_assert_false (hotline_url_parse ("hotline://2001:db8::1:5500", &p));
    /* User-info form with embedded IPv6 — still ambiguous, still
     * rejected. */
    g_assert_false (hotline_url_parse ("hotline://alice@2001:db8::1:5500", &p));
}

static void
test_uppercase_scheme (void)
{
    /* Scheme match is case-insensitive (matches RFC 3986 §3.1). */
    HotlineUrlParts p;
    g_assert_true (hotline_url_parse ("Hotline://hl.example.com", &p));
    g_assert_cmpstr (p.host, ==, "hl.example.com");
}

static void
test_rejects_wrong_scheme (void)
{
    HotlineUrlParts p;
    g_assert_false (hotline_url_parse ("http://example.com", &p));
    g_assert_false (hotline_url_parse ("hotlin://example.com", &p));
    g_assert_false (hotline_url_parse ("hl://example.com", &p));
}

static void
test_rejects_empty_host (void)
{
    HotlineUrlParts p;
    g_assert_false (hotline_url_parse ("hotline://", &p));
    g_assert_false (hotline_url_parse ("hotline://user@", &p));
    g_assert_false (hotline_url_parse ("hotline://:5500", &p));
}

static void
test_rejects_bad_port (void)
{
    /* Bad port silently falls back to 0; the URL still parses
     * because the host is fine. Caller defaults port to 5500. */
    HotlineUrlParts p;
    g_assert_true (hotline_url_parse ("hotline://hl.example.com:abcd", &p));
    g_assert_cmpuint (p.port, ==, 0);

    /* Out-of-range port also falls back to 0. */
    g_assert_true (hotline_url_parse ("hotline://hl.example.com:70000", &p));
    g_assert_cmpuint (p.port, ==, 0);
}

static void
test_password_with_unescaped_at (void)
{
    /* User pastes an unescaped '@' inside the password. We split on
     * the LAST '@', which keeps the host intact and lets the unescaped
     * '@' land in the password. */
    HotlineUrlParts p;
    g_assert_true (
        hotline_url_parse ("hotline://alice:p@ss@hl.example.com:5500", &p));
    g_assert_cmpstr (p.host, ==, "hl.example.com");
    g_assert_cmpstr (p.login, ==, "alice");
    g_assert_cmpstr (p.pass, ==, "p@ss");
    g_assert_cmpuint (p.port, ==, 5500);
}

static void
test_rejects_null_args (void)
{
    HotlineUrlParts p;
    g_assert_false (hotline_url_parse (NULL, &p));
    g_assert_false (hotline_url_parse ("hotline://x", NULL));
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/hotline_url/host_only", test_host_only);
    g_test_add_func ("/hotline_url/host_port", test_host_port);
    g_test_add_func ("/hotline_url/login_host", test_login_host);
    g_test_add_func ("/hotline_url/login_pass_host_port",
                     test_login_pass_host_port);
    g_test_add_func ("/hotline_url/trailing_slash", test_trailing_slash);
    g_test_add_func ("/hotline_url/trailing_query", test_trailing_query);
    g_test_add_func ("/hotline_url/url_decoding", test_url_decoding);
    g_test_add_func ("/hotline_url/ipv4_literal", test_ipv4_literal);
    g_test_add_func ("/hotline_url/ipv6_literal", test_ipv6_literal);
    g_test_add_func ("/hotline_url/rejects_ipv6_trailing_junk",
                     test_rejects_ipv6_trailing_junk);
    g_test_add_func ("/hotline_url/rejects_unbracketed_ipv6",
                     test_rejects_unbracketed_ipv6);
    g_test_add_func ("/hotline_url/uppercase_scheme", test_uppercase_scheme);
    g_test_add_func ("/hotline_url/rejects_wrong_scheme",
                     test_rejects_wrong_scheme);
    g_test_add_func ("/hotline_url/rejects_empty_host",
                     test_rejects_empty_host);
    g_test_add_func ("/hotline_url/rejects_bad_port", test_rejects_bad_port);
    g_test_add_func ("/hotline_url/password_with_unescaped_at",
                     test_password_with_unescaped_at);
    g_test_add_func ("/hotline_url/rejects_null_args", test_rejects_null_args);

    return g_test_run ();
}
