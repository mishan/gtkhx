/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/proto/test_tracker_v3.c — Tier 2 coverage for the pure
 * tracker v3 encoders / parsers in src/tracker_v3.c.
 *
 * The async tracker-fetch state machine in network.c reads bytes
 * off a GIOStream and hands them to these helpers; splitting the
 * helpers out means we pin the wire format against fixed KAT
 * vectors here without needing a live (or mock) v3 tracker to
 * catch regressions.
 *
 * What's pinned:
 *
 *   Handshake encoder       — 8 bytes: "HTRK" + 0x0003 + features.
 *   Handshake response      — 6-byte v1/v2 reply (features stays 0)
 *                             vs. 8-byte v3 reply.
 *   Listing-request encoder — 4-byte minimum-viable request.
 *   Response-header parser  — 10 bytes; response_type/total_size/
 *                             total_servers/record_count layout.
 *   Server-record parser    — variable-length record, all three
 *                             address-type bytes (0x04/0x06/0x48),
 *                             TLV trailer walked, truncation
 *                             rejected at every cursor step.
 *   TLV walker              — happy path with mixed-id payloads;
 *                             truncation rejected.
 *   Two-back-to-back        — consumed_out correctly advances the
 *                             cursor across multiple records in a
 *                             single buffer (mirrors what the
 *                             state machine does walking the
 *                             response payload).
 */

#include "config.h"

#include <string.h>
#include <glib.h>
#include <gio/gio.h>
#include "compat.h"  /* PACKED — used by structs in hotline.h */
#include "hotline.h"
#include "tracker_v3.h"

/* ---------- helpers used across tests ---------- */

/* Append `n` bytes of `buf` to `dst`. Returns the new length. */
static gsize
buf_append (GByteArray *dst, const void *src, gsize n)
{
    g_byte_array_append (dst, src, n);
    return dst->len;
}

static void
buf_append_u16_be (GByteArray *dst, guint16 v)
{
    guint8 b[2] = { (guint8)(v >> 8), (guint8)(v & 0xff) };
    g_byte_array_append (dst, b, 2);
}

/* ---------- handshake encoder ---------- */

static void
test_handshake_pack (void)
{
    /* Feature bits = IPV6 | QUERY = 0x0003. Pin the byte layout:
     *   "HTRK" | 00 03 | 00 03
     * Two distinct 0x0003 values back-to-back here are deliberate
     * — version field and feature bitmask happen to share that
     * value. Easy to mis-read so the explicit annotation in the
     * test helps the next reader. */
    guint8 buf[8];
    const guint8 expect[8] = { 'H', 'T', 'R', 'K',
                               0x00, 0x03,      /* version */
                               0x00, 0x03 };    /* features */
    g_assert_true (
        hx_tracker_v3_pack_handshake (buf, sizeof (buf),
                                      HTRK_V3_FEAT_IPV6 | HTRK_V3_FEAT_QUERY));
    g_assert_cmpmem (buf, 8, expect, 8);
}

static void
test_handshake_pack_no_features (void)
{
    /* features == 0 is the spec-minimum (no capabilities offered).
     * Make sure the encoder doesn't decide on a default value. */
    guint8 buf[8];
    const guint8 expect[8] = { 'H', 'T', 'R', 'K',
                               0x00, 0x03, 0x00, 0x00 };
    g_assert_true (hx_tracker_v3_pack_handshake (buf, sizeof (buf), 0));
    g_assert_cmpmem (buf, 8, expect, 8);
}

static void
test_handshake_pack_short_buf (void)
{
    guint8 buf[7];
    g_assert_false (hx_tracker_v3_pack_handshake (buf, sizeof (buf), 0));
    g_assert_false (hx_tracker_v3_pack_handshake (NULL, 8, 0));
}

/* ---------- handshake response parser ---------- */

static void
test_handshake_response_v3 (void)
{
    /* Tracker speaks v3, offers IPV6 + REG_ACK = 0x0009. */
    guint8 buf[8] = {
        'H', 'T', 'R', 'K',
        0x00, 0x03,
        0x00, 0x09
    };
    guint16 ver = 0, feat = 0;
    g_assert_true (
        hx_tracker_v3_parse_handshake_response (buf, sizeof (buf), &ver, &feat));
    g_assert_cmpuint (ver, ==, HTRK_VERSION_V3);
    g_assert_cmpuint (feat, ==, (HTRK_V3_FEAT_IPV6 | HTRK_V3_FEAT_REG_ACK));
}

