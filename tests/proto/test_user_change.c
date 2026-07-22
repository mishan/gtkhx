/*
 * tests/proto/test_user_change.c — drive hx_user_change_extract
 * against canned HTLS_HDR_USER_CHANGE payloads.
 *
 * USER_CHANGE arrives whenever a user's icon, name, or colour
 * changes (and also for "user joined" — the handler distinguishes
 * by checking whether we already know the UID). It carries five
 * chunks:
 *
 *   HTLS_DATA_UID     — the affected user
 *   HTLS_DATA_ICON    — new icon
 *   HTLS_DATA_NAME    — new name (max 31, strip_ansi-sanitised)
 *   HTLS_DATA_COLOUR  — new colour. Optional. The got_color flag
 *                       distinguishes "absent" from "explicitly 0",
 *                       so the GUI handler can preserve the prior
 *                       colour when the chunk wasn't sent.
 *   HTLS_DATA_CHAT_ID — which chat (cid 0 = main)
 */

#include "config.h"
#include <string.h>
#include <netinet/in.h>
#include <glib.h>
#include "protocol.h"
#include "hotline.h"
#include "proto_helpers.h"
#include "wire_fixture.h"

/* ---------- All five chunks present ---------- */

static void
test_user_change_extracts_all_five_chunks (void)
{
    struct htlc_conn htlc;
    const guint16 uid_wire = htons (42);
    const guint16 icon_wire = htons (412);
    const guint16 color_wire = htons (3);
    const guint32 cid_wire = htonl (0);
    const char *name = "Misha";

    wire_fixture_init (&htlc, HTLS_HDR_USER_CHANGE, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_UID, sizeof (uid_wire), &uid_wire);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_ICON, sizeof (icon_wire),
                            &icon_wire);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_NAME, strlen (name), name);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_COLOUR, sizeof (color_wire),
                            &color_wire);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_CHAT_ID, sizeof (cid_wire),
                            &cid_wire);

    struct hx_user_change_msg uc;
    g_assert_true (hx_user_change_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, &uc));
    g_assert_cmphex (uc.uid, ==, 42);
    g_assert_cmphex (uc.icon, ==, 412);
    g_assert_cmphex (uc.color, ==, 3);
    g_assert_true (uc.got_color);
    g_assert_cmphex (uc.cid, ==, 0);
    g_assert_cmpstr (uc.name, ==, "Misha");
    g_assert_cmpuint (uc.name_len, ==, 5);

    wire_fixture_free (&htlc);
}

/* ---------- got_color contract ----------
 *
 * The got_color flag matters: "color is 0 because that's the new
 * value" vs "color was not sent" produce different downstream
 * behaviour in the rcv.c handler. Pin the flag down separately.
 */

static void
test_user_change_color_chunk_present_sets_got_color (void)
{
    struct htlc_conn htlc;
    const guint16 color_wire = htons (0); /* explicit zero */
    wire_fixture_init (&htlc, HTLS_HDR_USER_CHANGE, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_COLOUR, sizeof (color_wire),
                            &color_wire);

    struct hx_user_change_msg uc;
    g_assert_true (hx_user_change_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, &uc));
    g_assert_true (uc.got_color);
    g_assert_cmphex (uc.color, ==, 0);

    wire_fixture_free (&htlc);
}

static void
test_user_change_color_chunk_absent_clears_got_color (void)
{
    struct htlc_conn htlc;
    const guint16 uid_wire = htons (5);
    wire_fixture_init (&htlc, HTLS_HDR_USER_CHANGE, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_UID, sizeof (uid_wire), &uid_wire);
    /* no COLOUR chunk */

    struct hx_user_change_msg uc;
    g_assert_true (hx_user_change_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, &uc));
    g_assert_false (uc.got_color);
    /* color stays at the default zero. */
    g_assert_cmphex (uc.color, ==, 0);

    wire_fixture_free (&htlc);
}

/* ---------- Name sanitisation + truncation ---------- */

static void
test_user_change_strips_ansi_in_name (void)
{
    struct htlc_conn htlc;
    const char name[] = "\x1b[31mMisha\x1b[0m";
    wire_fixture_init (&htlc, HTLS_HDR_USER_CHANGE, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_NAME, sizeof (name) - 1, name);

    struct hx_user_change_msg uc;
    g_assert_true (hx_user_change_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, &uc));
    g_assert_cmpstr (uc.name, ==, "[[31mMisha[[0m");

    wire_fixture_free (&htlc);
}

