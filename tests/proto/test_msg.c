/*
 * tests/proto/test_msg.c — drive hx_msg_extract against canned
 * HTLS_HDR_MSG (private message) wire bodies.
 *
 * The handler in rcv.c reads three chunks:
 *   HTLS_DATA_UID  — sender's UID. 0 means "broadcast from server"
 *                    and routes via broadcastmsg() instead of the
 *                    private-message window.
 *   HTLS_DATA_NAME — sender's display name (up to 128 bytes). Goes
 *                    through strip_ansi but NOT CR2LF.
 *   HTLS_DATA_MSG  — message body (up to 8192 bytes). Both CR2LF
 *                    and strip_ansi.
 *
 * The helper does the chunk walk + sanitisation; the GUI side
 * (uid==0 broadcast routing, ignore-list lookup, plugin signal,
 * sound, last_msg_nick stash) stays in rcv.c.
 */

#include "config.h"
#include <string.h>
#include <netinet/in.h>
#include <glib.h>
#include "protocol.h"
#include "hotline.h"
#include "proto_helpers.h"
#include "wire_fixture.h"

/* ---------- Body bytes ---------- */

static void
test_msg_extracts_simple (void)
{
    struct htlc_conn htlc;
    const char *name = "Misha";
    const char *body = "are you around?";
    const guint16 uid_wire = htons (42);

    wire_fixture_init (&htlc, HTLS_HDR_MSG, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_UID, sizeof (uid_wire), &uid_wire);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_NAME, strlen (name), name);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_MSG, strlen (body), body);

    struct hx_msg_msg pm;
    g_assert_true (hx_msg_extract (&htlc, &pm));
    g_assert_cmphex (pm.uid, ==, 42);
    g_assert_cmpstr (pm.name, ==, "Misha");
    g_assert_cmpuint (pm.name_len, ==, 5);
    g_assert_cmpstr (pm.msg, ==, "are you around?");
    g_assert_cmpuint (pm.msg_len, ==, strlen (body));

    wire_fixture_free (&htlc);
}

/* ---------- CR2LF on body, NOT on name ----------
 *
 * The handler runs CR2LF on the body but explicitly does not on the
 * name — since names shouldn't have line endings anyway. Pin it
 * down: a CR in a name passes through unchanged.
 */
static void
test_msg_cr_in_body_converted (void)
{
    struct htlc_conn htlc;
    const char *body = "line one\rline two";
    wire_fixture_init (&htlc, HTLS_HDR_MSG, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_MSG, strlen (body), body);

    struct hx_msg_msg pm;
    g_assert_true (hx_msg_extract (&htlc, &pm));
    g_assert_cmpstr (pm.msg, ==, "line one\nline two");

    wire_fixture_free (&htlc);
}

static void
test_msg_cr_in_name_not_converted (void)
{
    /* Names don't go through CR2LF. A '\r' in the name survives
	 * (this is what the original handler does). */
    struct htlc_conn htlc;
    const char *name = "Mi\rsha";
    wire_fixture_init (&htlc, HTLS_HDR_MSG, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_NAME, strlen (name), name);

    struct hx_msg_msg pm;
    g_assert_true (hx_msg_extract (&htlc, &pm));
    g_assert_cmpstr (pm.name, ==, "Mi\rsha");

    wire_fixture_free (&htlc);
}

static void
test_msg_strips_ansi_in_both_name_and_body (void)
{
    struct htlc_conn htlc;
    const char name[] = "\x1b[1madmin\x1b[0m";
    const char body[] = "\x1b[31malert!\x1b[0m";
    wire_fixture_init (&htlc, HTLS_HDR_MSG, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_NAME, sizeof (name) - 1, name);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_MSG, sizeof (body) - 1, body);

    struct hx_msg_msg pm;
    g_assert_true (hx_msg_extract (&htlc, &pm));
    g_assert_cmpstr (pm.name, ==, "[[1madmin[[0m");
    g_assert_cmpstr (pm.msg, ==, "[[31malert![[0m");

    wire_fixture_free (&htlc);
}

/* ---------- UID == 0 (server broadcast) ---------- */

static void
test_msg_uid_zero_indicates_broadcast (void)
{
    /* Servers send broadcasts as HTLS_HDR_MSG with uid=0. The
	 * extractor doesn't care about the routing — that's the GUI's
	 * job — but pin down that uid stays 0 so rcv.c's branch on it
	 * still works after the extraction. */
    struct htlc_conn htlc;
    const char *body = "Server is restarting in 5 minutes";
    wire_fixture_init (&htlc, HTLS_HDR_MSG, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_MSG, strlen (body), body);

    struct hx_msg_msg pm;
    g_assert_true (hx_msg_extract (&htlc, &pm));
    g_assert_cmphex (pm.uid, ==, 0);
    g_assert_cmpstr (pm.msg, ==, "Server is restarting in 5 minutes");

    wire_fixture_free (&htlc);
}

/* ---------- Truncation: name @ 128, body @ 8192 ---------- */

static void
test_msg_truncates_long_name (void)
{
    struct htlc_conn htlc;
    char long_name[200];
    memset (long_name, 'N', sizeof (long_name));
    wire_fixture_init (&htlc, HTLS_HDR_MSG, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_NAME, sizeof (long_name),
                            long_name);

    struct hx_msg_msg pm;
    g_assert_true (hx_msg_extract (&htlc, &pm));
    g_assert_cmpuint (pm.name_len, ==, 128);
    g_assert_cmpuint (strlen (pm.name), ==, 128);
    for (gsize i = 0; i < 128; i++) {
        g_assert_cmphex (pm.name[i], ==, 'N');
    }
    g_assert_cmphex (pm.name[128], ==, '\0');

    wire_fixture_free (&htlc);
}

