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
    g_assert_true (hx_user_change_extract (&htlc, &uc));
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
    g_assert_true (hx_user_change_extract (&htlc, &uc));
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
    g_assert_true (hx_user_change_extract (&htlc, &uc));
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
    g_assert_true (hx_user_change_extract (&htlc, &uc));
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
    g_assert_true (hx_user_change_extract (&htlc, &uc));
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
    g_assert_true (hx_user_change_extract (&htlc, &uc));
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
    g_assert_true (hx_user_change_extract (&htlc, &uc));
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
    g_assert_true (hx_user_change_extract (&htlc, &uc));
    g_assert_cmphex (uc.uid, ==, 456);
    g_assert_cmphex (uc.icon, ==, 123);
    g_assert_cmpstr (uc.name, ==, "Z");

    wire_fixture_free (&htlc);
}

static void
test_user_change_null_out_returns_false (void)
{
    struct htlc_conn htlc;
    wire_fixture_init (&htlc, HTLS_HDR_USER_CHANGE, 1, 0);

    g_assert_false (hx_user_change_extract (&htlc, NULL));

    wire_fixture_free (&htlc);
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

    g_test_add_func ("/proto/user_change/null_out_returns_false",
                     test_user_change_null_out_returns_false);

    return g_test_run ();
}
