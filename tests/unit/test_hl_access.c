/*
 * tests/unit/test_hl_access.c — verify hl_access_has() decodes the
 * Hotline access bitmap correctly.
 *
 * The bitmap is 8 big-endian bytes. Bit 0 is the MSB of byte 0;
 * bit 63 is the LSB of byte 7. Constants live in src/hl_access.h
 * and were reconciled against mhxd's struct hl_access_bits.
 *
 * Coverage:
 *   - Individual known bits (file ops, chat, news, broadcast).
 *   - The "guest on hlserver" canonical vector
 *     0x60 70 00 a0 00 80 00 00 — observed live during protocol
 *     trace work; serves as a real-world fixture so future bit-
 *     constant churn gets caught.
 *   - Out-of-range bit numbers return FALSE rather than walking
 *     off the end of the array.
 */

#include "config.h"
#include <glib.h>
#include "hl_access.h"

/* Helpers — assemble a bitmap with a single bit set, or with the
 * given hex bytes. */
static void
bitmap_clear (guint8 *out)
{
    memset (out, 0, 8);
}

static void
bitmap_set_bit (guint8 *out, int bit)
{
    bitmap_clear (out);
    out[bit >> 3] = (guint8)(0x80u >> (bit & 7));
}

/* ---------- Individual known bits ---------- */

static void
test_bit_zero_is_delete_files (void)
{
    guint8 access[8];
    bitmap_set_bit (access, HL_ACCESS_DELETE_FILES);
    g_assert_true (hl_access_has (access, HL_ACCESS_DELETE_FILES));
    g_assert_false (hl_access_has (access, HL_ACCESS_UPLOAD_FILES));
    g_assert_false (hl_access_has (access, HL_ACCESS_READ_NEWS));
}

static void
test_news_bit_position (void)
{
    guint8 access[8];
    bitmap_set_bit (access, HL_ACCESS_READ_NEWS);
    /* HL_ACCESS_READ_NEWS == 20: byte 2, MSB-3-positions-down
	 * (mask 0x08). Cross-check the placement so a future renumber
	 * trips this immediately. */
    g_assert_cmphex (access[2], ==, 0x08);
    g_assert_true (hl_access_has (access, HL_ACCESS_READ_NEWS));
    g_assert_false (hl_access_has (access, HL_ACCESS_POST_NEWS));
}

static void
test_broadcast_bit_position (void)
{
    guint8 access[8];
    bitmap_set_bit (access, HL_ACCESS_CAN_BROADCAST);
    /* bit 32 = byte 4, MSB (mask 0x80). */
    g_assert_cmphex (access[4], ==, 0x80);
    g_assert_true (hl_access_has (access, HL_ACCESS_CAN_BROADCAST));
}

static void
test_send_msgs_bit_position (void)
{
    guint8 access[8];
    bitmap_set_bit (access, HL_ACCESS_SEND_MSGS);
    /* bit 40 = byte 5, MSB (mask 0x80). */
    g_assert_cmphex (access[5], ==, 0x80);
    g_assert_true (hl_access_has (access, HL_ACCESS_SEND_MSGS));
}

static void
test_all_bits_clear (void)
{
    guint8 access[8] = { 0 };
    g_assert_false (hl_access_has (access, HL_ACCESS_DELETE_FILES));
    g_assert_false (hl_access_has (access, HL_ACCESS_READ_NEWS));
    g_assert_false (hl_access_has (access, HL_ACCESS_CAN_BROADCAST));
    g_assert_false (hl_access_has (access, HL_ACCESS_SEND_MSGS));
}

static void
test_all_bits_set (void)
{
    guint8 access[8];
    memset (access, 0xff, 8);
    g_assert_true (hl_access_has (access, HL_ACCESS_DELETE_FILES));
    g_assert_true (hl_access_has (access, HL_ACCESS_READ_NEWS));
    g_assert_true (hl_access_has (access, HL_ACCESS_POST_NEWS));
    g_assert_true (hl_access_has (access, HL_ACCESS_CAN_BROADCAST));
    g_assert_true (hl_access_has (access, HL_ACCESS_SEND_MSGS));
    /* And every individual bit position 0..63. */
    for (int b = 0; b < 64; b++) {
        g_assert_true (hl_access_has (access, b));
    }
}

