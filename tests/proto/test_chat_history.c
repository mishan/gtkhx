/*
 * tests/proto/test_chat_history.c — pin the wire shape of the
 * chat-history extension (fogWraith
 * Capabilities-Chat-History.md):
 *
 *   parse  hx_history_entry_parse decodes a packed-binary
 *          DATA_HISTORY_ENTRY (0x0F05) chunk into an
 *          HxHistoryEntry. We hand-build entries with known
 *          values and assert every field round-trips.
 *
 *   send   hx_get_chat_history packs the right HTLC_DATA_*
 *          chunks into an HTLC_HDR_GET_CHAT_HISTORY (700)
 *          request. Drive it with various cursor/limit
 *          combinations and verify the wire-side chunk shape.
 *
 *   subs   The mini-TLV sub-field skip path — append a known
 *          + unknown sub-field after the message body and
 *          verify the parser walks past both cleanly and
 *          doesn't surface unknown-type data.
 *
 *   bad    Defensive coverage: short header, nick_len past
 *          buffer, msg_len past buffer all return NULL.
 *
 * The header pin matches the pattern in test_capabilities.c —
 * if these constants ever change silently, the wire flip on
 * deployed servers will be the next thing to break.
 */

#include "config.h"
#include <string.h>
#include <stdarg.h>
#include <netinet/in.h>
#include <glib.h>
#include "protocol.h"
#include "hotline.h"
#include "htlc_recv_buf.h"
#include "proto_helpers.h"
#include "chat_history.h"

/* The production `hlwrite` lives in network.c, which pulls in
 * GIOChannel + proto_trace + cipher + compress + connection-state
 * signal emission. None of that is needed (or wanted) here; the
 * test only cares about the buffer hlwrite produces. Provide a
 * minimal in-test stub that does exactly what the production
 * function does for the buffer side — call hlpack — and skip
 * everything else. Linker picks this stub because we don't link
 * network.c.
 *
 * Same trick for hlwrite_chunks (the array-style variant chat_history
 * now uses after the LOGIN-fork removal — see commit message
 * "tests: end the LOGIN-packet fork between production and the
 * harness"). hlpack_chunks does the buffer side; everything else is
 * stubbed out. */
void
hlwrite (struct htlc_conn *htlc, guint32 type, guint32 flag, int hc, ...)
{
    va_list ap;
    va_start (ap, hc);
    gsize len = 0;
    guint8 *buf = hlpack (htlc, type, flag, hc, ap, &len);
    va_end (ap);
    /* Stash the packed frame into htlc->in so the tests can walk it
     * (hlpack now returns a fresh buffer — there's no htlc->out). */
    g_free (hx_test_in(htlc)->buf);
    hx_test_in(htlc)->buf = buf;
    hx_test_in(htlc)->pos = len;
}

/* Production declaration lives in network.h, which we deliberately
 * don't include (it would drag in GIOChannel + proto_trace + cipher
 * + compress). Forward-declare here so -Wmissing-prototypes is
 * happy — same pattern integration_harness.c uses. */
extern void hlwrite_chunks (struct htlc_conn *htlc, guint32 type,
                            guint32 flag, const struct hx_chunk *chunks,
                            int hc);
void
hlwrite_chunks (struct htlc_conn *htlc, guint32 type, guint32 flag,
                const struct hx_chunk *chunks, int hc)
{
    gsize len = 0;
    guint8 *buf = hlpack_chunks (htlc, type, flag, chunks, hc, &len);
    g_free (hx_test_in(htlc)->buf);
    hx_test_in(htlc)->buf = buf;
    hx_test_in(htlc)->pos = len;
}

/* ---------- entry-buffer construction helper ---------- */

/* Build a packed entry into `out` (caller-owned, big enough).
 * Returns the number of bytes written. Sub-field bytes (if any)
 * are appended verbatim — pass them already-packed. */
