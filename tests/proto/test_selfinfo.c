/*
 * tests/proto/test_selfinfo.c — drive hx_selfinfo_parse against
 * canned wire-format HTLS_HDR_USER_SELFINFO buffers.
 *
 * SELFINFO is the message the server sends after login that tells
 * us our access bitmap, our session UID, our icon, and our display
 * name. Phase 5 made this the canonical "post-login is finished"
 * trigger — do_post_login_fetches() fires off USER_GETLIST and the
 * news fetch from here. Getting the parse wrong means downstream
 * gating (READ_NEWS, DISCONNECT_USERS, etc. on the access bitmap)
 * gates against the wrong values.
 *
 * Two payload chunks matter:
 *   HTLS_DATA_ACCESS    — exactly 8 bytes, the access bitmap.
 *   HTLS_DATA_USER_LIST — hl_userlist_hdr (uid, icon, color, nlen) +
 *                         display name. The handler quirk: the UID
 *                         is byte-swapped from htlc->uid (which the
 *                         GUI already filled in during login), not
 *                         from the chunk body.
 *
 * The fixture builder packs hx_test_in(htlc)->buf the same way hlwrite would.
 */

#include "config.h"
#include <string.h>
#include <netinet/in.h>
#include <glib.h>
#include "protocol.h"
#include "hotline.h"
#include "proto_helpers.h"
#include "wire_fixture.h"

/* Build a HTLS_DATA_USER_LIST chunk into a stack buffer. Returns
 * the byte length. The hl_userlist_hdr struct's first two fields
 * (type, len) overlap with hl_data_hdr — wire_fixture_add_chunk
 * writes its own dh_hdr, so we hand it just the post-header tail
 * (uid/icon/color/nlen + name). */
static gsize
build_userlist_payload (guint8 *out, gsize out_size, guint16 uid, guint16 icon,
                        guint16 color, const char *name, gsize name_len)
{
    /* Layout from hotline.h, sans the leading hl_data_hdr that
	 * wire_fixture_add_chunk handles for us:
	 *
	 *   guint16 uid, icon, color, nlen
	 *   guint8  name[]
	 */
    gsize need = 8 + name_len;
    g_assert_cmpuint (need, <=, out_size);

    guint16 v;
    v = htons (uid);
    memcpy (out + 0, &v, 2);
    v = htons (icon);
    memcpy (out + 2, &v, 2);
    v = htons (color);
    memcpy (out + 4, &v, 2);
    v = htons ((guint16)name_len);
    memcpy (out + 6, &v, 2);
    memcpy (out + 8, name, name_len);
    return need;
}

/* ---------- Access bitmap ---------- */

static void
test_selfinfo_extracts_access_bitmap (void)
{
    struct htlc_conn htlc;
    wire_fixture_init (&htlc, HTLS_HDR_USER_SELFINFO, 1, 0);

    /* The hlserver-guest canonical access bitmap (same vector
	 * test_hl_access uses). Pin it byte-for-byte here so a future
	 * selfinfo parser that swaps endianness or shifts bytes fails
	 * loudly on a known-good value. */
    const guint8 access[8] = { 0x60, 0x70, 0x00, 0xa0, 0x00, 0x80, 0x00, 0x00 };
    wire_fixture_add_chunk (&htlc, HTLS_DATA_ACCESS, sizeof (access), access);

    unsigned seen = hx_selfinfo_parse (&htlc, hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos);
    g_assert_true (seen & HX_SELFINFO_ACCESS);
    g_assert_false (seen & HX_SELFINFO_USER_LIST);
    /* htlc.access is a guint64; compare its bytes against the wire
	 * vector. memcpy preserved the wire bytes in memory order. */
    g_assert_cmpmem (&htlc.access, 8, access, 8);

    wire_fixture_free (&htlc);
}

/* The handler explicitly checks `_len != 8` and silently skips a
 * malformed access chunk. Verify that — anything else would be a
 * memcpy-out-of-bounds bug waiting to happen. */
