/*
 * tests/proto/test_large_file.c — pin the wire shape of the
 * Capabilities-Large-File extension.
 *
 * Covers:
 *   - 64-bit companion field IDs are stable (DATA_FILESIZE64,
 *     DATA_OFFSET64, DATA_XFERSIZE64, DATA_FOLDER_ITEM_COUNT64)
 *   - HTXF handshake flag constants (HTXF_FLAG_LARGE_FILE,
 *     HTXF_FLAG_SIZE64)
 *   - send-side: client packs DATA_XFERSIZE64 alongside the legacy
 *     DATA_HTXF_SIZE on file_put when CAP_LARGE_FILES is active
 *   - recv-side: server reply with DATA_XFERSIZE64 round-trips
 *     correctly via the rcv.c parser's variable-width loop
 *   - HTXF handshake encoding for the three modes:
 *       legacy             16-byte, flags=0
 *       large-file < 4 GiB 16-byte, flags=HTXF_FLAG_LARGE_FILE
 *       large-file > 4 GiB 24-byte, flags=LARGE_FILE|SIZE64,
 *                          legacy length=0, 8-byte length appended
 *
 * The actual file payload (FFO fork headers with the split high/low
 * 64-bit encoding, raw-data uploads) is data-plane work — those
 * code paths run on a worker thread inside xfers.c and are
 * exercised by integration tests against a real server. This file
 * only pins the control-plane and handshake shapes.
 */

#include "config.h"
#include <string.h>
#include <stdarg.h>
#include <netinet/in.h>
#include <glib.h>
#include "protocol.h"
#include "hotline.h"
#include "proto_helpers.h"
#include "wire_fixture.h"

/* ---------- field id and flag constants ---------- */

static void
test_64bit_field_ids_are_stable (void)
{
    g_assert_cmphex (HTLC_DATA_FILESIZE64, ==, 0x01f1u);
    g_assert_cmphex (HTLS_DATA_FILESIZE64, ==, 0x01f1u);
    g_assert_cmphex (HTLC_DATA_OFFSET64, ==, 0x01f2u);
    g_assert_cmphex (HTLS_DATA_OFFSET64, ==, 0x01f2u);
    g_assert_cmphex (HTLC_DATA_XFERSIZE64, ==, 0x01f3u);
    g_assert_cmphex (HTLS_DATA_XFERSIZE64, ==, 0x01f3u);
    g_assert_cmphex (HTLC_DATA_FOLDER_ITEM_COUNT64, ==, 0x01f4u);
    g_assert_cmphex (HTLS_DATA_FOLDER_ITEM_COUNT64, ==, 0x01f4u);
}

static void
test_htxf_flag_constants_are_stable (void)
{
    g_assert_cmphex (HTXF_FLAG_LARGE_FILE, ==, 0x00000001u);
    g_assert_cmphex (HTXF_FLAG_SIZE64, ==, 0x00000002u);
}

/* ---------- send side: DATA_XFERSIZE64 on file_put ---------- */

static void
hlpack_v (struct htlc_conn *htlc, guint32 type, guint32 flag, int hc, ...)
{
    va_list ap;
    va_start (ap, hc);
    hlpack (htlc, type, flag, hc, ap);
    va_end (ap);
}

static void
flip_out_to_in (struct htlc_conn *htlc)
{
    g_free (htlc->in.buf);
    htlc->in.buf = htlc->out.buf;
    htlc->in.pos = htlc->out.len;
    htlc->out.buf = NULL;
    htlc->out.pos = 0;
    htlc->out.len = 0;
}

/* Drive hlpack with the same chunk list a large-file-mode file_put
 * would emit (NAME + clamped 32-bit XFERSIZE + 64-bit XFERSIZE64).
 * Walk the packed bytes and verify both size fields land. */
static void
test_send_xfersize64_alongside_legacy (void)
{
    struct htlc_conn htlc;
    memset (&htlc, 0, sizeof htlc);
    htlc.trans = 1;

    const char *name = "huge.bin";
    /* True size: 5 GiB = 0x140000000. Legacy clamped, 64-bit full. */
    guint64 true_size = 0x140000000ULL;
    guint32 legacy = htonl (0xFFFFFFFFu);
    guint64 size64 = GUINT64_TO_BE (true_size);

    hlpack_v (&htlc, HTLC_HDR_FILE_PUT, 0, 3,
              (int)HTLC_DATA_FILE_NAME, (guint16)strlen (name), name,
              (int)HTLC_DATA_HTXF_SIZE, 4, &legacy,
              (int)HTLC_DATA_XFERSIZE64, 8, &size64);

    flip_out_to_in (&htlc);

    int saw_legacy = 0;
    int saw_64 = 0;
    guint32 decoded_legacy = 0;
    guint64 decoded_64 = 0;

    dh_start (&htlc)
    {
        if (_type == HTLC_DATA_HTXF_SIZE) {
            g_assert_cmpuint (_len, ==, 4);
            for (guint16 i = 0; i < 4; i++) {
                decoded_legacy = (decoded_legacy << 8) | dh->data[i];
            }
            saw_legacy++;
        } else if (_type == HTLC_DATA_XFERSIZE64) {
            g_assert_cmpuint (_len, ==, 8);
            for (guint16 i = 0; i < 8; i++) {
                decoded_64 = (decoded_64 << 8) | dh->data[i];
            }
            saw_64++;
        }
    }
    dh_end ();

    g_assert_cmpint (saw_legacy, ==, 1);
    g_assert_cmpint (saw_64, ==, 1);
    g_assert_cmphex (decoded_legacy, ==, 0xFFFFFFFFu);
    g_assert_cmpuint (decoded_64, ==, true_size);

    g_free (htlc.in.buf);
}