static gsize
pack_entry (guint8 *out,
            guint64 message_id, gint64 timestamp,
            guint16 flags, guint16 icon_id,
            const char *nick, guint16 nick_len,
            const char *message, guint16 msg_len,
            const guint8 *subs, gsize subs_len)
{
    gsize off = 0;

    /* Fixed header — manually pack big-endian. */
    for (int i = 7; i >= 0; i--) {
        out[off++] = (guint8) ((message_id >> (i * 8)) & 0xff);
    }
    for (int i = 7; i >= 0; i--) {
        out[off++] = (guint8) ((((guint64) timestamp) >> (i * 8)) & 0xff);
    }
    out[off++] = (guint8) (flags >> 8);
    out[off++] = (guint8) (flags & 0xff);
    out[off++] = (guint8) (icon_id >> 8);
    out[off++] = (guint8) (icon_id & 0xff);
    out[off++] = (guint8) (nick_len >> 8);
    out[off++] = (guint8) (nick_len & 0xff);

    if (nick_len) {
        memcpy (out + off, nick, nick_len);
        off += nick_len;
    }

    out[off++] = (guint8) (msg_len >> 8);
    out[off++] = (guint8) (msg_len & 0xff);

    if (msg_len) {
        memcpy (out + off, message, msg_len);
        off += msg_len;
    }

    if (subs && subs_len) {
        memcpy (out + off, subs, subs_len);
        off += subs_len;
    }

    return off;
}

/* ---------- parse-side tests ---------- */

static void
test_parse_minimum_entry (void)
{
    /* The spec's stated minimum: 24 bytes with empty nick + msg. */
    guint8 buf[24];
    gsize len = pack_entry (buf, /*id=*/1, /*ts=*/0, /*flags=*/0,
                            /*icon=*/0, /*nick=*/NULL, 0,
                            /*msg=*/NULL, 0, NULL, 0);
    g_assert_cmpuint (len, ==, 24);

    HxHistoryEntry *e = hx_history_entry_parse (buf, len);
    g_assert_nonnull (e);
    g_assert_cmpuint (e->message_id, ==, 1);
    g_assert_cmpint  (e->timestamp,  ==, 0);
    g_assert_cmphex  (e->flags,      ==, 0);
    g_assert_cmpuint (e->icon_id,    ==, 0);
    g_assert_cmpuint (e->nick_len,   ==, 0);
    g_assert_cmpstr  (e->nick,       ==, "");
    g_assert_cmpuint (e->message_len, ==, 0);
    g_assert_cmpstr  (e->message,    ==, "");
    hx_history_entry_free (e);
}

static void
test_parse_typical_entry (void)
{
    /* From the spec example: greg saying "Hello everyone" at
     * Unix epoch 1729137536 (2024-10-17T12:32:16Z), icon 128. */
    guint8 buf[128];
    const char *nick = "greg";
    const char *msg  = "Hello everyone";
    gsize len = pack_entry (buf, /*id=*/1000, /*ts=*/1729137536,
                            /*flags=*/0, /*icon=*/128,
                            nick, 4, msg, 14, NULL, 0);

    HxHistoryEntry *e = hx_history_entry_parse (buf, len);
    g_assert_nonnull (e);
    g_assert_cmpuint (e->message_id, ==, 1000);
    g_assert_cmpint  (e->timestamp,  ==, 1729137536);
    g_assert_cmphex  (e->flags,      ==, 0);
    g_assert_cmpuint (e->icon_id,    ==, 128);
    g_assert_cmpuint (e->nick_len,   ==, 4);
    g_assert_cmpstr  (e->nick,       ==, "greg");
    g_assert_cmpuint (e->message_len, ==, 14);
    g_assert_cmpstr  (e->message,    ==, "Hello everyone");
    hx_history_entry_free (e);
}