static void
test_handshake_response_v1_fallback (void)
{
    /* Tracker speaks v1: 6 bytes, no feature flags. The state
     * machine sees a 6-byte reply and feeds those to us; we
     * confirm version and leave features at zero. */
    guint8 buf[6] = {
        'H', 'T', 'R', 'K',
        0x00, 0x01
    };
    guint16 ver = 0xdead, feat = 0xbeef;
    g_assert_true (
        hx_tracker_v3_parse_handshake_response (buf, sizeof (buf), &ver, &feat));
    g_assert_cmpuint (ver, ==, HTRK_VERSION_V1);
    g_assert_cmpuint (feat, ==, 0);
}

static void
test_handshake_response_v2 (void)
{
    /* v2 trackers exist in spec land (HotlineX added an auth
     * challenge on top of v1's wire). Pin that v2 maps to "fall
     * back to v1-shaped read" — features still zero. */
    guint8 buf[6] = {
        'H', 'T', 'R', 'K',
        0x00, 0x02
    };
    guint16 ver = 0, feat = 0xabcd;
    g_assert_true (
        hx_tracker_v3_parse_handshake_response (buf, sizeof (buf), &ver, &feat));
    g_assert_cmpuint (ver, ==, HTRK_VERSION_V2);
    g_assert_cmpuint (feat, ==, 0);
}

static void
test_handshake_response_bad_magic (void)
{
    /* Wrong magic — connect to something that isn't a tracker. */
    guint8 buf[8] = { 'X', 'X', 'X', 'X', 0x00, 0x03, 0, 0 };
    guint16 ver = 0, feat = 0;
    g_assert_false (
        hx_tracker_v3_parse_handshake_response (buf, sizeof (buf), &ver, &feat));
}

static void
test_handshake_response_bad_length (void)
{
    /* The parser accepts only 6 or 8 byte inputs — anything else
     * is a state-machine bug. */
    guint8 buf[7] = { 'H', 'T', 'R', 'K', 0x00, 0x03, 0x00 };
    guint16 ver = 0, feat = 0;
    g_assert_false (
        hx_tracker_v3_parse_handshake_response (buf, sizeof (buf), &ver, &feat));
}

/* ---------- listing request encoder ---------- */

static void
test_listing_request_simple (void)
{
    /* The minimum request: request_type=0x0001 + field_count=0.
     * 4 bytes total. */
    guint8 buf[8] = { 0 };
    gsize n = 0;
    const guint8 expect[4] = { 0x00, 0x01, 0x00, 0x00 };
    g_assert_true (
        hx_tracker_v3_pack_listing_request_simple (buf, sizeof (buf), &n));
    g_assert_cmpuint (n, ==, 4);
    g_assert_cmpmem (buf, 4, expect, 4);
}

static void
test_listing_request_short_buf (void)
{
    guint8 buf[3];
    gsize n = 0;
    g_assert_false (
        hx_tracker_v3_pack_listing_request_simple (buf, sizeof (buf), &n));
}

/* ---------- response header parser ---------- */

static void
test_response_header_basic (void)
{
    /* response_type=1, total_size=0x12345678, total_servers=42,
     * record_count=42. */
    guint8 buf[10] = {
        0x00, 0x01,                      /* response_type */
        0x12, 0x34, 0x56, 0x78,          /* total_size u32 */
        0x00, 0x2a,                      /* total_servers */
        0x00, 0x2a                       /* record_count */
    };
    guint16 type = 0, total_s = 0, rec = 0;
    guint32 total_b = 0;
    g_assert_true (hx_tracker_v3_parse_response_header (
        buf, sizeof (buf), &type, &total_b, &total_s, &rec));
    g_assert_cmpuint (type, ==, HTRK_V3_RESP_LIST);
    g_assert_cmpuint (total_b, ==, 0x12345678);
    g_assert_cmpuint (total_s, ==, 42);
    g_assert_cmpuint (rec, ==, 42);
}

