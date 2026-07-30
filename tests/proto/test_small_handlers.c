/*
 * tests/proto/test_small_handlers.c — verify the small chunk-walker
 * extractors:
 *
 *   hx_user_part_extract     (uid, cid)
 *   hx_chat_subject_extract  (cid, subject)
 *   hx_chat_invite_extract   (uid, cid, name)
 *
 * These all share the "walk a fixed set of chunks, fill a struct"
 * shape that the hx_chat_extract / hx_msg_extract pair already
 * established. A combined test program keeps the meson scaffolding
 * from sprawling — there's no per-handler complexity to justify
 * separate binaries.
 */

#include "config.h"
#include <string.h>
#include <glib.h>
#include "protocol.h"
#include "hotline.h"
#include "proto_helpers.h"
#include "wire_fixture.h"

/* ---------- hx_user_part_extract ---------- */

static void
test_user_part_extracts_uid_and_cid (void)
{
    struct htlc_conn htlc;
    const guint16 uid_wire = g_htons (42);
    const guint32 cid_wire = g_htonl (3);

    wire_fixture_init (&htlc, HTLS_HDR_USER_PART, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_UID, sizeof (uid_wire), &uid_wire);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_CHAT_ID, sizeof (cid_wire),
                            &cid_wire);

    struct hx_user_part_msg pm;
    g_assert_true (hx_user_part_extract (hx_test_in (&htlc)->buf,
                                         hx_test_in (&htlc)->pos, &pm));
    g_assert_cmphex (pm.uid, ==, 42);
    g_assert_cmphex (pm.cid, ==, 3);

    wire_fixture_free (&htlc);
}

static void
test_user_part_missing_uid_defaults_to_zero (void)
{
    /* Some servers send the part-of-main-chat case as a UID-only
     * message with no CHAT_ID (cid 0 = main chat). The reverse
     * (no UID, only CID) is malformed but the extractor should
     * still parse cleanly. */
    struct htlc_conn htlc;
    const guint16 uid_wire = g_htons (5);

    wire_fixture_init (&htlc, HTLS_HDR_USER_PART, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_UID, sizeof (uid_wire), &uid_wire);

    struct hx_user_part_msg pm;
    g_assert_true (hx_user_part_extract (hx_test_in (&htlc)->buf,
                                         hx_test_in (&htlc)->pos, &pm));
    g_assert_cmphex (pm.uid, ==, 5);
    g_assert_cmphex (pm.cid, ==, 0);

    wire_fixture_free (&htlc);
}

static void
test_user_part_null_out_returns_false (void)
{
    struct htlc_conn htlc;
    wire_fixture_init (&htlc, HTLS_HDR_USER_PART, 1, 0);

    g_assert_false (hx_user_part_extract (hx_test_in (&htlc)->buf,
                                          hx_test_in (&htlc)->pos, NULL));

    wire_fixture_free (&htlc);
}

/* ---------- hx_chat_subject_extract ---------- */

static void
test_chat_subject_extracts_cid_and_subject (void)
{
    struct htlc_conn htlc;
    const guint32 cid_wire = g_htonl (7);
    const char *subject = "weekly stand-up";

    wire_fixture_init (&htlc, HTLS_HDR_CHAT_SUBJECT, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_CHAT_ID, sizeof (cid_wire),
                            &cid_wire);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_CHAT_SUBJECT, strlen (subject),
                            subject);

    struct hx_chat_subject_msg sm;
    g_assert_true (hx_chat_subject_extract (hx_test_in (&htlc)->buf,
                                            hx_test_in (&htlc)->pos, &sm));
    g_assert_cmphex (sm.cid, ==, 7);
    g_assert_cmpstr (sm.subject, ==, "weekly stand-up");
    g_assert_cmpuint (sm.subject_len, ==, strlen (subject));

    wire_fixture_free (&htlc);
}

static void
test_chat_subject_truncates_at_255 (void)
{
    struct htlc_conn htlc;
    char long_subject[400];
    memset (long_subject, 'S', sizeof (long_subject));
    wire_fixture_init (&htlc, HTLS_HDR_CHAT_SUBJECT, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_CHAT_SUBJECT,
                            sizeof (long_subject), long_subject);

    struct hx_chat_subject_msg sm;
    g_assert_true (hx_chat_subject_extract (hx_test_in (&htlc)->buf,
                                            hx_test_in (&htlc)->pos, &sm));
    g_assert_cmpuint (sm.subject_len, ==, 255);
    g_assert_cmpuint (strlen (sm.subject), ==, 255);
    for (gsize i = 0; i < 255; i++) {
        g_assert_cmphex (sm.subject[i], ==, 'S');
    }
    g_assert_cmphex (sm.subject[255], ==, '\0');

    wire_fixture_free (&htlc);
}

static void
test_chat_subject_no_sanitisation (void)
{
    /* The handler does NOT run CR2LF or strip_ansi on the subject
     * line, since subjects are short single-line text and the
     * widget already treats them inertly. Pin that down so a future
     * "consistency" pass that adds CR2LF to subjects trips this. */
    struct htlc_conn htlc;
    const char *subject = "line\rwith\rCRs and \x1b[31mansi\x1b[0m";

    wire_fixture_init (&htlc, HTLS_HDR_CHAT_SUBJECT, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_CHAT_SUBJECT, strlen (subject),
                            subject);

    struct hx_chat_subject_msg sm;
    g_assert_true (hx_chat_subject_extract (hx_test_in (&htlc)->buf,
                                            hx_test_in (&htlc)->pos, &sm));
    /* Bytes are passed through verbatim. */
    g_assert_cmpmem (sm.subject, sm.subject_len, subject, strlen (subject));

    wire_fixture_free (&htlc);
}