/* ---------- recv side: DATA_XFERSIZE64 in file_get reply ---------- */

/* Replicate the dh_start parse loop in rcv_task_file_get for the
 * XFERSIZE64 chunk. Confirm the 8-byte big-endian decoder produces
 * the right 64-bit value. */
static void
test_recv_xfersize64_chunk_decode (void)
{
    struct htlc_conn htlc;
    memset (&htlc, 0, sizeof htlc);
    wire_fixture_init (&htlc, HTLS_HDR_TASK, /*trans=*/1, /*flag=*/0);

    /* 6 GiB == 0x180000000. */
    guint64 expected = 0x180000000ULL;
    guint64 size64_be = GUINT64_TO_BE (expected);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_XFERSIZE64, 8, &size64_be);

    int found = 0;
    guint64 size64 = 0;
    dh_start (&htlc)
    {
        if (_type == HTLS_DATA_XFERSIZE64 && _len >= 8) {
            for (guint16 i = 0; i < 8; i++) {
                size64 = (size64 << 8) | dh->data[i];
            }
            found++;
        }
    }
    dh_end ();

    g_assert_cmpint (found, ==, 1);
    g_assert_cmpuint (size64, ==, expected);

    wire_fixture_free (&htlc);
}

/* ---------- HTXF handshake bytes ---------- */

/* Build the 16-byte HTXF handshake by hand the way htxf_connect
 * does in legacy mode (no caps, no flags). Pin the byte layout
 * so a regression in struct htxf_hdr's wire shape shows up here. */
static void
test_htxf_handshake_legacy_16byte (void)
{
    struct htxf_hdr h;
    h.magic = htonl (HTXF_MAGIC_INT);
    h.ref = htonl (0xABCDu);
    h.len = htonl (1234567u);
    /* Legacy: high u16 = type (0 = file), low u16 = zero. */
    h.unknown = htonl ((guint32)HTXF_TYPE_FILE << 16);

    const guint8 *bytes = (const guint8 *)&h;
    g_assert_cmphex (bytes[0], ==, 'H');
    g_assert_cmphex (bytes[1], ==, 'T');
    g_assert_cmphex (bytes[2], ==, 'X');
    g_assert_cmphex (bytes[3], ==, 'F');
    /* ref */
    g_assert_cmphex (bytes[4], ==, 0x00);
    g_assert_cmphex (bytes[5], ==, 0x00);
    g_assert_cmphex (bytes[6], ==, 0xAB);
    g_assert_cmphex (bytes[7], ==, 0xCD);
    /* legacy length */
    g_assert_cmphex (bytes[8], ==, 0x00);
    g_assert_cmphex (bytes[9], ==, 0x12);
    g_assert_cmphex (bytes[10], ==, 0xD6);
    g_assert_cmphex (bytes[11], ==, 0x87);
    /* type | flags — all four bytes zero in legacy/file mode */
    g_assert_cmphex (bytes[12], ==, 0x00);
    g_assert_cmphex (bytes[13], ==, 0x00);
    g_assert_cmphex (bytes[14], ==, 0x00);
    g_assert_cmphex (bytes[15], ==, 0x00);
}

/* Large-file mode but transfer fits in 32 bits: 16-byte handshake,
 * LARGE_FILE bit set, SIZE64 not set, legacy length carries the
 * real size. The spec permits this — see "Flag relationship". */
static void
test_htxf_handshake_large_file_under_4gib (void)
{
    struct htxf_hdr h;
    h.magic = htonl (HTXF_MAGIC_INT);
    h.ref = htonl (0xABCDu);
    h.len = htonl (2000u);
    h.unknown = htonl (((guint32)HTXF_TYPE_FILE << 16) | HTXF_FLAG_LARGE_FILE);

    const guint8 *bytes = (const guint8 *)&h;
    /* Legacy length intact. */
    g_assert_cmphex (bytes[8], ==, 0x00);
    g_assert_cmphex (bytes[9], ==, 0x00);
    g_assert_cmphex (bytes[10], ==, 0x07);
    g_assert_cmphex (bytes[11], ==, 0xD0);
    /* type=0, flags = LARGE_FILE in the low byte. */
    g_assert_cmphex (bytes[12], ==, 0x00);
    g_assert_cmphex (bytes[13], ==, 0x00);
    g_assert_cmphex (bytes[14], ==, 0x00);
    g_assert_cmphex (bytes[15], ==, 0x01);
}