static void
test_selfinfo_skips_malformed_short_access (void)
{
    struct htlc_conn htlc;
    wire_fixture_init (&htlc, HTLS_HDR_USER_SELFINFO, 1, 0);

    const guint8 too_short[5] = { 0x11, 0x22, 0x33, 0x44, 0x55 };
    wire_fixture_add_chunk (&htlc, HTLS_DATA_ACCESS, sizeof (too_short),
                            too_short);

    unsigned seen = hx_selfinfo_parse (&htlc, hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos);
    g_assert_false (seen & HX_SELFINFO_ACCESS);
    /* And htlc.access stays zeroed by wire_fixture_init. */
    g_assert_cmphex (htlc.access, ==, 0);

    wire_fixture_free (&htlc);
}

static void
test_selfinfo_skips_malformed_long_access (void)
{
    struct htlc_conn htlc;
    wire_fixture_init (&htlc, HTLS_HDR_USER_SELFINFO, 1, 0);

    guint8 too_long[16];
    memset (too_long, 0xff, sizeof (too_long));
    wire_fixture_add_chunk (&htlc, HTLS_DATA_ACCESS, sizeof (too_long),
                            too_long);

    unsigned seen = hx_selfinfo_parse (&htlc, hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos);
    g_assert_false (seen & HX_SELFINFO_ACCESS);
    /* Crucially: nothing got copied into htlc.access (which would
	 * have to overflow into adjacent fields with a 16-byte chunk). */
    g_assert_cmphex (htlc.access, ==, 0);

    wire_fixture_free (&htlc);
}

/* ---------- User list chunk ---------- */

/* Phase 5: hx_selfinfo_parse intentionally does NOT write htlc.name
 * any more. The server caches the user's nick across sessions and on
 * reconnect echoes it back via this chunk; copying the bytes into
 * htlc->name lets a previously-corrupt cached value clobber the
 * user's local prefs nick. The new policy is "local nick wins" —
 * hx_rcv_user_selfinfo pushes our htlc->name to the server via
 * USER_CHANGE right after the parse, overwriting whatever the server
 * cached. uid / icon / access still parse normally. */
static void
test_selfinfo_extracts_user_list (void)
{
    struct htlc_conn htlc;
    wire_fixture_init (&htlc, HTLS_HDR_USER_SELFINFO, 1, 0);

    /* Pre-populate htlc.name with the user's local nick. The parse
	 * must leave it alone. */
    strncpy ((char *)htlc.name, "Local", sizeof (htlc.name) - 1);

    guint8 payload[64];
    const char *server_name = "Misha";
    gsize plen = build_userlist_payload (payload, sizeof (payload),
                                         /*uid*/ 0x1234,
                                         /*icon*/ 412,
                                         /*color*/ 0, server_name,
                                         strlen (server_name));
    wire_fixture_add_chunk (&htlc, HTLS_DATA_USER_LIST, (guint16)plen, payload);

    unsigned seen = hx_selfinfo_parse (&htlc, hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos);
    g_assert_true (seen & HX_SELFINFO_USER_LIST);

    g_assert_cmphex (htlc.uid, ==, 0x1234);
    g_assert_cmphex (htlc.icon, ==, 412);
    /* htlc.name is unchanged — the server-supplied "Misha" must not
	 * overwrite the pre-call "Local". */
    g_assert_cmpstr ((const char *)htlc.name, ==, "Local");

    wire_fixture_free (&htlc);
}

/* Regression-net for the self-aliasing HN16 bug we fixed in Phase 5
 * (commit `selfinfo HN16` — the previous code did
 *   HN16 (&htlc->uid, &htlc->uid)
 * which corrupted htlc->uid into (high<<8)|high). Set htlc->uid
 * pre-call to a non-zero value, point the wire UID at a different
 * value, confirm we end up with the wire UID. If anyone reverts to
 * the self-alias form, this test fails loudly because htlc->uid will
 * be neither the pre-call value nor the wire value. */
