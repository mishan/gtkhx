/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/proto/test_tracker_parser.c — Tier 2 coverage for the
 * pure HTRK reply parsers carved out of network.c into
 * src/tracker_parser.c.
 *
 * The async tracker fetch state machine (network.c on_tracker
 * _*_read callbacks) used to do the byte-extraction inline.
 * Splitting the parse out lets us pin the wire format with
 * fixed-input KAT vectors instead of waiting on a live tracker
 * server to find a regression.
 *
 * Four contracts under test:
 *
 *   hx_tracker_reply_parse_header — nservers at offset [10..11]
 *       (u16 BE) out of the 14-byte response header. Everything
 *       else in the header is opaque to the client.
 *
 *   hx_tracker_record_is_padding — the all-zero record sentinel
 *       (first byte == 0 → not a real server, skip without
 *       advancing the counter). The async fetch uses this to
 *       walk past padding the tracker emits between real
 *       records.
 *
 *   hx_tracker_record_parse_fixed — the 11-byte fixed prefix of
 *       a server record: addr[0..3] / port[4..5] / nusers[6..7]
 *       / reserved[8..9] / name_len[10]. Pin both byte ordering
 *       (big-endian on the wire, host-order out) and the
 *       skipping of the reserved bytes.
 *
 *   hx_tracker_normalize_text — CR2LF + strip_ansi on the
 *       server name + description. Belt-and-suspenders test so
 *       a future "simplification" of the macros doesn't change
 *       what the tracker window renders. (strip_ansi REMAPS
 *       low control bytes to printable ASCII via (c & 127) | 64;
 *       buffer length is unchanged.)
 */

#include "config.h"

#include <string.h>
#include <glib.h>
#include "tracker_parser.h"

/* ---- reply header --------------------------------------------- */

static void
test_reply_header_basic (void)
{
    /* 14-byte response header with nservers=7 at [10..11] BE. */
    guint8 buf[14] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x07, 0, 0 };
    guint16 n = 0xffff;
    g_assert_true (hx_tracker_reply_parse_header (buf, sizeof (buf), &n));
    g_assert_cmpuint (n, ==, 7);
}

static void
test_reply_header_large_count (void)
{
    /* The tracker reply count is a u16, so the maximum value is
     * 65535. Pin both ends of the range. */
    guint8 buf[14]
        = { /* opaque [0..9] */
            0xaa, 0xbb, 0xcc, 0xdd, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
            /* nservers [10..11] = 0xffff */
            0xff, 0xff,
            /* opaque [12..13] */
            0x99, 0x88
          };
    guint16 n = 0;
    g_assert_true (hx_tracker_reply_parse_header (buf, sizeof (buf), &n));
    g_assert_cmpuint (n, ==, 0xffff);
}

static void
test_reply_header_short_input (void)
{
    /* Less than 14 bytes is rejected — the caller in network.c
     * already gates on read length, but pinning the defensive
     * check here means the helper is safe in isolation. */
    guint8 buf[13] = { 0 };
    guint16 n = 0xdead;
    g_assert_false (hx_tracker_reply_parse_header (buf, sizeof (buf), &n));
    g_assert_cmpuint (n, ==, 0xdead); /* untouched on failure */
}

static void
test_reply_header_null_inputs (void)
{
    guint16 n = 42;
    g_assert_false (hx_tracker_reply_parse_header (NULL, 14, &n));
    {
        guint8 buf[14] = { 0 };
        g_assert_false (hx_tracker_reply_parse_header (buf, 14, NULL));
    }
}

/* ---- padding sentinel ----------------------------------------- */

static void
test_padding_first_byte_zero (void)
{
    /* The tracker pads between real records with all-zero bytes;
     * the leading byte (high octet of IPv4 addr) being 0 is the
     * sentinel. */
    guint8 buf[1] = { 0 };
    g_assert_true (hx_tracker_record_is_padding (buf, sizeof (buf)));
}

