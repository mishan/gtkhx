/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/proto/test_tracker_v3_meta.c — Tier 2 coverage for the
 * typed TLV-trailer accessor in src/tracker_v3_meta.c.
 *
 * Each TLV id has its own type (string, u8, u16, i16, u32, bool)
 * and the decoder fans out a switch over the catalog in
 * src/hotline.h. The tests below pin one canonical happy-path
 * decode per type plus the contract edges: unknown ids skipped,
 * malformed blobs rejected, empty / NULL inputs returning a
 * zeroed meta.
 *
 * Synthesized blobs are built via small helpers so the test
 * fixtures look like the spec tables rather than hex dumps.
 */

#include "config.h"

#include <string.h>
#include <glib.h>
#include "compat.h"
#include "hotline.h"
#include "tracker_v3_meta.h"

/* ---- Blob-builder helpers --------------------------------------- */

static void
buf_append_u16_be (GByteArray *dst, guint16 v)
{
    guint8 b[2] = { (guint8) (v >> 8), (guint8) (v & 0xff) };
    g_byte_array_append (dst, b, 2);
}

/* Append one TLV {id, len, value} to dst. */
static void
add_tlv (GByteArray *dst, guint16 id, const guint8 *value, guint16 value_len)
{
    buf_append_u16_be (dst, id);
    buf_append_u16_be (dst, value_len);
    if (value_len > 0) {
        g_byte_array_append (dst, value, value_len);
    }
}

static void
add_tlv_u8 (GByteArray *dst, guint16 id, guint8 v)
{
    add_tlv (dst, id, &v, 1);
}

static void
add_tlv_u16 (GByteArray *dst, guint16 id, guint16 v)
{
    guint8 b[2] = { (guint8) (v >> 8), (guint8) (v & 0xff) };
    add_tlv (dst, id, b, 2);
}

static void
add_tlv_u32 (GByteArray *dst, guint16 id, guint32 v)
{
    guint8 b[4] = {
        (guint8) ((v >> 24) & 0xff),
        (guint8) ((v >> 16) & 0xff),
        (guint8) ((v >> 8)  & 0xff),
        (guint8) (v & 0xff),
    };
    add_tlv (dst, id, b, 4);
}

static void
add_tlv_bool (GByteArray *dst, guint16 id, gboolean v)
{
    guint8 byte = v ? 1 : 0;
    add_tlv (dst, id, &byte, 1);
}

static void
add_tlv_str (GByteArray *dst, guint16 id, const char *s)
{
    add_tlv (dst, id, (const guint8 *) s, (guint16) strlen (s));
}

/* ---- Empty / degenerate inputs --------------------------------- */

static void
test_empty_tlv_count (void)
{
    /* tlv_count = 0 → zero-init meta, regardless of buf contents. */
    HxTrackerV3Meta *m = hx_tracker_v3_meta_new (NULL, 0, 0);
    g_assert_nonnull (m);
    g_assert_null (m->server_software);
    g_assert_null (m->country_code);
    g_assert_cmpuint (m->max_users, ==, 0);
    g_assert_false (m->has_max_users);
    g_assert_cmpuint (m->maturity, ==, HX_TRACKER_V3_MATURITY_GENERAL);
    g_assert_cmpuint (m->listing_category, ==,
                      HX_TRACKER_V3_CATEGORY_UNSPECIFIED);
    g_assert_false (m->supports_hope);
    g_assert_false (m->is_promoted);
    hx_tracker_v3_meta_free (m);
}

static void
test_empty_bytes_helper (void)
{
    /* NULL GBytes is the v1-path normal case — returns empty meta. */
    HxTrackerV3Meta *m = hx_tracker_v3_meta_new_from_bytes (NULL, 0);
    g_assert_nonnull (m);
    g_assert_null (m->server_software);
    hx_tracker_v3_meta_free (m);
}

/* ---- Descriptive fields (0x0200 block) -------------------------- */