static void
test_response_header_pagination (void)
{
    /* Pagination case: total_servers reflects the full match
     * count, record_count just this page. */
    guint8 buf[10] = {
        0x00, 0x01,
        0x00, 0x00, 0x10, 0x00,          /* 4096 bytes in this batch */
        0x03, 0xe8,                      /* total 1000 */
        0x00, 0x32                       /* this page = 50 */
    };
    guint16 type, total_s, rec;
    guint32 total_b;
    g_assert_true (hx_tracker_v3_parse_response_header (
        buf, sizeof (buf), &type, &total_b, &total_s, &rec));
    g_assert_cmpuint (total_s, ==, 1000);
    g_assert_cmpuint (rec, ==, 50);
}

static void
test_response_header_short_input (void)
{
    guint8 buf[9] = { 0 };
    guint16 type, total_s, rec;
    guint32 total_b;
    g_assert_false (hx_tracker_v3_parse_response_header (
        buf, sizeof (buf), &type, &total_b, &total_s, &rec));
}

static void
test_response_header_wrong_type (void)
{
    /* A non-listing response type should be rejected — the state
     * machine bails on this because it has no other RPC defined
     * for the listing connection. */
    guint8 buf[10] = {
        0x00, 0x02,                      /* not RESP_LIST */
        0, 0, 0, 0, 0, 0, 0, 0
    };
    guint16 type, total_s, rec;
    guint32 total_b;
    g_assert_false (hx_tracker_v3_parse_response_header (
        buf, sizeof (buf), &type, &total_b, &total_s, &rec));
}

/* ---------- server record parser ---------- */

/* Build a synthetic IPv4 record with three TLVs: SERVER_SOFTWARE
 * (string), PROTOCOL_VERSION (u16), SUPPORTS_HOPE (bool/u8). */
/* "203.0.113.42" -> the 4-byte IPv4 as a network-byte-order guint32,
 * via GInetAddress (portable — no POSIX inet_aton). */
static guint32
ipv4_be (const char *dotted)
{
    GInetAddress *ia = g_inet_address_new_from_string (dotted);
    g_assert_nonnull (ia);
    guint32 out = 0;
    memcpy (&out, g_inet_address_to_bytes (ia), 4);
    g_object_unref (ia);
    return out;
}

static GByteArray *
build_ipv4_record (guint32 addr, guint16 port, guint16 nusers,
                   const char *name, const char *desc)
{
    GByteArray *r = g_byte_array_new ();
    guint8 type = HTRK_V3_ADDR_IPV4;
    g_byte_array_append (r, &type, 1);
    g_byte_array_append (r, (const guint8 *)&addr, 4);
    buf_append_u16_be (r, port);
    buf_append_u16_be (r, nusers);
    gsize nl = strlen (name);
    buf_append_u16_be (r, (guint16) nl);
    g_byte_array_append (r, (const guint8 *)name, nl);
    gsize dl = strlen (desc);
    buf_append_u16_be (r, (guint16) dl);
    g_byte_array_append (r, (const guint8 *)desc, dl);
    /* TLVs: 3 entries. */
    buf_append_u16_be (r, 3);
    /* SERVER_SOFTWARE = "hxd/2.0" */
    buf_append_u16_be (r, HTRK_V3_TLV_SERVER_SOFTWARE);
    buf_append_u16_be (r, 7);
    g_byte_array_append (r, (const guint8 *)"hxd/2.0", 7);
    /* PROTOCOL_VERSION = 0x00be (190) */
    buf_append_u16_be (r, HTRK_V3_TLV_PROTOCOL_VERSION);
    buf_append_u16_be (r, 2);
    buf_append_u16_be (r, 0x00be);
    /* SUPPORTS_HOPE = 1 (u8) */
    buf_append_u16_be (r, HTRK_V3_TLV_SUPPORTS_HOPE);
    buf_append_u16_be (r, 1);
    guint8 one = 1;
    g_byte_array_append (r, &one, 1);
    return r;
}