static void
test_selfinfo_uid_overrides_pre_call_value (void)
{
    struct htlc_conn htlc;
    wire_fixture_init (&htlc, HTLS_HDR_USER_SELFINFO, 1, 0);

    /* Pre-populate htlc->uid (this is what the LOGIN_REPLY handler
	 * does earlier in the connection lifecycle). */
    htlc.uid = 0xbeef;

    guint8 payload[32];
    gsize plen = build_userlist_payload (payload, sizeof (payload),
                                         /*uid*/ 0x00cd, 0, 0, "x", 1);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_USER_LIST, (guint16)plen, payload);

    hx_selfinfo_parse (&htlc, hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos);

    /* The wire UID wins; the pre-call value is irrelevant.
	 * Specifically NOT equal to (0xbe << 8) | 0xbe = 0xbebe — that
	 * was the buggy outcome of the self-aliasing HN16. */
    g_assert_cmphex (htlc.uid, ==, 0x00cd);
    g_assert_cmphex (htlc.uid, !=, 0xbebe);
}

/* Phase 5: hx_selfinfo_parse no longer writes htlc->name, so a long
 * server-supplied name shouldn't corrupt either htlc->name or the
 * adjacent htlc->login field. Verify that an oversized USER_LIST name
 * parses cleanly with all neighbouring state untouched. */
static void
test_selfinfo_long_server_name_leaves_local_intact (void)
{
    struct htlc_conn htlc;
    wire_fixture_init (&htlc, HTLS_HDR_USER_SELFINFO, 1, 0);

    /* Pre-populate htlc.name and htlc.login with sentinels. Both
	 * 32-byte fixed buffers sit adjacent in struct htlc_conn; an
	 * old-style memcpy(htlc->name, uh->name, nlen) without a length
	 * cap could spill into htlc->login. The new behaviour writes
	 * neither. */
    strncpy ((char *)htlc.name, "Local", sizeof (htlc.name) - 1);
    strncpy ((char *)htlc.login, "user", sizeof (htlc.login) - 1);

    char long_name[64];
    memset (long_name, 'X', sizeof (long_name));
    guint8 payload[80];
    gsize plen = build_userlist_payload (payload, sizeof (payload), 0, 0, 0,
                                         long_name, sizeof (long_name));
    wire_fixture_add_chunk (&htlc, HTLS_DATA_USER_LIST, (guint16)plen, payload);

    hx_selfinfo_parse (&htlc, hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos);

    g_assert_cmpstr ((const char *)htlc.name, ==, "Local");
    g_assert_cmpstr ((const char *)htlc.login, ==, "user");

    wire_fixture_free (&htlc);
}

static void
test_selfinfo_skips_too_short_user_list (void)
{
    struct htlc_conn htlc;
    wire_fixture_init (&htlc, HTLS_HDR_USER_SELFINFO, 1, 0);

    /* hl_userlist_hdr's tail is 8 bytes (uid/icon/color/nlen);
	 * a chunk shorter than that should be skipped. */
    guint8 too_short[5] = { 0 };
    wire_fixture_add_chunk (&htlc, HTLS_DATA_USER_LIST, sizeof (too_short),
                            too_short);

    unsigned seen = hx_selfinfo_parse (&htlc, hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos);
    g_assert_false (seen & HX_SELFINFO_USER_LIST);
    g_assert_cmpuint (htlc.icon, ==, 0);
    /* htlc.name is not touched by the parse path anyway (see
	 * test_selfinfo_extracts_user_list); the wire_fixture_init
	 * memset leaves it zeroed, so the [0]==0 check still holds. */
    g_assert_cmphex (htlc.name[0], ==, 0);

    wire_fixture_free (&htlc);
}

/* ---------- Combined: access + user list ---------- */