static void
test_parse_flag_bits_decode (void)
{
    /* All three known flag bits set simultaneously. */
    guint8 buf[64];
    gsize len = pack_entry (
        buf, /*id=*/77, /*ts=*/-1, /*flags=*/(HX_HISTORY_FLAG_ACTION
                                              | HX_HISTORY_FLAG_SERVER_MSG
                                              | HX_HISTORY_FLAG_DELETED),
        /*icon=*/0, NULL, 0, NULL, 0, NULL, 0);
    HxHistoryEntry *e = hx_history_entry_parse (buf, len);
    g_assert_nonnull (e);
    g_assert_true  (e->flags & HX_HISTORY_FLAG_ACTION);
    g_assert_true  (e->flags & HX_HISTORY_FLAG_SERVER_MSG);
    g_assert_true  (e->flags & HX_HISTORY_FLAG_DELETED);
    /* Signed timestamp preserved across the cast. */
    g_assert_cmpint (e->timestamp, ==, -1);
    hx_history_entry_free (e);
}

static void
test_parse_skips_subfields (void)
{
    /* Append two mini-TLV sub-fields after the message body. The
     * parser walks past them silently — no errors, no
     * surfacing. Use unknown sub-types to prove forward-compat. */
    guint8 subs[] = {
        /* sub 1: type=0x0001, len=4, data=DEADBEEF */
        0x00, 0x01, 0x00, 0x04, 0xde, 0xad, 0xbe, 0xef,
        /* sub 2: type=0xABCD, len=0 (degenerate empty sub-field) */
        0xab, 0xcd, 0x00, 0x00,
    };
    guint8 buf[128];
    gsize len = pack_entry (buf, /*id=*/10, /*ts=*/0, /*flags=*/0,
                            /*icon=*/0, "x", 1, "y", 1,
                            subs, sizeof subs);

    HxHistoryEntry *e = hx_history_entry_parse (buf, len);
    g_assert_nonnull (e);
    g_assert_cmpstr (e->nick,    ==, "x");
    g_assert_cmpstr (e->message, ==, "y");
    hx_history_entry_free (e);
}

static void
test_parse_short_header_returns_null (void)
{
    /* 23 bytes is one short of the minimum entry. */
    guint8 buf[23];
    memset (buf, 0, sizeof buf);
    HxHistoryEntry *e = hx_history_entry_parse (buf, sizeof buf);
    g_assert_null (e);
}

static void
test_parse_nick_len_past_buffer_returns_null (void)
{
    /* nick_len claims 10 bytes but the buffer only has 24 total
     * — not enough room for header + nick + msg_len. */
    guint8 buf[24];
    memset (buf, 0, sizeof buf);
    /* nick_len at offset 20 */
    buf[20] = 0x00;
    buf[21] = 0x0a; /* nick_len = 10 */
    HxHistoryEntry *e = hx_history_entry_parse (buf, sizeof buf);
    g_assert_null (e);
}

static void
test_parse_msg_len_past_buffer_returns_null (void)
{
    /* nick_len fits, msg_len lies. */
    guint8 buf[26];
    memset (buf, 0, sizeof buf);
    /* nick_len = 0, msg_len = 100 */
    buf[20] = 0x00;
    buf[21] = 0x00;
    buf[22] = 0x00;
    buf[23] = 0x64; /* msg_len = 100 */
    HxHistoryEntry *e = hx_history_entry_parse (buf, sizeof buf);
    g_assert_null (e);
}

static void
test_parse_null_data_returns_null (void)
{
    g_assert_null (hx_history_entry_parse (NULL, 24));
    g_assert_null (hx_history_entry_parse ((const guint8 *) "x", 0));
}

/* ---------- send-side tests ---------- */

static void
htlc_init (struct htlc_conn *htlc, guint64 caps)
{
    memset (htlc, 0, sizeof (*htlc));
    htlc->trans = 1;
    htlc->caps  = caps;
}

static void
htlc_free (struct htlc_conn *htlc)
{
    g_free (hx_test_in(htlc)->buf);
    hx_test_in(htlc)->buf = NULL;
}