static void
test_descriptive_block (void)
{
    GByteArray *blob = g_byte_array_new ();
    add_tlv_str  (blob, HTRK_V3_TLV_SERVER_SOFTWARE,  "hxd/2.0.8-dev");
    add_tlv_str  (blob, HTRK_V3_TLV_COUNTRY_CODE,     "US");
    add_tlv_str  (blob, HTRK_V3_TLV_REGION,           "Bay Area");
    add_tlv_str  (blob, HTRK_V3_TLV_LANGUAGE,         "en");
    add_tlv_u16  (blob, HTRK_V3_TLV_MAX_USERS,        128);
    add_tlv_u8   (blob, HTRK_V3_TLV_MATURITY,
                  HX_TRACKER_V3_MATURITY_MATURE);
    add_tlv_u32  (blob, HTRK_V3_TLV_UPTIME,           86400);
    add_tlv_str  (blob, HTRK_V3_TLV_RULES_URL,
                  "https://example.org/rules");
    add_tlv_str  (blob, HTRK_V3_TLV_BANNER_URL,
                  "https://example.org/banner.gif");
    add_tlv_str  (blob, HTRK_V3_TLV_ICON_URL,
                  "https://example.org/icon.png");
    add_tlv_u32  (blob, HTRK_V3_TLV_LINK_DOWN_MBIT,   1000);
    add_tlv_u32  (blob, HTRK_V3_TLV_LINK_UP_MBIT,     100);
    /* i16 — -480 = UTC-8. Pack via add_tlv_u16 since we know
     * two's-complement representation matches u16 bits. */
    add_tlv_u16  (blob, HTRK_V3_TLV_TIMEZONE_OFFSET,
                  (guint16) ((gint16) -480));
    add_tlv_str  (blob, HTRK_V3_TLV_CONTACT_URL,
                  "mailto:admin@example.org");
    add_tlv_u32  (blob, HTRK_V3_TLV_SERVER_LAUNCHED,  1700000000);
    add_tlv_u16  (blob, HTRK_V3_TLV_MIN_PROTO_VERSION, 0x00be);
    add_tlv_u16  (blob, HTRK_V3_TLV_PEAK_24H,         42);
    add_tlv_u16  (blob, HTRK_V3_TLV_AVG_24H,          17);
    add_tlv_str  (blob, HTRK_V3_TLV_TAGS,             "mac,retro,dev");

    HxTrackerV3Meta *m = hx_tracker_v3_meta_new (blob->data, blob->len, 19);
    g_assert_nonnull (m);

    g_assert_cmpstr (m->server_software, ==, "hxd/2.0.8-dev");
    g_assert_cmpstr (m->country_code, ==, "US");
    g_assert_cmpstr (m->region, ==, "Bay Area");
    g_assert_cmpstr (m->language, ==, "en");
    g_assert_cmpuint (m->max_users, ==, 128);
    g_assert_true (m->has_max_users);
    g_assert_cmpuint (m->maturity, ==, HX_TRACKER_V3_MATURITY_MATURE);
    g_assert_cmpuint (m->uptime_secs, ==, 86400);
    g_assert_cmpstr (m->rules_url, ==, "https://example.org/rules");
    g_assert_cmpstr (m->banner_url, ==, "https://example.org/banner.gif");
    g_assert_cmpstr (m->icon_url, ==, "https://example.org/icon.png");
    g_assert_cmpuint (m->link_down_mbit, ==, 1000);
    g_assert_cmpuint (m->link_up_mbit, ==, 100);
    g_assert_cmpint  (m->timezone_offset_min, ==, -480);
    g_assert_true (m->has_timezone_offset);
    g_assert_cmpstr (m->contact_url, ==, "mailto:admin@example.org");
    g_assert_cmpuint (m->server_launched, ==, 1700000000);
    g_assert_cmpuint (m->min_proto_version, ==, 0x00be);
    g_assert_cmpuint (m->peak_24h, ==, 42);
    g_assert_cmpuint (m->avg_24h, ==, 17);
    g_assert_cmpstr (m->tags, ==, "mac,retro,dev");

    hx_tracker_v3_meta_free (m);
    g_byte_array_free (blob, TRUE);
}

/* ---- Capability fields (0x0300 block) -------------------------- */