static void
test_record_ipv4_basic (void)
{
    guint32 a = ipv4_be ("203.0.113.42");
    GByteArray *r = build_ipv4_record (a, 5500, 17, "Retro Hub",
                                       "Vintage Mac hangout");

    hx_tracker_v3_record rec = { 0 };
    gsize consumed = 0;
    g_assert_true (
        hx_tracker_v3_parse_record (r->data, r->len, &rec, &consumed));
    g_assert_cmpuint (consumed, ==, r->len);

    /* Fixed header. */
    g_assert_cmpuint (rec.addr_type, ==, HTRK_V3_ADDR_IPV4);
    g_assert_cmpuint (rec.address_len, ==, 4);
    /* Address bytes preserved in network byte order; reassemble
     * back into a guint32 to verify the round-trip without endian
     * pitfalls. */
    {
        guint32 roundtrip = 0;
        memcpy (&roundtrip, rec.address, 4);
        g_assert_cmpuint (roundtrip, ==, a);
    }
    g_assert_cmpuint (rec.port, ==, 5500);
    g_assert_cmpuint (rec.nusers, ==, 17);

    /* Borrowed name / desc slices. */
    g_assert_cmpuint (rec.name_len, ==, strlen ("Retro Hub"));
    g_assert_cmpmem (rec.name, rec.name_len, "Retro Hub", strlen ("Retro Hub"));
    g_assert_cmpuint (rec.desc_len, ==, strlen ("Vintage Mac hangout"));
    g_assert_cmpmem (rec.desc, rec.desc_len, "Vintage Mac hangout",
                     strlen ("Vintage Mac hangout"));

    g_assert_cmpuint (rec.tlv_count, ==, 3);
    /* TLV-bytes slice is non-empty when count>0. */
    g_assert_cmpuint (rec.tlv_bytes_len, >, 0);

    g_byte_array_free (r, TRUE);
}

static void
test_record_ipv6 (void)
{
    /* 2001:db8::1 → 20 01 0d b8 00 ... 01. We construct the
     * address bytes directly rather than depending on inet_pton
     * to avoid platform variance in the test. */
    static const guint8 v6_addr[16] = {
        0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0x01
    };
    GByteArray *r = g_byte_array_new ();
    guint8 type = HTRK_V3_ADDR_IPV6;
    g_byte_array_append (r, &type, 1);
    g_byte_array_append (r, v6_addr, 16);
    buf_append_u16_be (r, 5500);
    buf_append_u16_be (r, 0);
    buf_append_u16_be (r, 5);
    g_byte_array_append (r, (const guint8 *)"hello", 5);
    buf_append_u16_be (r, 0);    /* zero-length desc */
    buf_append_u16_be (r, 0);    /* zero TLVs */

    hx_tracker_v3_record rec = { 0 };
    gsize consumed = 0;
    g_assert_true (
        hx_tracker_v3_parse_record (r->data, r->len, &rec, &consumed));
    g_assert_cmpuint (consumed, ==, r->len);

    g_assert_cmpuint (rec.addr_type, ==, HTRK_V3_ADDR_IPV6);
    g_assert_cmpuint (rec.address_len, ==, 16);
    g_assert_cmpmem (rec.address, 16, v6_addr, 16);
    g_assert_cmpuint (rec.desc_len, ==, 0);
    g_assert_cmpuint (rec.tlv_count, ==, 0);

    g_byte_array_free (r, TRUE);
}

static void
test_record_hostname (void)
{
    /* Hostname address type uses a u16-prefixed UTF-8 string in
     * place of the fixed-size IP bytes. Pin both the length-prefix
     * shape and the borrowed-slice semantics — address_len reports
     * the string length, NOT including the prefix. */
    const char *host = "tracker.example.com";
    GByteArray *r = g_byte_array_new ();
    guint8 type = HTRK_V3_ADDR_HOSTNAME;
    g_byte_array_append (r, &type, 1);
    buf_append_u16_be (r, (guint16) strlen (host));
    g_byte_array_append (r, (const guint8 *)host, strlen (host));
    buf_append_u16_be (r, 5500);
    buf_append_u16_be (r, 0);
    buf_append_u16_be (r, 7);
    g_byte_array_append (r, (const guint8 *)"by-host", 7);
    buf_append_u16_be (r, 0);
    buf_append_u16_be (r, 0);

    hx_tracker_v3_record rec = { 0 };
    gsize consumed = 0;
    g_assert_true (
        hx_tracker_v3_parse_record (r->data, r->len, &rec, &consumed));
    g_assert_cmpuint (consumed, ==, r->len);

    g_assert_cmpuint (rec.addr_type, ==, HTRK_V3_ADDR_HOSTNAME);
    g_assert_cmpuint (rec.address_len, ==, strlen (host));
    g_assert_cmpmem (rec.address, rec.address_len, host, strlen (host));

    g_byte_array_free (r, TRUE);
}

