/*
 * tests/proto/test_task_error.c — drive task_error_extract against
 * canned wire-format buffers.
 *
 * task_error() runs whenever a server replies to one of our requests
 * with a flag=1 (task-error) bit set on its HTLS_HDR_TASK header.
 * The chunk we care about is HTLS_DATA_TASKERROR (0x64), whose body
 * is the human-readable error string ("Uh, no.", "Permission denied",
 * etc., depending on the server). The Tier 2 contract for the
 * extractor is:
 *
 *   1. Find the HTLS_DATA_TASKERROR chunk and copy its body into
 *      the caller's buffer.
 *   2. Run CR2LF (Hotline wire endings → unix endings).
 *   3. Run strip_ansi (control-byte sanitiser).
 *   4. NUL-terminate.
 *   5. Return TRUE; on no chunk found return FALSE without touching
 *      the buffer's tail.
 *
 * The fixture builder in wire_fixture.c packs htlc->in.buf the same
 * way hlwrite would have — header + chunked data — so the dh_start
 * walker in protocol.h sees a real Hotline message.
 */

#include "config.h"
#include <string.h>
#include <glib.h>
#include "protocol.h"
#include "hotline.h"
#include "proto_helpers.h"
#include "wire_fixture.h"

/* ---------- The classic case: one TASKERROR chunk ---------- */

static void
test_task_error_extracts_simple_message (void)
{
    struct htlc_conn htlc;
    const char *msg = "Permission denied.";
    wire_fixture_init (&htlc, HTLS_HDR_TASK, /*trans=*/42, /*flag=*/1);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_TASKERROR, strlen (msg), msg);

    char out[64];
    gsize out_len = 0;
    g_assert_true (task_error_extract (&htlc, out, sizeof (out), &out_len));
    g_assert_cmpstr (out, ==, "Permission denied.");
    g_assert_cmpuint (out_len, ==, strlen (msg));

    wire_fixture_free (&htlc);
}

/* The hlserver-on-mhxd-style "Uh, no." reply is what kicked off the
 * whole protocol-trace investigation in Phase 5. Pin it down so a
 * future change that reorders chunk parsing or drops the dh_start
 * decoder fails loudly on a known-good fixture. */
static void
test_task_error_hlserver_uh_no (void)
{
    struct htlc_conn htlc;
    const char *msg = "Uh, no.";
    wire_fixture_init (&htlc, HTLS_HDR_TASK, 12, 1);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_TASKERROR, strlen (msg), msg);

    char out[64];
    gsize out_len = 0;
    g_assert_true (task_error_extract (&htlc, out, sizeof (out), &out_len));
    g_assert_cmpstr (out, ==, "Uh, no.");
    g_assert_cmpuint (out_len, ==, 7);

    wire_fixture_free (&htlc);
}

/* ---------- CR-line-ending sanitisation ----------
 *
 * Hotline servers send '\r' between lines. The extractor must
 * convert those to '\n' before handing the string off to the toast.
 * If a server ever sends a multi-line task error, we don't want it
 * to display as one mashed-together line.
 */
static void
test_task_error_converts_cr_to_lf (void)
{
    struct htlc_conn htlc;
    const char *msg = "line one\rline two\rline three";
    wire_fixture_init (&htlc, HTLS_HDR_TASK, 1, 1);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_TASKERROR, strlen (msg), msg);

    char out[128];
    g_assert_true (task_error_extract (&htlc, out, sizeof (out), NULL));
    g_assert_cmpstr (out, ==, "line one\nline two\nline three");

    wire_fixture_free (&htlc);
}

/* ---------- ANSI / control-byte sanitisation ----------
 *
 * Some servers append ANSI colour escapes to error strings. The
 * extractor's strip_ansi() pass maps them to printable bytes so
 * Pango doesn't choke on the embedded ESC.
 */
static void
test_task_error_strips_ansi (void)
{
    struct htlc_conn htlc;
    const char input[] = "\x1b[31mforbidden\x1b[0m";
    const char expected[] = "[[31mforbidden[[0m";
    wire_fixture_init (&htlc, HTLS_HDR_TASK, 1, 1);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_TASKERROR, sizeof (input) - 1,
                            input);

    char out[64];
    g_assert_true (task_error_extract (&htlc, out, sizeof (out), NULL));
    g_assert_cmpstr (out, ==, expected);

    wire_fixture_free (&htlc);
}

/* ---------- No TASKERROR chunk → returns FALSE ----------
 *
 * If the message has chunks but none of them are HTLS_DATA_TASKERROR
 * (e.g. the server sends a UID and CID but forgets the error body),
 * the extractor returns FALSE without writing to out.
 */
static void
test_task_error_no_taskerror_chunk_returns_false (void)
{
    struct htlc_conn htlc;
    const guint16 some_uid = 42;
    wire_fixture_init (&htlc, HTLS_HDR_TASK, 1, 1);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_UID, sizeof (some_uid), &some_uid);

    char out[64] = "untouched";
    g_assert_false (task_error_extract (&htlc, out, sizeof (out), NULL));
    /* The extractor doesn't write to out on FALSE. We can't assert
	 * exact contents because the contract says "MUST NOT read out",
	 * but we can at least confirm we still have a valid C string at
	 * the start. */
    g_assert_cmpstr (out, ==, "untouched");

    wire_fixture_free (&htlc);
}