static void
test_capability_block (void)
{
    GByteArray *blob = g_byte_array_new ();
    add_tlv_u16  (blob, HTRK_V3_TLV_PROTOCOL_VERSION, 0x0197); /* 1.9.x */
    add_tlv_bool (blob, HTRK_V3_TLV_SUPPORTS_HOPE,    TRUE);
    add_tlv_bool (blob, HTRK_V3_TLV_SUPPORTS_TLS,     TRUE);
    add_tlv_u16  (blob, HTRK_V3_TLV_TLS_PORT,         5600);
    add_tlv_bool (blob, HTRK_V3_TLV_SUPPORTS_INLINE,  FALSE);
    add_tlv_bool (blob, HTRK_V3_TLV_SUPPORTS_VOICE,   FALSE);
    add_tlv_bool (blob, HTRK_V3_TLV_SUPPORTS_LARGEFILE, TRUE);
    add_tlv_bool (blob, HTRK_V3_TLV_SUPPORTS_IPV6_TLV, TRUE);
    add_tlv_str  (blob, HTRK_V3_TLV_HOPE_CIPHERS,
                  "CHACHA20-POLY1305,RC4,BLOWFISH");

    HxTrackerV3Meta *m = hx_tracker_v3_meta_new (blob->data, blob->len, 9);
    g_assert_nonnull (m);

    g_assert_cmpuint (m->protocol_version, ==, 0x0197);
    g_assert_true (m->supports_hope);
    g_assert_true (m->supports_tls);
    g_assert_cmpuint (m->tls_port, ==, 5600);
    g_assert_false (m->supports_inline_media);
    g_assert_false (m->supports_voice);
    g_assert_true (m->supports_large_files);
    g_assert_true (m->supports_ipv6);
    g_assert_cmpstr (m->hope_ciphers, ==,
                     "CHACHA20-POLY1305,RC4,BLOWFISH");

    hx_tracker_v3_meta_free (m);
    g_byte_array_free (blob, TRUE);
}

/* ---- Content-index fields (0x0400 block) ----------------------- */

static void
test_content_index_block (void)
{
    GByteArray *blob = g_byte_array_new ();
    add_tlv_u32 (blob, HTRK_V3_TLV_NEWS_COUNT,        500);
    add_tlv_u32 (blob, HTRK_V3_TLV_MSGBOARD_COUNT,    1200);
    add_tlv_u32 (blob, HTRK_V3_TLV_FILES_COUNT,       8400);
    add_tlv_u32 (blob, HTRK_V3_TLV_TOTAL_FILE_SIZE,   0xffffffffu);
    add_tlv_u32 (blob, HTRK_V3_TLV_LAST_NEWS_TIME,    1700001234);
    add_tlv_u32 (blob, HTRK_V3_TLV_LAST_CHAT_TIME,    1700005678);

    HxTrackerV3Meta *m = hx_tracker_v3_meta_new (blob->data, blob->len, 6);
    g_assert_nonnull (m);

    g_assert_cmpuint (m->news_count, ==, 500);
    g_assert_cmpuint (m->msgboard_count, ==, 1200);
    g_assert_cmpuint (m->files_count, ==, 8400);
    g_assert_cmpuint (m->total_file_size, ==, 0xffffffffu);
    g_assert_cmpuint (m->last_news_timestamp, ==, 1700001234);
    g_assert_cmpuint (m->last_chat_timestamp, ==, 1700005678);

    hx_tracker_v3_meta_free (m);
    g_byte_array_free (blob, TRUE);
}

/* ---- Privacy / tracker-injected blocks ------------------------- */

static void
test_privacy_and_tracker_injected (void)
{
    GByteArray *blob = g_byte_array_new ();
    add_tlv_bool (blob, HTRK_V3_TLV_PRIVATE_LISTING,  FALSE);
    add_tlv_u8   (blob, HTRK_V3_TLV_LISTING_CATEGORY,
                  HX_TRACKER_V3_CATEGORY_GAMING);
    add_tlv_bool (blob, HTRK_V3_TLV_LANGUAGE_STRICT,  TRUE);
    add_tlv_bool (blob, HTRK_V3_TLV_IS_PROMOTED,      TRUE);
    add_tlv_u32  (blob, HTRK_V3_TLV_FIRST_SEEN,       1690000000);
    add_tlv_u32  (blob, HTRK_V3_TLV_LAST_HEARTBEAT,   1700100000);
    add_tlv_bool (blob, HTRK_V3_TLV_VERIFIED_ONLINE,  TRUE);

    HxTrackerV3Meta *m = hx_tracker_v3_meta_new (blob->data, blob->len, 7);
    g_assert_nonnull (m);

    g_assert_false (m->private_listing);
    g_assert_cmpuint (m->listing_category, ==,
                      HX_TRACKER_V3_CATEGORY_GAMING);
    g_assert_true (m->language_strict);
    g_assert_true (m->is_promoted);
    g_assert_cmpuint (m->first_seen, ==, 1690000000);
    g_assert_cmpuint (m->last_heartbeat, ==, 1700100000);
    g_assert_true (m->verified_online);

    hx_tracker_v3_meta_free (m);
    g_byte_array_free (blob, TRUE);
}