static void
test_record_unknown_addr_type (void)
{
    /* A future spec rev that adds a new type byte arrives at our
     * parser. We can't tell how many bytes follow, so the only
     * safe move is to fail and let the state machine close the
     * connection. */
    guint8 buf[32] = { 0xff, 0, 0, 0, 0 };
    hx_tracker_v3_record rec = { 0 };
    gsize consumed = 0;
    g_assert_false (
        hx_tracker_v3_parse_record (buf, sizeof (buf), &rec, &consumed));
}

static void
test_record_truncated_after_addr (void)
{
    /* IPv4 record but with only the type byte + 3 of 4 address
     * bytes present. Address-bounds check must fail. */
    guint8 buf[4] = { HTRK_V3_ADDR_IPV4, 192, 168, 1 };
    hx_tracker_v3_record rec = { 0 };
    gsize consumed = 99;
    g_assert_false (
        hx_tracker_v3_parse_record (buf, sizeof (buf), &rec, &consumed));
    /* consumed left as caller set it on failure — defensive but not
     * a hard contract. */
}

static void
test_record_truncated_in_name (void)
{
    /* Claims name_len=10, only delivers 3 bytes of name. */
    GByteArray *r = g_byte_array_new ();
    guint8 type = HTRK_V3_ADDR_IPV4;
    g_byte_array_append (r, &type, 1);
    {
        guint8 addr[4] = { 10, 0, 0, 1 };
        g_byte_array_append (r, addr, 4);
    }
    buf_append_u16_be (r, 5500);
    buf_append_u16_be (r, 0);
    buf_append_u16_be (r, 10);       /* lies — only 3 bytes follow */
    g_byte_array_append (r, (const guint8 *)"abc", 3);

    hx_tracker_v3_record rec = { 0 };
    gsize consumed = 0;
    g_assert_false (
        hx_tracker_v3_parse_record (r->data, r->len, &rec, &consumed));

    g_byte_array_free (r, TRUE);
}

static void
test_record_truncated_in_tlv (void)
{
    /* Server record with a TLV that claims value_len=20 but only
     * 5 bytes are left in the buffer. */
    GByteArray *r = g_byte_array_new ();
    guint8 type = HTRK_V3_ADDR_IPV4;
    g_byte_array_append (r, &type, 1);
    {
        guint8 addr[4] = { 10, 0, 0, 1 };
        g_byte_array_append (r, addr, 4);
    }
    buf_append_u16_be (r, 5500);
    buf_append_u16_be (r, 0);
    buf_append_u16_be (r, 0); /* zero-length name */
    buf_append_u16_be (r, 0); /* zero-length desc */
    buf_append_u16_be (r, 1); /* one TLV */
    buf_append_u16_be (r, HTRK_V3_TLV_TAGS);
    buf_append_u16_be (r, 20); /* lies — only 5 bytes available */
    g_byte_array_append (r, (const guint8 *)"abcde", 5);

    hx_tracker_v3_record rec = { 0 };
    gsize consumed = 0;
    g_assert_false (
        hx_tracker_v3_parse_record (r->data, r->len, &rec, &consumed));

    g_byte_array_free (r, TRUE);
}

static void
test_record_two_back_to_back (void)
{
    /* The state machine reads `total_size` bytes from the wire,
     * then walks records via repeated parse_record calls,
     * advancing by `consumed` each iteration. Pin that contract
     * by stuffing two records into one buffer and verifying both
     * decode + consumed accumulates correctly. */
    guint32 a = ipv4_be ("192.0.2.10");
    guint32 b = ipv4_be ("198.51.100.20");
    GByteArray *r1 = build_ipv4_record (a, 5500, 3, "alpha", "first");
    GByteArray *r2 = build_ipv4_record (b, 5500, 7, "beta", "second");
    GByteArray *cat = g_byte_array_new ();
    buf_append (cat, r1->data, r1->len);
    buf_append (cat, r2->data, r2->len);

    hx_tracker_v3_record rec = { 0 };
    gsize pos = 0, consumed = 0;

    g_assert_true (hx_tracker_v3_parse_record (
        cat->data + pos, cat->len - pos, &rec, &consumed));
    g_assert_cmpuint (rec.port, ==, 5500);
    g_assert_cmpuint (rec.nusers, ==, 3);
    g_assert_cmpmem (rec.name, rec.name_len, "alpha", 5);
    pos += consumed;

    g_assert_cmpuint (pos, ==, r1->len);

    g_assert_true (hx_tracker_v3_parse_record (
        cat->data + pos, cat->len - pos, &rec, &consumed));
    g_assert_cmpuint (rec.nusers, ==, 7);
    g_assert_cmpmem (rec.name, rec.name_len, "beta", 4);
    pos += consumed;

    g_assert_cmpuint (pos, ==, cat->len);

    g_byte_array_free (r1, TRUE);
    g_byte_array_free (r2, TRUE);
    g_byte_array_free (cat, TRUE);
}