static void
test_user_change_truncates_long_name (void)
{
    struct htlc_conn htlc;
    char long_name[64];
    memset (long_name, 'X', sizeof (long_name));
    wire_fixture_init (&htlc, HTLS_HDR_USER_CHANGE, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_NAME, sizeof (long_name),
                            long_name);

    struct hx_user_change_msg uc;
    g_assert_true (hx_user_change_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, &uc));
    g_assert_cmpuint (uc.name_len, ==, 31);
    g_assert_cmpuint (strlen (uc.name), ==, 31);
    g_assert_cmphex (uc.name[31], ==, '\0');

    wire_fixture_free (&htlc);
}

/* ---------- Empty / minimal payloads ---------- */

static void
test_user_change_empty_payload_returns_zero_defaults (void)
{
    struct htlc_conn htlc;
    wire_fixture_init (&htlc, HTLS_HDR_USER_CHANGE, 1, 0);

    struct hx_user_change_msg uc;
    g_assert_true (hx_user_change_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, &uc));
    g_assert_cmphex (uc.uid, ==, 0);
    g_assert_cmphex (uc.icon, ==, 0);
    g_assert_cmphex (uc.color, ==, 0);
    g_assert_false (uc.got_color);
    g_assert_cmphex (uc.cid, ==, 0);
    g_assert_cmpstr (uc.name, ==, "");

    wire_fixture_free (&htlc);
}

/* ---------- Self-change (uid == htlc->uid) ----------
 *
 * The rcv.c handler at the bottom does
 *   if ((uid) && (uid == htlc->uid)) {
 *     htlc->icon = user->icon; htlc->color = user->color;
 *     strcpy (htlc->name, user->name);
 *   }
 * to detect "this is me" and propagate state into htlc. This was
 * the branch broken by the SELFINFO HN16 bug — pin it down by
 * running the extraction with a wire UID that matches a typical
 * post-SELFINFO htlc->uid. */
static void
test_user_change_self_change_uid_matches (void)
{
    struct htlc_conn htlc;
    const guint16 uid_wire = htons (5);
    wire_fixture_init (&htlc, HTLS_HDR_USER_CHANGE, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_UID, sizeof (uid_wire), &uid_wire);

    struct hx_user_change_msg uc;
    g_assert_true (hx_user_change_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, &uc));
    g_assert_cmphex (uc.uid, ==, 5);
    /* The downstream "this is me" check at rcv.c:370 is
	 * `uid == htlc->uid`. Pre-SELFINFO-fix, htlc->uid was
	 * corrupted; with the fix in place, this comparison works. */

    wire_fixture_free (&htlc);
}

/* ---------- Chunk-order independence ---------- */

static void
test_user_change_chunk_order_does_not_matter (void)
{
    struct htlc_conn htlc;
    const guint16 icon_wire = htons (123);
    const guint16 uid_wire = htons (456);
    const char *name = "Z";

    wire_fixture_init (&htlc, HTLS_HDR_USER_CHANGE, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_NAME, strlen (name), name);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_ICON, sizeof (icon_wire),
                            &icon_wire);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_UID, sizeof (uid_wire), &uid_wire);

    struct hx_user_change_msg uc;
    g_assert_true (hx_user_change_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, &uc));
    g_assert_cmphex (uc.uid, ==, 456);
    g_assert_cmphex (uc.icon, ==, 123);
    g_assert_cmpstr (uc.name, ==, "Z");

    wire_fixture_free (&htlc);
}

/* ---------- Colored-Nicknames extension ----------
 *
 * HTLS_DATA_COLOR (0x0500) is an optional u32 BE chunk carrying the
 * user's 0x00RRGGBB nick color. The parser MUST: accept exactly
 * 4 bytes and decode it big-endian into uc.nick_color + set
 * uc.got_nick_color=TRUE; ignore any other length (got_nick_color
 * stays FALSE, nick_color stays at hx_user_change_extract's
 * initialization default of HX_NICK_COLOR_NONE — NOT zero — so
 * "absent" semantically means "no color set" without aliasing
 * pure black). HX_NICK_COLOR_NONE (0xFFFFFFFF) round-trips
 * verbatim through the wire. */

