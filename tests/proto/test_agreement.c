/*
 * tests/proto/test_agreement.c — drive hx_agreement_extract against
 * canned HTLS_HDR_AGREEMENT_FILE messages.
 *
 * Servers send the agreement on connect (after login) so the user
 * can read and accept it. The handler distinguishes three cases:
 *
 *   HTLS_DATA_AGREEMENT chunk    → agreement text to display
 *   HTLS_DATA_NOAGREEMENT chunk  → server has no agreement, skip
 *   neither chunk type           → no-op (malformed message)
 *
 * The original handler used `continue` to skip non-matching chunks
 * and `return` to early-out on NOAGREEMENT — so a NOAGREEMENT chunk
 * appearing before an AGREEMENT chunk wins. This extractor
 * preserves that ordering sensitivity.
 */

#include "config.h"
#include <string.h>
#include <glib.h>
#include "protocol.h"
#include "hotline.h"
#include "proto_helpers.h"
#include "wire_fixture.h"

/* ---------- AGREEMENT chunk: text fills `out` ---------- */

static void
test_agreement_extracts_text (void)
{
    struct htlc_conn htlc;
    const char *body = "Welcome to the server.\rNo spam.\rEnjoy.";
    wire_fixture_init (&htlc, HTLS_HDR_AGREEMENT, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_AGREEMENT, strlen (body), body);

    char out[256];
    gsize out_len = 0;
    hx_agreement_result r = hx_agreement_extract (hx_test_in (&htlc)->buf,
                                                  hx_test_in (&htlc)->pos, out,
                                                  sizeof (out), &out_len);
    g_assert_cmpint (r, ==, HX_AGREEMENT_OK);
    g_assert_cmpstr (out, ==, "Welcome to the server.\nNo spam.\nEnjoy.");
    g_assert_cmpuint (out_len, ==, strlen (body));

    wire_fixture_free (&htlc);
}

static void
test_agreement_strips_ansi (void)
{
    struct htlc_conn htlc;
    const char body[] = "\x1b[1mHEADER\x1b[0m\rRules apply.";
    wire_fixture_init (&htlc, HTLS_HDR_AGREEMENT, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_AGREEMENT, sizeof (body) - 1,
                            body);

    char out[128];
    hx_agreement_result r = hx_agreement_extract (hx_test_in (&htlc)->buf,
                                                  hx_test_in (&htlc)->pos, out,
                                                  sizeof (out), NULL);
    g_assert_cmpint (r, ==, HX_AGREEMENT_OK);
    g_assert_cmpstr (out, ==, "[[1mHEADER[[0m\nRules apply.");

    wire_fixture_free (&htlc);
}

/* Truncate to the caller's buffer; preserve NUL terminator. */
static void
test_agreement_truncates_to_buffer (void)
{
    struct htlc_conn htlc;
    char big[2048];
    memset (big, 'A', sizeof (big));
    wire_fixture_init (&htlc, HTLS_HDR_AGREEMENT, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_AGREEMENT, sizeof (big), big);

    char out[64];
    gsize out_len = 0;
    hx_agreement_result r = hx_agreement_extract (hx_test_in (&htlc)->buf,
                                                  hx_test_in (&htlc)->pos, out,
                                                  sizeof (out), &out_len);
    g_assert_cmpint (r, ==, HX_AGREEMENT_OK);
    g_assert_cmpuint (out_len, ==, sizeof (out) - 1);
    g_assert_cmpuint (strlen (out), ==, sizeof (out) - 1);
    for (gsize i = 0; i < out_len; i++) {
        g_assert_cmphex (out[i], ==, 'A');
    }
    g_assert_cmphex (out[sizeof (out) - 1], ==, '\0');

    wire_fixture_free (&htlc);
}

/* ---------- NOAGREEMENT chunk: server explicitly has none ---------- */

static void
test_agreement_noagreement_chunk_returns_none (void)
{
    struct htlc_conn htlc;
    wire_fixture_init (&htlc, HTLS_HDR_AGREEMENT, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_NOAGREEMENT, 0, NULL);

    char out[64] = "untouched";
    gsize out_len = 0;
    hx_agreement_result r = hx_agreement_extract (hx_test_in (&htlc)->buf,
                                                  hx_test_in (&htlc)->pos, out,
                                                  sizeof (out), &out_len);
    g_assert_cmpint (r, ==, HX_AGREEMENT_NONE);
    g_assert_cmpstr (out, ==, "untouched"); /* not written */

    wire_fixture_free (&htlc);
}

/* ---------- Order sensitivity: NOAGREEMENT first wins ----------
 *
 * The original handler walked chunks in order, returning early on
 * NOAGREEMENT. So a server that mixes the two (a misbehaviour, but
 * we should be deterministic) gets NOAGREEMENT-wins-if-first.
 */