/* ---------- The "guest on hlserver" canonical vector ----------
 *
 * Captured live during protocol trace work — this is exactly what
 * hlserver.com sends to misha's guest account in HTLS_HDR_USER_SELFINFO:
 *
 *   chunk type=HTLC/S_DATA_ACCESS (0x006e) len=8
 *     60 70 00 a0 00 80 00 00
 *
 * Decoded:
 *   byte 0 = 0x60 = bits 1, 2 set → upload_files, download_files
 *   byte 1 = 0x70 = bits 9, 10, 11 set → read_chat, send_chat, create_pchats
 *   byte 2 = 0x00 → nothing in 16..23: NO read_news (bit 20)
 *   byte 3 = 0xa0 = bits 24, 26 set → get_user_info, use_any_name
 *   byte 4 = 0x00 → nothing in 32..39: NO broadcast (32),
 *                   NO delete_articles (33)
 *   byte 5 = 0x80 = bit 40 set → send_msgs
 *   bytes 6, 7 = 0
 *
 * If any of these assertions ever start failing, either hl_access.h's
 * bit numbering drifted or someone changed the test fixture. Either
 * way the failure tells us where to look. */
static void
test_hlserver_guest_vector (void)
{
    const guint8 access[8] = { 0x60, 0x70, 0x00, 0xa0, 0x00, 0x80, 0x00, 0x00 };

    /* Granted */
    g_assert_true (hl_access_has (access, HL_ACCESS_UPLOAD_FILES));
    g_assert_true (hl_access_has (access, HL_ACCESS_DOWNLOAD_FILES));
    g_assert_true (hl_access_has (access, HL_ACCESS_READ_CHAT));
    g_assert_true (hl_access_has (access, HL_ACCESS_SEND_CHAT));
    g_assert_true (hl_access_has (access, HL_ACCESS_CREATE_PCHATS));
    g_assert_true (hl_access_has (access, HL_ACCESS_GET_USER_INFO));
    g_assert_true (hl_access_has (access, HL_ACCESS_USE_ANY_NAME));
    g_assert_true (hl_access_has (access, HL_ACCESS_SEND_MSGS));

    /* Denied — these were the basis for the news / kick / ban
	 * gating in users.c and the toolbar. */
    g_assert_false (hl_access_has (access, HL_ACCESS_DELETE_FILES));
    g_assert_false (hl_access_has (access, HL_ACCESS_RENAME_FILES));
    g_assert_false (hl_access_has (access, HL_ACCESS_READ_NEWS));
    g_assert_false (hl_access_has (access, HL_ACCESS_POST_NEWS));
    g_assert_false (hl_access_has (access, HL_ACCESS_DISCONNECT_USERS));
    g_assert_false (hl_access_has (access, HL_ACCESS_CAN_BROADCAST));
    g_assert_false (hl_access_has (access, HL_ACCESS_DELETE_ARTICLES));
    g_assert_false (hl_access_has (access, HL_ACCESS_CREATE_USERS));
}

/* ---------- Out-of-range bit numbers ---------- */

static void
test_out_of_range_bits_return_false (void)
{
    guint8 access[8];
    memset (access, 0xff, 8); /* all bits set */
    g_assert_false (hl_access_has (access, -1));
    g_assert_false (hl_access_has (access, 64));
    g_assert_false (hl_access_has (access, 100));
    g_assert_false (hl_access_has (access, G_MAXINT));
    g_assert_false (hl_access_has (access, G_MININT));
}

/* ---------- Round-trip: every bit position 0..63 ---------- */

static void
test_every_bit_position_round_trips (void)
{
    for (int b = 0; b < 64; b++) {
        guint8 access[8];
        bitmap_set_bit (access, b);
        g_assert_true (hl_access_has (access, b));
        /* And no other bit in 0..63 is set. */
        for (int q = 0; q < 64; q++) {
            if (q == b) {
                continue;
            }
            g_assert_false (hl_access_has (access, q));
        }
    }
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/hl_access/bit_zero_is_delete_files",
                     test_bit_zero_is_delete_files);
    g_test_add_func ("/hl_access/news_bit_position", test_news_bit_position);
    g_test_add_func ("/hl_access/broadcast_bit_position",
                     test_broadcast_bit_position);
    g_test_add_func ("/hl_access/send_msgs_bit_position",
                     test_send_msgs_bit_position);
    g_test_add_func ("/hl_access/all_bits_clear", test_all_bits_clear);
    g_test_add_func ("/hl_access/all_bits_set", test_all_bits_set);
    g_test_add_func ("/hl_access/hlserver_guest_vector",
                     test_hlserver_guest_vector);
    g_test_add_func ("/hl_access/out_of_range_bits_return_false",
                     test_out_of_range_bits_return_false);
    g_test_add_func ("/hl_access/every_bit_position_round_trips",
                     test_every_bit_position_round_trips);

    return g_test_run ();
}