static void
test_send_skipped_without_cap (void)
{
    /* No CAP_CHAT_HISTORY in htlc->caps → sender refuses. */
    struct htlc_conn htlc;
    htlc_init (&htlc, /*caps=*/0);

    g_assert_false (hx_get_chat_history (&htlc, 0, 0, 0, 0));
    /* Nothing got written. */
    g_assert_cmpuint (hx_test_in(&htlc)->pos, ==, 0);

    htlc_free (&htlc);
}

static void
test_send_bare_request (void)
{
    /* Negotiated cap is set; bare request with no cursors / limit.
     * Wire should carry exactly one chunk: DATA_CHANNEL_ID=0. */
    struct htlc_conn htlc;
    htlc_init (&htlc, HTLC_CAP_CHAT_HISTORY);

    g_assert_true (hx_get_chat_history (&htlc, /*channel=*/0,
                                        /*before=*/0, /*after=*/0,
                                        /*limit=*/0));


    int saw_channel = 0;
    int total = 0;
    dh_start (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos)
    {
        total++;
        switch (_type) {
        case HTLC_DATA_CHANNEL_ID:
            saw_channel++;
            g_assert_cmpuint (_len, ==, 4);
            /* big-endian u32 == 0 */
            for (int i = 0; i < 4; i++) {
                g_assert_cmpuint (dh->data[i], ==, 0);
            }
            break;
        default:
            g_error ("unexpected chunk type 0x%04x in bare request", _type);
            break;
        }
    }
    dh_end ();
    g_assert_cmpint (saw_channel, ==, 1);
    g_assert_cmpint (total, ==, 1);

    htlc_free (&htlc);
}

static void
test_send_with_before_cursor (void)
{
    /* "Scroll back" — channel 0, before=1000, limit=50. */
    struct htlc_conn htlc;
    htlc_init (&htlc, HTLC_CAP_CHAT_HISTORY);

    g_assert_true (hx_get_chat_history (&htlc, /*channel=*/0,
                                        /*before=*/1000,
                                        /*after=*/0, /*limit=*/50));


    int saw_channel = 0, saw_before = 0, saw_limit = 0, saw_after = 0;
    dh_start (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos)
    {
        switch (_type) {
        case HTLC_DATA_CHANNEL_ID:
            saw_channel++;
            g_assert_cmpuint (_len, ==, 4);
            break;
        case HTLC_DATA_HISTORY_BEFORE:
            saw_before++;
            g_assert_cmpuint (_len, ==, 8);
            /* Big-endian u64 == 1000 → last byte = 0xe8, 2nd-last
             * = 0x03, rest zero. */
            g_assert_cmpuint (dh->data[6], ==, 0x03);
            g_assert_cmpuint (dh->data[7], ==, 0xe8);
            break;
        case HTLC_DATA_HISTORY_AFTER:
            saw_after++;
            break;
        case HTLC_DATA_HISTORY_LIMIT:
            saw_limit++;
            g_assert_cmpuint (_len, ==, 2);
            g_assert_cmpuint (dh->data[0], ==, 0);
            g_assert_cmpuint (dh->data[1], ==, 50);
            break;
        }
    }
    dh_end ();
    g_assert_cmpint (saw_channel, ==, 1);
    g_assert_cmpint (saw_before,  ==, 1);
    g_assert_cmpint (saw_after,   ==, 0);
    g_assert_cmpint (saw_limit,   ==, 1);

    htlc_free (&htlc);
}

static void
test_send_with_after_cursor (void)
{
    /* "Catch up after reconnect" — channel 0, after=5000. */
    struct htlc_conn htlc;
    htlc_init (&htlc, HTLC_CAP_CHAT_HISTORY);

    g_assert_true (hx_get_chat_history (&htlc, 0, /*before=*/0,
                                        /*after=*/5000, /*limit=*/0));


    int saw_after = 0;
    dh_start (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos)
    {
        if (_type == HTLC_DATA_HISTORY_AFTER) {
            saw_after++;
            g_assert_cmpuint (_len, ==, 8);
            /* big-endian u64 == 5000 → 0x00...0x13 0x88 */
            g_assert_cmpuint (dh->data[6], ==, 0x13);
            g_assert_cmpuint (dh->data[7], ==, 0x88);
        }
    }
    dh_end ();
    g_assert_cmpint (saw_after, ==, 1);

    htlc_free (&htlc);
}