static void
test_selfinfo_parses_both_chunks (void)
{
    struct htlc_conn htlc;
    wire_fixture_init (&htlc, HTLS_HDR_USER_SELFINFO, 1, 0);

    /* Server typically sends ACCESS first, USER_LIST second. */
    const guint8 access[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    wire_fixture_add_chunk (&htlc, HTLS_DATA_ACCESS, sizeof (access), access);

    guint8 payload[64];
    const char *name = "admin";
    gsize plen = build_userlist_payload (payload, sizeof (payload),
                                         /*uid*/ 5, /*icon*/ 100, /*color*/ 0,
                                         name, strlen (name));
    wire_fixture_add_chunk (&htlc, HTLS_DATA_USER_LIST, (guint16)plen, payload);

    unsigned seen = hx_selfinfo_parse (&htlc, hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos);
    g_assert_cmpuint (seen, ==, (HX_SELFINFO_ACCESS | HX_SELFINFO_USER_LIST));
    g_assert_cmphex (htlc.uid, ==, 5);
    g_assert_cmphex (htlc.icon, ==, 100);
    /* Phase 5: htlc.name is not written by hx_selfinfo_parse; the
	 * server-supplied "admin" stays in the wire chunk but doesn't
	 * land in htlc.name. wire_fixture_init zeros the struct so the
	 * pre-call value is "". */
    g_assert_cmpstr ((const char *)htlc.name, ==, "");
    /* All-ones in every byte → guint64 is 0xffffffffffffffff. */
    g_assert_cmphex (htlc.access, ==, G_GUINT64_CONSTANT (0xffffffffffffffff));

    wire_fixture_free (&htlc);
}

static void
test_selfinfo_empty_message_returns_zero (void)
{
    struct htlc_conn htlc;
    wire_fixture_init (&htlc, HTLS_HDR_USER_SELFINFO, 1, 0);

    unsigned seen = hx_selfinfo_parse (&htlc, hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos);
    g_assert_cmpuint (seen, ==, 0);

    wire_fixture_free (&htlc);
}

/* ---------- Colored-Nicknames extension ----------
 *
 * SELFINFO may carry HTLS_DATA_COLOR (0x0500) — the server's view of
 * our RGB nick color, either the per-account override from server
 * config or the default the server assigned us at login. Parser
 * contract: 4 bytes BE → htlc->nick_color + HX_SELFINFO_NICK_COLOR
 * bit set; any other length is silently skipped (no mutation of
 * htlc->nick_color). HX_NICK_COLOR_NONE round-trips verbatim. */

static void
test_selfinfo_decodes_nick_color (void)
{
    struct htlc_conn htlc;
    wire_fixture_init (&htlc, HTLS_HDR_USER_SELFINFO, 1, 0);
    /* Pre-set htlc->nick_color to a sentinel so we can prove the
	 * parse overwrote it (not just left a zero in place). */
    htlc.nick_color = 0xdeadbeefu;

    const guint32 color_wire = htonl (0x00abcdefu);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_COLOR, sizeof (color_wire),
                            &color_wire);

    unsigned seen = hx_selfinfo_parse (&htlc, hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos);
    g_assert_true (seen & HX_SELFINFO_NICK_COLOR);
    g_assert_cmphex (htlc.nick_color, ==, 0x00abcdefu);

    wire_fixture_free (&htlc);
}

static void
test_selfinfo_nick_color_none_round_trips (void)
{
    struct htlc_conn htlc;
    wire_fixture_init (&htlc, HTLS_HDR_USER_SELFINFO, 1, 0);

    const guint32 color_wire = htonl (HX_NICK_COLOR_NONE);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_COLOR, sizeof (color_wire),
                            &color_wire);

    unsigned seen = hx_selfinfo_parse (&htlc, hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos);
    g_assert_true (seen & HX_SELFINFO_NICK_COLOR);
    g_assert_cmphex (htlc.nick_color, ==, HX_NICK_COLOR_NONE);

    wire_fixture_free (&htlc);
}

static void
test_selfinfo_nick_color_absent_leaves_seen_clear (void)
{
    struct htlc_conn htlc;
    wire_fixture_init (&htlc, HTLS_HDR_USER_SELFINFO, 1, 0);
    /* Pre-set the field to prove the parser doesn't touch it when
	 * the chunk isn't there. */
    htlc.nick_color = 0x11223344u;

    /* Unrelated chunk so the message isn't empty. */
    const guint16 some_uid = htons (1);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_UID, sizeof (some_uid), &some_uid);

    unsigned seen = hx_selfinfo_parse (&htlc, hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos);
    g_assert_false (seen & HX_SELFINFO_NICK_COLOR);
    g_assert_cmphex (htlc.nick_color, ==, 0x11223344u);

    wire_fixture_free (&htlc);
}