static void
test_padding_first_byte_nonzero (void)
{
    /* Any nonzero first byte means a real record. Try a couple
     * of common IP prefixes (1.x, 10.x, 192.x, 255.x) to make
     * sure the check doesn't accidentally narrow. */
    {
        guint8 buf[1] = { 1 };
        g_assert_false (hx_tracker_record_is_padding (buf, 1));
    }
    {
        guint8 buf[1] = { 10 };
        g_assert_false (hx_tracker_record_is_padding (buf, 1));
    }
    {
        guint8 buf[1] = { 192 };
        g_assert_false (hx_tracker_record_is_padding (buf, 1));
    }
    {
        guint8 buf[1] = { 255 };
        g_assert_false (hx_tracker_record_is_padding (buf, 1));
    }
}

static void
test_padding_short_input (void)
{
    /* Zero-length / NULL input returns FALSE (defensive — caller
     * has the byte to check, but we don't want to falsely report
     * "padding" if it doesn't). */
    g_assert_false (hx_tracker_record_is_padding (NULL, 1));
    {
        guint8 buf[1] = { 0 };
        g_assert_false (hx_tracker_record_is_padding (buf, 0));
    }
}

/* ---- fixed-record parse --------------------------------------- */

static void
test_record_parse_basic (void)
{
    /* 11-byte fixed prefix: 192.168.1.42 : 5500, 17 users,
     * reserved=0xabcd, name_len=23. */
    guint8 buf[11] = {
        192,  168,  1, 42, /* addr */
        0x15, 0x7c,        /* port = 5500 (0x157c) */
        0x00, 0x11,        /* nusers = 17 */
        0xab, 0xcd,        /* reserved (skipped) */
        23                 /* name_len */
    };
    hx_tracker_record_fixed rec = { 0 };
    g_assert_true (hx_tracker_record_parse_fixed (buf, sizeof (buf), &rec));

    /* addr stays in network byte order. 192.168.1.42 is 0xC0A8012A in
     * host order; g_htonl gives the network-order value the parser stores. */
    g_assert_cmpuint (rec.addr, ==,
                      g_htonl ((192u << 24) | (168u << 16) | (1u << 8) | 42u));
    g_assert_cmpuint (rec.port, ==, 5500);
    g_assert_cmpuint (rec.nusers, ==, 17);
    g_assert_cmpuint (rec.name_len, ==, 23);
}

static void
test_record_parse_max_values (void)
{
    /* 255.255.255.255 / port 65535 / 65535 users / name_len=255.
     * Pins that there's no signed-overflow lurking in the
     * accessors and that name_len=255 (the max u8) is accepted
     * (production reads 255 name bytes afterward). */
    guint8 buf[11]
        = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xaa, 0xbb, 0xff };
    hx_tracker_record_fixed rec = { 0 };
    g_assert_true (hx_tracker_record_parse_fixed (buf, 11, &rec));
    g_assert_cmpuint (rec.addr, ==, 0xffffffffu);
    g_assert_cmpuint (rec.port, ==, 0xffff);
    g_assert_cmpuint (rec.nusers, ==, 0xffff);
    g_assert_cmpuint (rec.name_len, ==, 0xff);
}

static void
test_record_parse_reserved_bytes_ignored (void)
{
    /* Whatever bytes [8..9] hold MUST NOT affect any parsed
     * field. Build two records that differ only in the reserved
     * bytes and verify the parser returns the same struct
     * contents. Cheap insurance for a future spec change that
     * tries to repurpose those bytes — the test will fire and
     * force a conscious decision instead of silently misparsing. */
    guint8 buf_a[11] = { 10, 0, 0, 1, 0x13, 0x88, 0, 5, 0x00, 0x00, 4 };
    guint8 buf_b[11] = { 10, 0, 0, 1, 0x13, 0x88, 0, 5, 0xde, 0xad, 4 };
    hx_tracker_record_fixed a = { 0 };
    hx_tracker_record_fixed b = { 0 };
    g_assert_true (hx_tracker_record_parse_fixed (buf_a, 11, &a));
    g_assert_true (hx_tracker_record_parse_fixed (buf_b, 11, &b));
    g_assert_cmpuint (a.addr, ==, b.addr);
    g_assert_cmpuint (a.port, ==, b.port);
    g_assert_cmpuint (a.nusers, ==, b.nusers);
    g_assert_cmpuint (a.name_len, ==, b.name_len);
}