static void
test_send_with_range_query (void)
{
    /* Range query — both before AND after set. */
    struct htlc_conn htlc;
    htlc_init (&htlc, HTLC_CAP_CHAT_HISTORY);

    g_assert_true (hx_get_chat_history (&htlc, 0, /*before=*/600,
                                        /*after=*/200, /*limit=*/50));


    int saw_before = 0, saw_after = 0, saw_limit = 0;
    dh_start (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos)
    {
        if (_type == HTLC_DATA_HISTORY_BEFORE) saw_before++;
        if (_type == HTLC_DATA_HISTORY_AFTER)  saw_after++;
        if (_type == HTLC_DATA_HISTORY_LIMIT)  saw_limit++;
    }
    dh_end ();
    g_assert_cmpint (saw_before, ==, 1);
    g_assert_cmpint (saw_after,  ==, 1);
    g_assert_cmpint (saw_limit,  ==, 1);

    htlc_free (&htlc);
}

static void
test_send_request_type_and_trans (void)
{
    /* The on-wire transaction type must be 700 (HTLC_HDR_GET_CHAT_HISTORY). */
    struct htlc_conn htlc;
    htlc_init (&htlc, HTLC_CAP_CHAT_HISTORY);

    g_assert_true (hx_get_chat_history (&htlc, 0, 0, 0, 0));

    /* hl_hdr starts with guint32 type at offset 0 (big-endian on
     * the wire — see struct hl_hdr in hotline.h). */
    g_assert_cmpuint (hx_test_in(&htlc)->pos, >=, 4);
    guint32 hdr_type = ((guint32) hx_test_in(&htlc)->buf[0] << 24)
                     | ((guint32) hx_test_in(&htlc)->buf[1] << 16)
                     | ((guint32) hx_test_in(&htlc)->buf[2] << 8)
                     |  (guint32) hx_test_in(&htlc)->buf[3];
    g_assert_cmphex (hdr_type, ==, HTLC_HDR_GET_CHAT_HISTORY);

    htlc_free (&htlc);
}

/* ---------- main ---------- */

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/proto/chat_history/parse/minimum",
                     test_parse_minimum_entry);
    g_test_add_func ("/proto/chat_history/parse/typical",
                     test_parse_typical_entry);
    g_test_add_func ("/proto/chat_history/parse/flags",
                     test_parse_flag_bits_decode);
    g_test_add_func ("/proto/chat_history/parse/subfields-skipped",
                     test_parse_skips_subfields);
    g_test_add_func ("/proto/chat_history/parse/short-header-null",
                     test_parse_short_header_returns_null);
    g_test_add_func ("/proto/chat_history/parse/nick-overflow-null",
                     test_parse_nick_len_past_buffer_returns_null);
    g_test_add_func ("/proto/chat_history/parse/msg-overflow-null",
                     test_parse_msg_len_past_buffer_returns_null);
    g_test_add_func ("/proto/chat_history/parse/null-input",
                     test_parse_null_data_returns_null);

    g_test_add_func ("/proto/chat_history/send/skipped-without-cap",
                     test_send_skipped_without_cap);
    g_test_add_func ("/proto/chat_history/send/bare-request",
                     test_send_bare_request);
    g_test_add_func ("/proto/chat_history/send/before-cursor",
                     test_send_with_before_cursor);
    g_test_add_func ("/proto/chat_history/send/after-cursor",
                     test_send_with_after_cursor);
    g_test_add_func ("/proto/chat_history/send/range-query",
                     test_send_with_range_query);
    g_test_add_func ("/proto/chat_history/send/header-type",
                     test_send_request_type_and_trans);

    return g_test_run ();
}