static void
test_chat_subject_empty_message_returns_zero_length (void)
{
    struct htlc_conn htlc;
    wire_fixture_init (&htlc, HTLS_HDR_CHAT_SUBJECT, 1, 0);

    struct hx_chat_subject_msg sm;
    g_assert_true (hx_chat_subject_extract (hx_test_in (&htlc)->buf,
                                            hx_test_in (&htlc)->pos, &sm));
    g_assert_cmphex (sm.cid, ==, 0);
    g_assert_cmpstr (sm.subject, ==, "");
    g_assert_cmpuint (sm.subject_len, ==, 0);

    wire_fixture_free (&htlc);
}

/* ---------- hx_chat_invite_extract ---------- */

static void
test_chat_invite_extracts_all_three_fields (void)
{
    struct htlc_conn htlc;
    const guint16 uid_wire = g_htons (99);
    const guint32 cid_wire = g_htonl (12);
    const char *name = "Misha";

    wire_fixture_init (&htlc, HTLS_HDR_CHAT_INVITE, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_UID, sizeof (uid_wire), &uid_wire);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_CHAT_ID, sizeof (cid_wire),
                            &cid_wire);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_NAME, strlen (name), name);

    struct hx_chat_invite_msg im;
    g_assert_true (hx_chat_invite_extract (hx_test_in (&htlc)->buf,
                                           hx_test_in (&htlc)->pos, &im));
    g_assert_cmphex (im.uid, ==, 99);
    g_assert_cmphex (im.cid, ==, 12);
    g_assert_cmpstr (im.name, ==, "Misha");
    g_assert_cmpuint (im.name_len, ==, 5);

    wire_fixture_free (&htlc);
}

static void
test_chat_invite_strips_ansi_in_name (void)
{
    struct htlc_conn htlc;
    const char name[] = "\x1b[1mMisha\x1b[0m";

    wire_fixture_init (&htlc, HTLS_HDR_CHAT_INVITE, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_NAME, sizeof (name) - 1, name);

    struct hx_chat_invite_msg im;
    g_assert_true (hx_chat_invite_extract (hx_test_in (&htlc)->buf,
                                           hx_test_in (&htlc)->pos, &im));
    g_assert_cmpstr (im.name, ==, "[[1mMisha[[0m");

    wire_fixture_free (&htlc);
}

static void
test_chat_invite_truncates_name_at_31 (void)
{
    struct htlc_conn htlc;
    char long_name[64];
    memset (long_name, 'X', sizeof (long_name));

    wire_fixture_init (&htlc, HTLS_HDR_CHAT_INVITE, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_NAME, sizeof (long_name),
                            long_name);

    struct hx_chat_invite_msg im;
    g_assert_true (hx_chat_invite_extract (hx_test_in (&htlc)->buf,
                                           hx_test_in (&htlc)->pos, &im));
    g_assert_cmpuint (im.name_len, ==, 31);
    g_assert_cmpuint (strlen (im.name), ==, 31);
    g_assert_cmphex (im.name[31], ==, '\0');

    wire_fixture_free (&htlc);
}

static void
test_chat_invite_missing_chunks_return_zero_defaults (void)
{
    struct htlc_conn htlc;
    wire_fixture_init (&htlc, HTLS_HDR_CHAT_INVITE, 1, 0);

    struct hx_chat_invite_msg im;
    g_assert_true (hx_chat_invite_extract (hx_test_in (&htlc)->buf,
                                           hx_test_in (&htlc)->pos, &im));
    g_assert_cmphex (im.uid, ==, 0);
    g_assert_cmphex (im.cid, ==, 0);
    g_assert_cmpstr (im.name, ==, "");
    g_assert_cmpuint (im.name_len, ==, 0);

    wire_fixture_free (&htlc);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/proto/user_part/extracts_uid_and_cid",
                     test_user_part_extracts_uid_and_cid);
    g_test_add_func ("/proto/user_part/missing_uid_defaults_to_zero",
                     test_user_part_missing_uid_defaults_to_zero);
    g_test_add_func ("/proto/user_part/null_out_returns_false",
                     test_user_part_null_out_returns_false);

    g_test_add_func ("/proto/chat_subject/extracts_cid_and_subject",
                     test_chat_subject_extracts_cid_and_subject);
    g_test_add_func ("/proto/chat_subject/truncates_at_255",
                     test_chat_subject_truncates_at_255);
    g_test_add_func ("/proto/chat_subject/no_sanitisation",
                     test_chat_subject_no_sanitisation);
    g_test_add_func ("/proto/chat_subject/empty_message_returns_zero_length",
                     test_chat_subject_empty_message_returns_zero_length);

    g_test_add_func ("/proto/chat_invite/extracts_all_three_fields",
                     test_chat_invite_extracts_all_three_fields);
    g_test_add_func ("/proto/chat_invite/strips_ansi_in_name",
                     test_chat_invite_strips_ansi_in_name);
    g_test_add_func ("/proto/chat_invite/truncates_name_at_31",
                     test_chat_invite_truncates_name_at_31);
    g_test_add_func ("/proto/chat_invite/missing_chunks_return_zero_defaults",
                     test_chat_invite_missing_chunks_return_zero_defaults);

    return g_test_run ();
}