static void
test_selfinfo_nick_color_too_short_is_skipped (void)
{
    struct htlc_conn htlc;
    wire_fixture_init (&htlc, HTLS_HDR_USER_SELFINFO, 1, 0);
    htlc.nick_color = 0xfeedfaceu;

    const guint8 too_short[3] = { 0x11, 0x22, 0x33 };
    wire_fixture_add_chunk (&htlc, HTLS_DATA_COLOR, sizeof (too_short),
                            too_short);

    unsigned seen = hx_selfinfo_parse (&htlc, hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos);
    g_assert_false (seen & HX_SELFINFO_NICK_COLOR);
    /* Crucial: the parser must not have partial-read or zero-filled
	 * htlc->nick_color from a malformed-length chunk. */
    g_assert_cmphex (htlc.nick_color, ==, 0xfeedfaceu);

    wire_fixture_free (&htlc);
}

static void
test_selfinfo_nick_color_too_long_is_skipped (void)
{
    struct htlc_conn htlc;
    wire_fixture_init (&htlc, HTLS_HDR_USER_SELFINFO, 1, 0);
    htlc.nick_color = 0xfeedfaceu;

    const guint8 too_long[8]
        = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };
    wire_fixture_add_chunk (&htlc, HTLS_DATA_COLOR, sizeof (too_long), too_long);

    unsigned seen = hx_selfinfo_parse (&htlc, hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos);
    g_assert_false (seen & HX_SELFINFO_NICK_COLOR);
    g_assert_cmphex (htlc.nick_color, ==, 0xfeedfaceu);

    wire_fixture_free (&htlc);
}

static void
test_selfinfo_unrelated_chunks_are_ignored (void)
{
    struct htlc_conn htlc;
    wire_fixture_init (&htlc, HTLS_HDR_USER_SELFINFO, 1, 0);

    const guint16 some_uid = 99;
    wire_fixture_add_chunk (&htlc, HTLS_DATA_UID, sizeof (some_uid), &some_uid);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_CHAT_ID, sizeof (some_uid),
                            &some_uid);

    unsigned seen = hx_selfinfo_parse (&htlc, hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos);
    g_assert_cmpuint (seen, ==, 0);

    wire_fixture_free (&htlc);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/proto/selfinfo/extracts_access_bitmap",
                     test_selfinfo_extracts_access_bitmap);
    g_test_add_func ("/proto/selfinfo/skips_malformed_short_access",
                     test_selfinfo_skips_malformed_short_access);
    g_test_add_func ("/proto/selfinfo/skips_malformed_long_access",
                     test_selfinfo_skips_malformed_long_access);

    g_test_add_func ("/proto/selfinfo/extracts_user_list",
                     test_selfinfo_extracts_user_list);
    g_test_add_func ("/proto/selfinfo/uid_overrides_pre_call_value",
                     test_selfinfo_uid_overrides_pre_call_value);
    g_test_add_func ("/proto/selfinfo/long_server_name_leaves_local_intact",
                     test_selfinfo_long_server_name_leaves_local_intact);
    g_test_add_func ("/proto/selfinfo/skips_too_short_user_list",
                     test_selfinfo_skips_too_short_user_list);

    g_test_add_func ("/proto/selfinfo/parses_both_chunks",
                     test_selfinfo_parses_both_chunks);
    g_test_add_func ("/proto/selfinfo/empty_message_returns_zero",
                     test_selfinfo_empty_message_returns_zero);
    g_test_add_func ("/proto/selfinfo/unrelated_chunks_are_ignored",
                     test_selfinfo_unrelated_chunks_are_ignored);

    g_test_add_func ("/proto/selfinfo/decodes_nick_color",
                     test_selfinfo_decodes_nick_color);
    g_test_add_func ("/proto/selfinfo/nick_color_none_round_trips",
                     test_selfinfo_nick_color_none_round_trips);
    g_test_add_func ("/proto/selfinfo/nick_color_absent_leaves_seen_clear",
                     test_selfinfo_nick_color_absent_leaves_seen_clear);
    g_test_add_func ("/proto/selfinfo/nick_color_too_short_is_skipped",
                     test_selfinfo_nick_color_too_short_is_skipped);
    g_test_add_func ("/proto/selfinfo/nick_color_too_long_is_skipped",
                     test_selfinfo_nick_color_too_long_is_skipped);

    return g_test_run ();
}