static void
test_record_parse_short_input (void)
{
    guint8 buf[10] = { 0 };
    hx_tracker_record_fixed rec = { 0xdeadbeef, 9, 9, 9 };
    g_assert_false (hx_tracker_record_parse_fixed (buf, 10, &rec));
    /* On failure the out struct is left as-is. */
    g_assert_cmpuint (rec.port, ==, 9);
    g_assert_cmpuint (rec.nusers, ==, 9);
    g_assert_cmpuint (rec.name_len, ==, 9);
}

static void
test_record_parse_null_inputs (void)
{
    hx_tracker_record_fixed rec = { 0 };
    g_assert_false (hx_tracker_record_parse_fixed (NULL, 11, &rec));
    {
        guint8 buf[11] = { 0 };
        g_assert_false (hx_tracker_record_parse_fixed (buf, 11, NULL));
    }
}

/* ---- text normalization --------------------------------------- */

static void
test_normalize_cr_to_lf (void)
{
    /* Pre-OS X line endings (raw CR) become LF in place.
     * Production's downstream consumers (the tracker list
     * widget) want one-line descriptions, so any \r becomes
     * \n which the widget then renders as a space-equivalent. */
    char buf[] = "first\rsecond\rthird";
    hx_tracker_normalize_text (buf, sizeof (buf) - 1);
    g_assert_cmpstr (buf, ==, "first\nsecond\nthird");
}

static void
test_normalize_strip_ansi (void)
{
    /* strip_ansi (protocol.h) remaps low control bytes — including
     * ESC (0x1b) — to printable ASCII via `(c & 127) | 64`, so
     * 0x1b becomes '[' (0x5b). The buffer length is unchanged;
     * what we're pinning here is that the post-call buffer
     * contains no remaining ESC bytes, so the tracker widget
     * never has to render raw escape gunk in server
     * descriptions. */
    char buf[64] = { 0 };
    g_snprintf (buf, sizeof (buf), "\x1b[31mred\x1b[0m text");
    gsize n = strlen (buf);
    hx_tracker_normalize_text (buf, n);
    g_assert_null (strchr (buf, 0x1b));
}

static void
test_normalize_empty (void)
{
    /* Defensive: zero-length input is a no-op. NULL is also a
     * no-op. Both fire from production when the server sends a
     * record with an empty name or description. */
    char buf[1] = { 0 };
    hx_tracker_normalize_text (buf, 0);
    g_assert_cmpuint (buf[0], ==, 0);
    hx_tracker_normalize_text (NULL, 0);
    hx_tracker_normalize_text (NULL, 32); /* NULL guard wins */
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/tracker_parser/reply_header/basic",
                     test_reply_header_basic);
    g_test_add_func ("/tracker_parser/reply_header/large_count",
                     test_reply_header_large_count);
    g_test_add_func ("/tracker_parser/reply_header/short_input",
                     test_reply_header_short_input);
    g_test_add_func ("/tracker_parser/reply_header/null_inputs",
                     test_reply_header_null_inputs);

    g_test_add_func ("/tracker_parser/padding/first_byte_zero",
                     test_padding_first_byte_zero);
    g_test_add_func ("/tracker_parser/padding/first_byte_nonzero",
                     test_padding_first_byte_nonzero);
    g_test_add_func ("/tracker_parser/padding/short_input",
                     test_padding_short_input);

    g_test_add_func ("/tracker_parser/record_fixed/basic",
                     test_record_parse_basic);
    g_test_add_func ("/tracker_parser/record_fixed/max_values",
                     test_record_parse_max_values);
    g_test_add_func ("/tracker_parser/record_fixed/reserved_bytes_ignored",
                     test_record_parse_reserved_bytes_ignored);
    g_test_add_func ("/tracker_parser/record_fixed/short_input",
                     test_record_parse_short_input);
    g_test_add_func ("/tracker_parser/record_fixed/null_inputs",
                     test_record_parse_null_inputs);

    g_test_add_func ("/tracker_parser/normalize/cr_to_lf",
                     test_normalize_cr_to_lf);
    g_test_add_func ("/tracker_parser/normalize/strip_ansi",
                     test_normalize_strip_ansi);
    g_test_add_func ("/tracker_parser/normalize/empty", test_normalize_empty);

    return g_test_run ();
}
