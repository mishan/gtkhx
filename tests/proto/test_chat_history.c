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
#include <glib.h>
#include "chat_history.h"

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

    /* The send-side cases (cap-gate + cursor / limit chunk shape + opcode)
     * moved to the hxhandlers Rust crate's send/chat_history.rs unit tests
     * when hx_get_chat_history became Rust — they run under
     * `cargo test -p hxhandlers`. This proto test keeps the parser cases,
     * which drive the Rust hx_history_entry_parse through its C ABI. */

    return g_test_run ();
}