static void
test_user_change_decodes_nick_color (void)
{
    struct htlc_conn htlc;
    /* 0x11223344 wire-side. Big-endian on the wire → htonl. */
    const guint32 color_wire = htonl (0x11223344u);
    wire_fixture_init (&htlc, HTLS_HDR_USER_CHANGE, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_COLOR, sizeof (color_wire),
                            &color_wire);

    struct hx_user_change_msg uc;
    g_assert_true (hx_user_change_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, &uc));
    g_assert_true (uc.got_nick_color);
    g_assert_cmphex (uc.nick_color, ==, 0x11223344u);

    wire_fixture_free (&htlc);
}

static void
test_user_change_nick_color_none_round_trips (void)
{
    struct htlc_conn htlc;
    const guint32 color_wire = htonl (HX_NICK_COLOR_NONE);
    wire_fixture_init (&htlc, HTLS_HDR_USER_CHANGE, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_COLOR, sizeof (color_wire),
                            &color_wire);

    struct hx_user_change_msg uc;
    g_assert_true (hx_user_change_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, &uc));
    g_assert_true (uc.got_nick_color);
    g_assert_cmphex (uc.nick_color, ==, HX_NICK_COLOR_NONE);

    wire_fixture_free (&htlc);
}

static void
test_user_change_nick_color_absent_clears_got_flag (void)
{
    struct htlc_conn htlc;
    const guint16 uid_wire = htons (7);
    wire_fixture_init (&htlc, HTLS_HDR_USER_CHANGE, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_UID, sizeof (uid_wire), &uid_wire);
    /* No COLOR chunk. */

    struct hx_user_change_msg uc;
    g_assert_true (hx_user_change_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, &uc));
    g_assert_false (uc.got_nick_color);
    /* Parser defaults to HX_NICK_COLOR_NONE rather than 0 — "absent"
	 * means "no color set", not "pure black". Callers gate on
	 * got_nick_color before reading, so this default is only a
	 * defence-in-depth value, but pin it down to catch a future
	 * regression that zero-fills it. */
    g_assert_cmphex (uc.nick_color, ==, HX_NICK_COLOR_NONE);

    wire_fixture_free (&htlc);
}

/* Malformed-length cases: spec pins COLOR at 4 bytes. Anything else
 * must be skipped silently — not partial-read, not zero-fill, just
 * dropped. Pin both shorter and longer lengths. */
static void
test_user_change_nick_color_too_short_is_skipped (void)
{
    struct htlc_conn htlc;
    const guint8 too_short[3] = { 0xaa, 0xbb, 0xcc };
    wire_fixture_init (&htlc, HTLS_HDR_USER_CHANGE, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_COLOR, sizeof (too_short),
                            too_short);

    struct hx_user_change_msg uc;
    g_assert_true (hx_user_change_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, &uc));
    g_assert_false (uc.got_nick_color);
    /* Malformed length must not partial-read into nick_color — it
	 * must stay at the parser's default (HX_NICK_COLOR_NONE). */
    g_assert_cmphex (uc.nick_color, ==, HX_NICK_COLOR_NONE);

    wire_fixture_free (&htlc);
}

static void
test_user_change_nick_color_too_long_is_skipped (void)
{
    struct htlc_conn htlc;
    const guint8 too_long[8]
        = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };
    wire_fixture_init (&htlc, HTLS_HDR_USER_CHANGE, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_COLOR, sizeof (too_long), too_long);

    struct hx_user_change_msg uc;
    g_assert_true (hx_user_change_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, &uc));
    g_assert_false (uc.got_nick_color);
    g_assert_cmphex (uc.nick_color, ==, HX_NICK_COLOR_NONE);

    wire_fixture_free (&htlc);
}

static void
test_user_change_null_out_returns_false (void)
{
    struct htlc_conn htlc;
    wire_fixture_init (&htlc, HTLS_HDR_USER_CHANGE, 1, 0);

    g_assert_false (hx_user_change_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, NULL));

    wire_fixture_free (&htlc);
}

/* ============================================================
 * hx_user_change_plan_resolve — FFI smoke tests. The decision logic lives in
 * Rust (hotline-proto's user_change module) with the exhaustive case coverage;
 * these two go through the real C ABI (fill the C struct, call the linked
 * symbol, read the C plan back) to pin the #[repr(C)] marshalling end to end.
 * ============================================================ */