/* ---------- TLV walker ---------- */

struct tlv_collect {
    guint count;
    guint16 last_id;
    guint16 last_len;
    guint8 last_first_byte;
};

static gboolean
tlv_collect_cb (guint16 id, guint16 value_len, const guint8 *value,
                gpointer user_data)
{
    struct tlv_collect *c = user_data;
    c->count++;
    c->last_id = id;
    c->last_len = value_len;
    c->last_first_byte = value_len ? value[0] : 0;
    return TRUE;
}

static void
test_walk_tlvs_basic (void)
{
    /* Build a TLV blob of three entries — string, u16, u8 — and
     * verify the callback sees each one. */
    GByteArray *blob = g_byte_array_new ();
    buf_append_u16_be (blob, HTRK_V3_TLV_SERVER_SOFTWARE);
    buf_append_u16_be (blob, 5);
    g_byte_array_append (blob, (const guint8 *)"hxd99", 5);
    buf_append_u16_be (blob, HTRK_V3_TLV_PROTOCOL_VERSION);
    buf_append_u16_be (blob, 2);
    buf_append_u16_be (blob, 0x00c8);
    buf_append_u16_be (blob, HTRK_V3_TLV_MATURITY);
    buf_append_u16_be (blob, 1);
    {
        guint8 m = 2;
        g_byte_array_append (blob, &m, 1);
    }

    struct tlv_collect c = { 0, 0, 0, 0 };
    g_assert_true (hx_tracker_v3_walk_tlvs (blob->data, blob->len, 3,
                                            tlv_collect_cb, &c));
    g_assert_cmpuint (c.count, ==, 3);
    g_assert_cmpuint (c.last_id, ==, HTRK_V3_TLV_MATURITY);
    g_assert_cmpuint (c.last_len, ==, 1);
    g_assert_cmpuint (c.last_first_byte, ==, 2);

    g_byte_array_free (blob, TRUE);
}

static void
test_walk_tlvs_truncated (void)
{
    /* TLV header (4 bytes) says value_len=10 but only 2 bytes follow. */
    GByteArray *blob = g_byte_array_new ();
    buf_append_u16_be (blob, HTRK_V3_TLV_TAGS);
    buf_append_u16_be (blob, 10);
    g_byte_array_append (blob, (const guint8 *)"ab", 2);

    struct tlv_collect c = { 0, 0, 0, 0 };
    g_assert_false (hx_tracker_v3_walk_tlvs (blob->data, blob->len, 1,
                                             tlv_collect_cb, &c));

    g_byte_array_free (blob, TRUE);
}

static gboolean
tlv_stop_after_first (guint16 id, guint16 value_len, const guint8 *value,
                      gpointer user_data)
{
    (void)id;
    (void)value_len;
    (void)value;
    int *n = user_data;
    (*n)++;
    return FALSE;            /* stop walking */
}

static void
test_walk_tlvs_early_stop (void)
{
    /* Callback returning FALSE is "stop, but not a failure". The
     * walker stops immediately and reports success. */
    GByteArray *blob = g_byte_array_new ();
    buf_append_u16_be (blob, HTRK_V3_TLV_TAGS);
    buf_append_u16_be (blob, 3);
    g_byte_array_append (blob, (const guint8 *)"abc", 3);
    buf_append_u16_be (blob, HTRK_V3_TLV_TAGS);
    buf_append_u16_be (blob, 3);
    g_byte_array_append (blob, (const guint8 *)"def", 3);

    int seen = 0;
    g_assert_true (hx_tracker_v3_walk_tlvs (blob->data, blob->len, 2,
                                            tlv_stop_after_first, &seen));
    g_assert_cmpint (seen, ==, 1);

    g_byte_array_free (blob, TRUE);
}