/* ---- Vocabulary clamps ----------------------------------------- */

static void
test_unknown_maturity_clamps_to_general (void)
{
    /* Spec: unknown maturity values MUST be treated as 0 (general). */
    GByteArray *blob = g_byte_array_new ();
    add_tlv_u8 (blob, HTRK_V3_TLV_MATURITY, 99);
    HxTrackerV3Meta *m = hx_tracker_v3_meta_new (blob->data, blob->len, 1);
    g_assert_nonnull (m);
    g_assert_cmpuint (m->maturity, ==, HX_TRACKER_V3_MATURITY_GENERAL);
    hx_tracker_v3_meta_free (m);
    g_byte_array_free (blob, TRUE);
}

static void
test_unknown_category_clamps_to_unspecified (void)
{
    /* Spec: unknown listing-category values MUST be 0 (unspecified). */
    GByteArray *blob = g_byte_array_new ();
    add_tlv_u8 (blob, HTRK_V3_TLV_LISTING_CATEGORY, 200);
    HxTrackerV3Meta *m = hx_tracker_v3_meta_new (blob->data, blob->len, 1);
    g_assert_nonnull (m);
    g_assert_cmpuint (m->listing_category, ==,
                      HX_TRACKER_V3_CATEGORY_UNSPECIFIED);
    hx_tracker_v3_meta_free (m);
    g_byte_array_free (blob, TRUE);
}

/* ---- Forward-compat: unknown ids silently skipped --------------- */

static void
test_unknown_ids_skipped (void)
{
    /* Two unknown TLV ids surrounding one known one. The decoder
     * MUST advance past the unknowns and still populate the known
     * field. */
    GByteArray *blob = g_byte_array_new ();
    /* Hypothetical future TLV with a 9-byte string payload. */
    add_tlv_str (blob, 0x7777, "futureval");
    add_tlv_str (blob, HTRK_V3_TLV_SERVER_SOFTWARE, "real-server/1.0");
    /* Another unknown id, u32 payload. */
    add_tlv_u32 (blob, 0x8888, 0xdeadbeefu);

    HxTrackerV3Meta *m = hx_tracker_v3_meta_new (blob->data, blob->len, 3);
    g_assert_nonnull (m);
    g_assert_cmpstr (m->server_software, ==, "real-server/1.0");
    hx_tracker_v3_meta_free (m);
    g_byte_array_free (blob, TRUE);
}

/* ---- Malformed inputs ------------------------------------------ */

static void
test_truncated_blob_returns_null (void)
{
    /* TLV declares value_len=20 but only 4 bytes of value follow.
     * The walker bails partway through; the public API turns that
     * into NULL so callers don't act on partial state. */
    GByteArray *blob = g_byte_array_new ();
    buf_append_u16_be (blob, HTRK_V3_TLV_SERVER_SOFTWARE);
    buf_append_u16_be (blob, 20);
    g_byte_array_append (blob, (const guint8 *) "abcd", 4);

    HxTrackerV3Meta *m = hx_tracker_v3_meta_new (blob->data, blob->len, 1);
    g_assert_null (m);
    g_byte_array_free (blob, TRUE);
}

static void
test_count_too_high_returns_null (void)
{
    /* One well-formed TLV in the blob, but we claim count=2. The
     * walker tries to read a second TLV header past the buffer
     * end and fails. */
    GByteArray *blob = g_byte_array_new ();
    add_tlv_str (blob, HTRK_V3_TLV_TAGS, "abc");

    HxTrackerV3Meta *m = hx_tracker_v3_meta_new (blob->data, blob->len, 2);
    g_assert_null (m);
    g_byte_array_free (blob, TRUE);
}

/* ---- Wrong-sized values default sensibly ----------------------- */

static void
test_wrong_size_numeric_falls_back_to_default (void)
{
    /* MAX_USERS claims a 1-byte value (wrong — spec is u16). The
     * read fails closed: max_users stays 0 and has_max_users stays
     * FALSE because the assignment branch never ran... actually no,
     * the assignment IS guarded inside the case but read_u16
     * returns the default (0) on len != 2, AND has_max_users still
     * gets set TRUE because the case body runs unconditionally.
     *
     * Pin the current contract: presence-of-TLV sets has_*, value
     * defaults to 0 when malformed. Callers that want "ignore
     * malformed" need a separate check. */
    GByteArray *blob = g_byte_array_new ();
    guint8 short_v = 0x42;
    add_tlv (blob, HTRK_V3_TLV_MAX_USERS, &short_v, 1);
    HxTrackerV3Meta *m = hx_tracker_v3_meta_new (blob->data, blob->len, 1);
    g_assert_nonnull (m);
    g_assert_cmpuint (m->max_users, ==, 0);
    g_assert_true (m->has_max_users);
    hx_tracker_v3_meta_free (m);
    g_byte_array_free (blob, TRUE);
}