/* Large-file mode AND transfer exceeds 4 GiB: 24-byte handshake.
 * LARGE_FILE + SIZE64 set, legacy length zeroed, 8-byte BE length
 * appended after the 16-byte header. Per spec: "When the total
 * transfer length exceeds 0xFFFFFFFF, set the legacy length field
 * to zero." */
static void
test_htxf_handshake_large_file_over_4gib (void)
{
    /* 5 GiB FFO. */
    guint64 size64 = 0x140000066ULL;
    struct htxf_hdr h;
    h.magic = htonl (HTXF_MAGIC_INT);
    h.ref = htonl (0xABCDu);
    h.len = 0; /* zeroed when SIZE64 is set */
    h.unknown = htonl (((guint32)HTXF_TYPE_FILE << 16) | HTXF_FLAG_LARGE_FILE
                       | HTXF_FLAG_SIZE64);
    guint64 size64_be = GUINT64_TO_BE (size64);

    /* Splice the 16-byte header + 8-byte appendix into a 24-byte
	 * buffer the way htxf_connect would write it (two consecutive
	 * write() calls) and inspect the resulting bytes. */
    guint8 buf[24];
    memcpy (buf, &h, 16);
    memcpy (buf + 16, &size64_be, 8);

    /* Legacy length must be zero per spec. */
    g_assert_cmphex (buf[8], ==, 0x00);
    g_assert_cmphex (buf[9], ==, 0x00);
    g_assert_cmphex (buf[10], ==, 0x00);
    g_assert_cmphex (buf[11], ==, 0x00);
    /* Flags = LARGE_FILE | SIZE64 = 0x03 in the low byte. */
    g_assert_cmphex (buf[15], ==, 0x03);
    /* Appended 64-bit length. */
    g_assert_cmphex (buf[16], ==, 0x00);
    g_assert_cmphex (buf[17], ==, 0x00);
    g_assert_cmphex (buf[18], ==, 0x00);
    g_assert_cmphex (buf[19], ==, 0x01);
    g_assert_cmphex (buf[20], ==, 0x40);
    g_assert_cmphex (buf[21], ==, 0x00);
    g_assert_cmphex (buf[22], ==, 0x00);
    g_assert_cmphex (buf[23], ==, 0x66);
}

/* Folder transfers use HTXF_TYPE_FOLDER (=1) in the high u16 of the
 * flags field. With LARGE_FILE set we should see both: type=1 in
 * the high half, LARGE_FILE in the low byte. */
static void
test_htxf_handshake_folder_with_large_file (void)
{
    struct htxf_hdr h;
    h.magic = htonl (HTXF_MAGIC_INT);
    h.ref = htonl (0xABCDu);
    h.len = htonl (1000u);
    h.unknown
        = htonl (((guint32)HTXF_TYPE_FOLDER << 16) | HTXF_FLAG_LARGE_FILE);

    const guint8 *bytes = (const guint8 *)&h;
    /* type=1 in bytes 12-13. */
    g_assert_cmphex (bytes[12], ==, 0x00);
    g_assert_cmphex (bytes[13], ==, 0x01);
    /* flags low byte. */
    g_assert_cmphex (bytes[14], ==, 0x00);
    g_assert_cmphex (bytes[15], ==, 0x01);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/large_file/64bit_field_ids_are_stable",
                     test_64bit_field_ids_are_stable);
    g_test_add_func ("/large_file/htxf_flag_constants_are_stable",
                     test_htxf_flag_constants_are_stable);

    g_test_add_func ("/large_file/send/xfersize64_alongside_legacy",
                     test_send_xfersize64_alongside_legacy);
    g_test_add_func ("/large_file/recv/xfersize64_chunk_decode",
                     test_recv_xfersize64_chunk_decode);

    g_test_add_func ("/large_file/htxf/handshake_legacy_16byte",
                     test_htxf_handshake_legacy_16byte);
    g_test_add_func ("/large_file/htxf/handshake_large_file_under_4gib",
                     test_htxf_handshake_large_file_under_4gib);
    g_test_add_func ("/large_file/htxf/handshake_large_file_over_4gib",
                     test_htxf_handshake_large_file_over_4gib);
    g_test_add_func ("/large_file/htxf/handshake_folder_with_large_file",
                     test_htxf_handshake_folder_with_large_file);

    return g_test_run ();
}