static void
test_walk_tlvs_leftover_bytes (void)
{
    /* Strict contract from the header doc: walking `count` TLVs
     * must leave the cursor exactly at buf_len; trailing bytes
     * past the declared TLVs are a malformed input. Pin the
     * contract here so the record parser's "slice exactly to
     * the TLV section" invariant has a regression net — and
     * direct callers of the walker can't silently accept
     * malformed blobs. */
    GByteArray *blob = g_byte_array_new ();
    buf_append_u16_be (blob, HTRK_V3_TLV_TAGS);
    buf_append_u16_be (blob, 3);
    g_byte_array_append (blob, (const guint8 *)"abc", 3);
    /* Now append two unused trailer bytes — count says 1 entry,
     * but the buffer extends past the end of that entry. */
    g_byte_array_append (blob, (const guint8 *)"xx", 2);

    struct tlv_collect c = { 0, 0, 0, 0 };
    g_assert_false (hx_tracker_v3_walk_tlvs (blob->data, blob->len, 1,
                                             tlv_collect_cb, &c));
    /* The callback still ran for the one declared TLV — pin that
     * so a future refactor doesn't accidentally turn the walker
     * into a "validate first, dispatch after" two-pass. */
    g_assert_cmpuint (c.count, ==, 1);

    g_byte_array_free (blob, TRUE);
}

/* ---------- main ---------- */

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/tracker_v3/handshake/pack",
                     test_handshake_pack);
    g_test_add_func ("/tracker_v3/handshake/pack_no_features",
                     test_handshake_pack_no_features);
    g_test_add_func ("/tracker_v3/handshake/pack_short_buf",
                     test_handshake_pack_short_buf);

    g_test_add_func ("/tracker_v3/handshake/response_v3",
                     test_handshake_response_v3);
    g_test_add_func ("/tracker_v3/handshake/response_v1_fallback",
                     test_handshake_response_v1_fallback);
    g_test_add_func ("/tracker_v3/handshake/response_v2",
                     test_handshake_response_v2);
    g_test_add_func ("/tracker_v3/handshake/response_bad_magic",
                     test_handshake_response_bad_magic);
    g_test_add_func ("/tracker_v3/handshake/response_bad_length",
                     test_handshake_response_bad_length);

    g_test_add_func ("/tracker_v3/request/simple",
                     test_listing_request_simple);
    g_test_add_func ("/tracker_v3/request/short_buf",
                     test_listing_request_short_buf);

    g_test_add_func ("/tracker_v3/response/header_basic",
                     test_response_header_basic);
    g_test_add_func ("/tracker_v3/response/header_pagination",
                     test_response_header_pagination);
    g_test_add_func ("/tracker_v3/response/header_short_input",
                     test_response_header_short_input);
    g_test_add_func ("/tracker_v3/response/header_wrong_type",
                     test_response_header_wrong_type);

    g_test_add_func ("/tracker_v3/record/ipv4_basic",
                     test_record_ipv4_basic);
    g_test_add_func ("/tracker_v3/record/ipv6",
                     test_record_ipv6);
    g_test_add_func ("/tracker_v3/record/hostname",
                     test_record_hostname);
    g_test_add_func ("/tracker_v3/record/unknown_addr_type",
                     test_record_unknown_addr_type);
    g_test_add_func ("/tracker_v3/record/truncated_after_addr",
                     test_record_truncated_after_addr);
    g_test_add_func ("/tracker_v3/record/truncated_in_name",
                     test_record_truncated_in_name);
    g_test_add_func ("/tracker_v3/record/truncated_in_tlv",
                     test_record_truncated_in_tlv);
    g_test_add_func ("/tracker_v3/record/two_back_to_back",
                     test_record_two_back_to_back);

    g_test_add_func ("/tracker_v3/tlv/walk_basic", test_walk_tlvs_basic);
    g_test_add_func ("/tracker_v3/tlv/walk_truncated",
                     test_walk_tlvs_truncated);
    g_test_add_func ("/tracker_v3/tlv/walk_early_stop",
                     test_walk_tlvs_early_stop);
    g_test_add_func ("/tracker_v3/tlv/walk_leftover_bytes",
                     test_walk_tlvs_leftover_bytes);

    return g_test_run ();
}