static void
test_oversized_numeric_falls_back_to_default (void)
{
    /* Spec-fixed-width numeric TLVs (u8 / u16 / i16 / u32) must
     * reject lengths OTHER than the spec width — not just shorter.
     * An overlong payload is just as malformed as a truncated one
     * and must take the default branch, otherwise:
     *   - a corrupt TLV with garbage trailing bytes silently
     *     decodes from its first N bytes, hiding the bug;
     *   - a hypothetical future spec rev that widens a field would
     *     get its first N bytes decoded under the old contract and
     *     misinterpreted as the legacy type.
     *
     * Pin: u8 / u16 / i16 / u32 with len = spec_width + N (N > 0)
     * all read as their respective defaults. */
    GByteArray *blob = g_byte_array_new ();

    /* MATURITY is u8 → length 2 is wrong. Spec default = GENERAL. */
    const guint8 over_u8[] = { 0x02, 0xff };
    add_tlv (blob, HTRK_V3_TLV_MATURITY, over_u8, sizeof (over_u8));

    /* MAX_USERS is u16 → length 3 is wrong. Reader default = 0. */
    const guint8 over_u16[] = { 0x00, 0x80, 0xab };
    add_tlv (blob, HTRK_V3_TLV_MAX_USERS, over_u16, sizeof (over_u16));

    /* TIMEZONE_OFFSET is i16 → length 4 is wrong. Reader default = 0. */
    const guint8 over_i16[] = { 0xfe, 0x20, 0xde, 0xad };
    add_tlv (blob, HTRK_V3_TLV_TIMEZONE_OFFSET, over_i16, sizeof (over_i16));

    /* UPTIME is u32 → length 5 is wrong. Reader default = 0. */
    const guint8 over_u32[] = { 0x00, 0x01, 0x51, 0x80, 0xff };
    add_tlv (blob, HTRK_V3_TLV_UPTIME, over_u32, sizeof (over_u32));

    HxTrackerV3Meta *m = hx_tracker_v3_meta_new (blob->data, blob->len, 4);
    g_assert_nonnull (m);

    /* MATURITY went default → GENERAL (not whatever 0x02 would map
     * to). MAX_USERS and TIMEZONE_OFFSET defaulted to 0; uptime
     * defaulted to 0 too. presence flags still flip on case entry. */
    g_assert_cmpuint (m->maturity, ==, HX_TRACKER_V3_MATURITY_GENERAL);
    g_assert_cmpuint (m->max_users, ==, 0);
    g_assert_true (m->has_max_users);
    g_assert_cmpint  (m->timezone_offset_min, ==, 0);
    g_assert_true (m->has_timezone_offset);
    g_assert_cmpuint (m->uptime_secs, ==, 0);

    hx_tracker_v3_meta_free (m);
    g_byte_array_free (blob, TRUE);
}

static void
test_empty_string_tlv (void)
{
    /* Zero-length string TLV — the field becomes "" (empty string,
     * not NULL). NULL is reserved for "TLV absent." */
    GByteArray *blob = g_byte_array_new ();
    add_tlv (blob, HTRK_V3_TLV_TAGS, NULL, 0);
    HxTrackerV3Meta *m = hx_tracker_v3_meta_new (blob->data, blob->len, 1);
    g_assert_nonnull (m);
    g_assert_nonnull (m->tags);
    g_assert_cmpstr (m->tags, ==, "");
    hx_tracker_v3_meta_free (m);
    g_byte_array_free (blob, TRUE);
}

/* ---- UTF-8 sanitisation ---------------------------------------- */