static void
test_task_error_empty_message_returns_false (void)
{
    /* No chunks at all. dh_start walks zero iterations, found
	 * stays FALSE. */
    struct htlc_conn htlc;
    wire_fixture_init (&htlc, HTLS_HDR_TASK, 1, 1);

    char out[64];
    gsize out_len = 99;
    g_assert_false (task_error_extract (&htlc, out, sizeof (out), &out_len));
    g_assert_cmpuint (out_len, ==, 99); /* unchanged */

    wire_fixture_free (&htlc);
}

/* ---------- Truncation: message larger than out buffer ---------- */

static void
test_task_error_truncates_to_buffer_size (void)
{
    struct htlc_conn htlc;
    /* 100-byte message, but we only give the extractor 16 bytes
	 * (15 + NUL). It should truncate. */
    char msg[100];
    memset (msg, 'A', sizeof (msg));
    wire_fixture_init (&htlc, HTLS_HDR_TASK, 1, 1);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_TASKERROR, sizeof (msg), msg);

    char out[16];
    gsize out_len = 0;
    g_assert_true (task_error_extract (&htlc, out, sizeof (out), &out_len));
    g_assert_cmpuint (out_len, ==, sizeof (out) - 1);
    g_assert_cmpuint (strlen (out), ==, sizeof (out) - 1);
    for (gsize i = 0; i < out_len; i++) {
        g_assert_cmphex (out[i], ==, 'A');
    }
    g_assert_cmphex (out[sizeof (out) - 1], ==, '\0');

    wire_fixture_free (&htlc);
}

/* ---------- Multiple chunks: the first TASKERROR wins ----------
 *
 * Servers shouldn't send two TASKERROR chunks in one reply, but if
 * one ever does we should pick the first and ignore the rest. The
 * extractor short-circuits via the `found` guard.
 */
static void
test_task_error_first_taskerror_wins_when_duplicated (void)
{
    struct htlc_conn htlc;
    const char *first = "first error";
    const char *second = "ignored";
    wire_fixture_init (&htlc, HTLS_HDR_TASK, 1, 1);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_TASKERROR, strlen (first), first);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_TASKERROR, strlen (second),
                            second);

    char out[64];
    g_assert_true (task_error_extract (&htlc, out, sizeof (out), NULL));
    g_assert_cmpstr (out, ==, "first error");

    wire_fixture_free (&htlc);
}

/* ---------- Other chunks alongside TASKERROR are skipped ---------- */

static void
test_task_error_skips_unrelated_chunks (void)
{
    struct htlc_conn htlc;
    const guint16 some_uid = 17;
    const char *msg = "the real error";
    wire_fixture_init (&htlc, HTLS_HDR_TASK, 1, 1);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_UID, sizeof (some_uid), &some_uid);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_TASKERROR, strlen (msg), msg);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_CHAT_ID, sizeof (some_uid),
                            &some_uid);

    char out[64];
    g_assert_true (task_error_extract (&htlc, out, sizeof (out), NULL));
    g_assert_cmpstr (out, ==, "the real error");

    wire_fixture_free (&htlc);
}

/* ---------- API edge cases ---------- */

static void
test_task_error_null_out_returns_false (void)
{
    struct htlc_conn htlc;
    const char *msg = "anything";
    wire_fixture_init (&htlc, HTLS_HDR_TASK, 1, 1);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_TASKERROR, strlen (msg), msg);

    g_assert_false (task_error_extract (&htlc, NULL, 64, NULL));

    wire_fixture_free (&htlc);
}

static void
test_task_error_zero_out_size_returns_false (void)
{
    struct htlc_conn htlc;
    const char *msg = "anything";
    wire_fixture_init (&htlc, HTLS_HDR_TASK, 1, 1);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_TASKERROR, strlen (msg), msg);

    char out[64];
    g_assert_false (task_error_extract (&htlc, out, 0, NULL));

    wire_fixture_free (&htlc);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/proto/task_error/extracts_simple_message",
                     test_task_error_extracts_simple_message);
    g_test_add_func ("/proto/task_error/hlserver_uh_no",
                     test_task_error_hlserver_uh_no);

    g_test_add_func ("/proto/task_error/converts_cr_to_lf",
                     test_task_error_converts_cr_to_lf);
    g_test_add_func ("/proto/task_error/strips_ansi",
                     test_task_error_strips_ansi);

    g_test_add_func ("/proto/task_error/no_taskerror_chunk_returns_false",
                     test_task_error_no_taskerror_chunk_returns_false);
    g_test_add_func ("/proto/task_error/empty_message_returns_false",
                     test_task_error_empty_message_returns_false);

    g_test_add_func ("/proto/task_error/truncates_to_buffer_size",
                     test_task_error_truncates_to_buffer_size);
    g_test_add_func ("/proto/task_error/first_taskerror_wins_when_duplicated",
                     test_task_error_first_taskerror_wins_when_duplicated);
    g_test_add_func ("/proto/task_error/skips_unrelated_chunks",
                     test_task_error_skips_unrelated_chunks);

    g_test_add_func ("/proto/task_error/null_out_returns_false",
                     test_task_error_null_out_returns_false);
    g_test_add_func ("/proto/task_error/zero_out_size_returns_false",
                     test_task_error_zero_out_size_returns_false);

    return g_test_run ();
}