static void
test_agreement_noagreement_before_agreement_wins (void)
{
    struct htlc_conn htlc;
    const char *body = "ignored";
    wire_fixture_init (&htlc, HTLS_HDR_AGREEMENT, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_NOAGREEMENT, 0, NULL);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_AGREEMENT, strlen (body), body);

    char out[64];
    hx_agreement_result r = hx_agreement_extract (hx_test_in (&htlc)->buf,
                                                  hx_test_in (&htlc)->pos, out,
                                                  sizeof (out), NULL);
    g_assert_cmpint (r, ==, HX_AGREEMENT_NONE);

    wire_fixture_free (&htlc);
}

static void
test_agreement_agreement_before_noagreement_wins (void)
{
    /* The flip side: AGREEMENT first, NOAGREEMENT after — we
     * return OK with the agreement text and never see the later
     * NOAGREEMENT. */
    struct htlc_conn htlc;
    const char *body = "the rules";
    wire_fixture_init (&htlc, HTLS_HDR_AGREEMENT, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_AGREEMENT, strlen (body), body);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_NOAGREEMENT, 0, NULL);

    char out[64];
    hx_agreement_result r = hx_agreement_extract (hx_test_in (&htlc)->buf,
                                                  hx_test_in (&htlc)->pos, out,
                                                  sizeof (out), NULL);
    g_assert_cmpint (r, ==, HX_AGREEMENT_OK);
    g_assert_cmpstr (out, ==, "the rules");

    wire_fixture_free (&htlc);
}

/* ---------- Neither chunk type ---------- */

static void
test_agreement_no_relevant_chunks_returns_not_found (void)
{
    struct htlc_conn htlc;
    const guint16 some_uid = g_htons (5);
    wire_fixture_init (&htlc, HTLS_HDR_AGREEMENT, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_UID, sizeof (some_uid), &some_uid);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_CHAT_ID, sizeof (some_uid),
                            &some_uid);

    char out[64];
    hx_agreement_result r = hx_agreement_extract (hx_test_in (&htlc)->buf,
                                                  hx_test_in (&htlc)->pos, out,
                                                  sizeof (out), NULL);
    g_assert_cmpint (r, ==, HX_AGREEMENT_NOT_FOUND);

    wire_fixture_free (&htlc);
}

static void
test_agreement_empty_message_returns_not_found (void)
{
    struct htlc_conn htlc;
    wire_fixture_init (&htlc, HTLS_HDR_AGREEMENT, 1, 0);

    char out[64];
    g_assert_cmpint (hx_agreement_extract (hx_test_in (&htlc)->buf,
                                           hx_test_in (&htlc)->pos, out,
                                           sizeof (out), NULL),
                     ==, HX_AGREEMENT_NOT_FOUND);

    wire_fixture_free (&htlc);
}

/* ---------- The continue-skip behaviour ----------
 *
 * This is the case that hung on the previous dh_start macro (where
 * `continue` skipped the position increment). The macro fix made
 * `continue` correct; the fact that this test passes proves the
 * walker advances past unrelated chunks.
 */
static void
test_agreement_skips_unrelated_chunks_before_agreement (void)
{
    struct htlc_conn htlc;
    const guint16 uid_wire = g_htons (1);
    const guint32 cid_wire = g_htonl (2);
    const char *body = "real agreement";
    wire_fixture_init (&htlc, HTLS_HDR_AGREEMENT, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_UID, sizeof (uid_wire), &uid_wire);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_CHAT_ID, sizeof (cid_wire),
                            &cid_wire);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_AGREEMENT, strlen (body), body);

    char out[64];
    hx_agreement_result r = hx_agreement_extract (hx_test_in (&htlc)->buf,
                                                  hx_test_in (&htlc)->pos, out,
                                                  sizeof (out), NULL);
    g_assert_cmpint (r, ==, HX_AGREEMENT_OK);
    g_assert_cmpstr (out, ==, "real agreement");

    wire_fixture_free (&htlc);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/proto/agreement/extracts_text",
                     test_agreement_extracts_text);
    g_test_add_func ("/proto/agreement/strips_ansi",
                     test_agreement_strips_ansi);
    g_test_add_func ("/proto/agreement/truncates_to_buffer",
                     test_agreement_truncates_to_buffer);

    g_test_add_func ("/proto/agreement/noagreement_chunk_returns_none",
                     test_agreement_noagreement_chunk_returns_none);
    g_test_add_func ("/proto/agreement/noagreement_before_agreement_wins",
                     test_agreement_noagreement_before_agreement_wins);
    g_test_add_func ("/proto/agreement/agreement_before_noagreement_wins",
                     test_agreement_agreement_before_noagreement_wins);

    g_test_add_func ("/proto/agreement/no_relevant_chunks_returns_not_found",
                     test_agreement_no_relevant_chunks_returns_not_found);
    g_test_add_func ("/proto/agreement/empty_message_returns_not_found",
                     test_agreement_empty_message_returns_not_found);

    g_test_add_func ("/proto/agreement/skips_unrelated_chunks_before_agreement",
                     test_agreement_skips_unrelated_chunks_before_agreement);

    return g_test_run ();
}