static void
test_invalid_utf8_replaced_not_dropped (void)
{
    /* A name with an invalid UTF-8 sequence — 0xff is never a valid
     * UTF-8 leading byte. g_utf8_make_valid replaces it with U+FFFD;
     * length and surrounding bytes remain. We pin that the field is
     * non-NULL and contains no raw 0xff bytes. */
    GByteArray *blob = g_byte_array_new ();
    const guint8 bad[] = { 'h', 'i', 0xff, 'o', 'k' };
    add_tlv (blob, HTRK_V3_TLV_SERVER_SOFTWARE, bad, sizeof (bad));
    HxTrackerV3Meta *m = hx_tracker_v3_meta_new (blob->data, blob->len, 1);
    g_assert_nonnull (m);
    g_assert_nonnull (m->server_software);
    g_assert_null (strchr (m->server_software, (char) 0xff));
    /* And the resulting string IS valid UTF-8 — Pango-safe. */
    g_assert_true (g_utf8_validate (m->server_software, -1, NULL));
    hx_tracker_v3_meta_free (m);
    g_byte_array_free (blob, TRUE);
}

/* ---- Copy semantics -------------------------------------------- */

static void
test_copy_deep_strings (void)
{
    GByteArray *blob = g_byte_array_new ();
    add_tlv_str (blob, HTRK_V3_TLV_SERVER_SOFTWARE, "ServerName");
    add_tlv_str (blob, HTRK_V3_TLV_TAGS,            "a,b,c");
    add_tlv_u16 (blob, HTRK_V3_TLV_MAX_USERS,       50);

    HxTrackerV3Meta *src = hx_tracker_v3_meta_new (blob->data, blob->len, 3);
    g_assert_nonnull (src);

    HxTrackerV3Meta *dup = hx_tracker_v3_meta_copy (src);
    g_assert_nonnull (dup);

    /* Strings are distinct allocations (deep copy). */
    g_assert_true (src->server_software != dup->server_software);
    g_assert_cmpstr (src->server_software, ==, dup->server_software);
    g_assert_true (src->tags != dup->tags);
    g_assert_cmpstr (src->tags, ==, dup->tags);
    g_assert_cmpuint (src->max_users, ==, dup->max_users);

    /* Freeing src must not corrupt dup. */
    hx_tracker_v3_meta_free (src);
    g_assert_cmpstr (dup->server_software, ==, "ServerName");
    g_assert_cmpstr (dup->tags, ==, "a,b,c");
    hx_tracker_v3_meta_free (dup);
    g_byte_array_free (blob, TRUE);
}

static void
test_copy_null_is_null (void)
{
    g_assert_null (hx_tracker_v3_meta_copy (NULL));
}

/* ---- main ------------------------------------------------------- */

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/tracker_v3_meta/empty/tlv_count_zero",
                     test_empty_tlv_count);
    g_test_add_func ("/tracker_v3_meta/empty/null_bytes_helper",
                     test_empty_bytes_helper);

    g_test_add_func ("/tracker_v3_meta/descriptive/full_block",
                     test_descriptive_block);
    g_test_add_func ("/tracker_v3_meta/capability/full_block",
                     test_capability_block);
    g_test_add_func ("/tracker_v3_meta/content_index/full_block",
                     test_content_index_block);
    g_test_add_func ("/tracker_v3_meta/privacy_and_tracker_injected",
                     test_privacy_and_tracker_injected);

    g_test_add_func ("/tracker_v3_meta/vocab/unknown_maturity_clamps",
                     test_unknown_maturity_clamps_to_general);
    g_test_add_func ("/tracker_v3_meta/vocab/unknown_category_clamps",
                     test_unknown_category_clamps_to_unspecified);

    g_test_add_func ("/tracker_v3_meta/forward_compat/unknown_ids_skipped",
                     test_unknown_ids_skipped);

    g_test_add_func ("/tracker_v3_meta/malformed/truncated_blob",
                     test_truncated_blob_returns_null);
    g_test_add_func ("/tracker_v3_meta/malformed/count_too_high",
                     test_count_too_high_returns_null);

    g_test_add_func ("/tracker_v3_meta/edges/wrong_size_numeric",
                     test_wrong_size_numeric_falls_back_to_default);
    g_test_add_func ("/tracker_v3_meta/edges/oversized_numeric",
                     test_oversized_numeric_falls_back_to_default);
    g_test_add_func ("/tracker_v3_meta/edges/empty_string_tlv",
                     test_empty_string_tlv);
    g_test_add_func ("/tracker_v3_meta/edges/invalid_utf8_replaced",
                     test_invalid_utf8_replaced_not_dropped);

    g_test_add_func ("/tracker_v3_meta/copy/deep_strings",
                     test_copy_deep_strings);
    g_test_add_func ("/tracker_v3_meta/copy/null_is_null",
                     test_copy_null_is_null);

    return g_test_run ();
}