static void
test_msg_truncates_long_body (void)
{
    struct htlc_conn htlc;
    guint8 big[9000];
    memset (big, 'M', sizeof (big));
    wire_fixture_init (&htlc, HTLS_HDR_MSG, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_MSG, sizeof (big), big);

    struct hx_msg_msg pm;
    g_assert_true (hx_msg_extract (&htlc, &pm));
    g_assert_cmpuint (pm.msg_len, ==, 8192);
    g_assert_cmpuint (strlen (pm.msg), ==, 8192);
    g_assert_cmphex (pm.msg[8192], ==, '\0');

    wire_fixture_free (&htlc);
}

/* ---------- Missing chunks default to empty ---------- */

static void
test_msg_no_chunks_returns_empty (void)
{
    struct htlc_conn htlc;
    wire_fixture_init (&htlc, HTLS_HDR_MSG, 1, 0);

    struct hx_msg_msg pm;
    g_assert_true (hx_msg_extract (&htlc, &pm));
    g_assert_cmphex (pm.uid, ==, 0);
    g_assert_cmpstr (pm.name, ==, "");
    g_assert_cmpstr (pm.msg, ==, "");
    g_assert_cmpuint (pm.name_len, ==, 0);
    g_assert_cmpuint (pm.msg_len, ==, 0);

    wire_fixture_free (&htlc);
}

static void
test_msg_only_name_no_body (void)
{
    /* Name-only is malformed (the GUI side will probably display
	 * an empty PM), but the extractor should still parse cleanly. */
    struct htlc_conn htlc;
    const char *name = "ghost";
    wire_fixture_init (&htlc, HTLS_HDR_MSG, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_NAME, strlen (name), name);

    struct hx_msg_msg pm;
    g_assert_true (hx_msg_extract (&htlc, &pm));
    g_assert_cmpstr (pm.name, ==, "ghost");
    g_assert_cmpstr (pm.msg, ==, "");
    g_assert_cmpuint (pm.uid, ==, 0);

    wire_fixture_free (&htlc);
}

/* ---------- Chunk order independence ----------
 *
 * The handler's chunk-order tolerance is the same as for chat:
 * UID/MSG/NAME can arrive in any order, the dh_start walker just
 * dispatches by type. Pin it down — useful regression-net for any
 * future "optimise by reading specific chunks at known offsets"
 * idea.
 */
static void
test_msg_chunks_in_reverse_order (void)
{
    struct htlc_conn htlc;
    const char *name = "Misha";
    const char *body = "hello";
    const guint16 uid_wire = htons (7);

    wire_fixture_init (&htlc, HTLS_HDR_MSG, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_MSG, strlen (body), body);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_NAME, strlen (name), name);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_UID, sizeof (uid_wire), &uid_wire);

    struct hx_msg_msg pm;
    g_assert_true (hx_msg_extract (&htlc, &pm));
    g_assert_cmphex (pm.uid, ==, 7);
    g_assert_cmpstr (pm.name, ==, "Misha");
    g_assert_cmpstr (pm.msg, ==, "hello");

    wire_fixture_free (&htlc);
}

/* ---------- Unrelated chunks are ignored ---------- */

static void
test_msg_unrelated_chunks_skipped (void)
{
    struct htlc_conn htlc;
    const char *name = "x";
    const guint32 cid_wire = htonl (99);
    wire_fixture_init (&htlc, HTLS_HDR_MSG, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_CHAT_ID, sizeof (cid_wire),
                            &cid_wire);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_NAME, strlen (name), name);

    struct hx_msg_msg pm;
    g_assert_true (hx_msg_extract (&htlc, &pm));
    g_assert_cmpstr (pm.name, ==, "x");
    /* CHAT_ID didn't bleed into anything visible. */

    wire_fixture_free (&htlc);
}

/* ---------- API edge case ---------- */

static void
test_msg_null_out_returns_false (void)
{
    struct htlc_conn htlc;
    const char *body = "hi";
    wire_fixture_init (&htlc, HTLS_HDR_MSG, 1, 0);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_MSG, strlen (body), body);

    g_assert_false (hx_msg_extract (&htlc, NULL));

    wire_fixture_free (&htlc);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/proto/msg/extracts_simple", test_msg_extracts_simple);

    g_test_add_func ("/proto/msg/cr_in_body_converted",
                     test_msg_cr_in_body_converted);
    g_test_add_func ("/proto/msg/cr_in_name_not_converted",
                     test_msg_cr_in_name_not_converted);
    g_test_add_func ("/proto/msg/strips_ansi_in_both_name_and_body",
                     test_msg_strips_ansi_in_both_name_and_body);

    g_test_add_func ("/proto/msg/uid_zero_indicates_broadcast",
                     test_msg_uid_zero_indicates_broadcast);

    g_test_add_func ("/proto/msg/truncates_long_name",
                     test_msg_truncates_long_name);
    g_test_add_func ("/proto/msg/truncates_long_body",
                     test_msg_truncates_long_body);

    g_test_add_func ("/proto/msg/no_chunks_returns_empty",
                     test_msg_no_chunks_returns_empty);
    g_test_add_func ("/proto/msg/only_name_no_body",
                     test_msg_only_name_no_body);

    g_test_add_func ("/proto/msg/chunks_in_reverse_order",
                     test_msg_chunks_in_reverse_order);
    g_test_add_func ("/proto/msg/unrelated_chunks_skipped",
                     test_msg_unrelated_chunks_skipped);

    g_test_add_func ("/proto/msg/null_out_returns_false",
                     test_msg_null_out_returns_false);

    return g_test_run ();
}