/* Build a parsed USER_CHANGE struct without going through the wire. */
static struct hx_user_change_msg
mk_uc (guint16 uid, const char *name, guint16 icon, guint16 color,
       gboolean got_color, guint32 nick_color, gboolean got_nick_color)
{
    struct hx_user_change_msg uc;
    memset (&uc, 0, sizeof uc);
    uc.uid = uid;
    uc.icon = icon;
    uc.color = color;
    uc.got_color = got_color;
    uc.nick_color = got_nick_color ? nick_color : HX_NICK_COLOR_NONE;
    uc.got_nick_color = got_nick_color;
    uc.cid = 0;
    if (name) {
        g_strlcpy (uc.name, name, sizeof uc.name);
        uc.name_len = (guint16)strlen (uc.name);
    }
    return uc;
}

static void
test_resolve_rename_fires_notice (void)
{
    struct hx_user_change_msg uc = mk_uc (42, "Bobby", 412, 3, TRUE, 0, FALSE);
    struct hx_user_change_plan p;
    hx_user_change_plan_resolve (&uc, /*old_exists=*/TRUE, /*old_status=*/3,
                                 HX_NICK_COLOR_NONE, /*old_name=*/"Bob",
                                 /*self_uid=*/5, "Me", &p);
    g_assert_false (p.is_new);
    g_assert_true (p.do_rename_notice);
    g_assert_cmphex (p.eff_color, ==, 3);
}

static void
test_resolve_self_uid_adoption (void)
{
    /* self_uid==0 (SELFINFO didn't carry it) + the broadcast name matches
     * our nick → adopt uc.uid as ours. Exercises the name-compare + the
     * adopt/skip_self_create flag writes across both structs. */
    struct hx_user_change_msg uc = mk_uc (77, "Me", 412, 3, TRUE, 0, FALSE);
    struct hx_user_change_plan p;
    hx_user_change_plan_resolve (&uc, /*old_exists=*/FALSE, 0, HX_NICK_COLOR_NONE,
                                 NULL, /*self_uid=*/0, "Me", &p);
    g_assert_true (p.adopt_self_uid);
    g_assert_true (p.is_self);
    g_assert_true (p.skip_self_create); /* new + self → don't add our own row */
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/proto/user_change/extracts_all_five_chunks",
                     test_user_change_extracts_all_five_chunks);

    g_test_add_func ("/proto/user_change/color_chunk_present_sets_got_color",
                     test_user_change_color_chunk_present_sets_got_color);
    g_test_add_func ("/proto/user_change/color_chunk_absent_clears_got_color",
                     test_user_change_color_chunk_absent_clears_got_color);

    g_test_add_func ("/proto/user_change/strips_ansi_in_name",
                     test_user_change_strips_ansi_in_name);
    g_test_add_func ("/proto/user_change/truncates_long_name",
                     test_user_change_truncates_long_name);

    g_test_add_func ("/proto/user_change/empty_payload_returns_zero_defaults",
                     test_user_change_empty_payload_returns_zero_defaults);

    g_test_add_func ("/proto/user_change/self_change_uid_matches",
                     test_user_change_self_change_uid_matches);

    g_test_add_func ("/proto/user_change/chunk_order_does_not_matter",
                     test_user_change_chunk_order_does_not_matter);

    g_test_add_func ("/proto/user_change/decodes_nick_color",
                     test_user_change_decodes_nick_color);
    g_test_add_func ("/proto/user_change/nick_color_none_round_trips",
                     test_user_change_nick_color_none_round_trips);
    g_test_add_func ("/proto/user_change/nick_color_absent_clears_got_flag",
                     test_user_change_nick_color_absent_clears_got_flag);
    g_test_add_func ("/proto/user_change/nick_color_too_short_is_skipped",
                     test_user_change_nick_color_too_short_is_skipped);
    g_test_add_func ("/proto/user_change/nick_color_too_long_is_skipped",
                     test_user_change_nick_color_too_long_is_skipped);

    g_test_add_func ("/proto/user_change/null_out_returns_false",
                     test_user_change_null_out_returns_false);

    /* Plan-resolve FFI smoke tests (logic coverage is in the Rust crate). */
    g_test_add_func ("/proto/user_change/resolve/rename_fires_notice",
                     test_resolve_rename_fires_notice);
    g_test_add_func ("/proto/user_change/resolve/self_uid_adoption",
                     test_resolve_self_uid_adoption);

    return g_test_run ();
}
