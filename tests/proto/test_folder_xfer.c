/*
 * tests/proto/test_folder_xfer.c — wire-format lockdown for the
 * Hotline 1.5 folder-transfer opcodes (HTLC_HDR_FILE_GETFOLDER 0xd2
 * and HTLC_HDR_FILE_PUTFOLDER 0xd5) plus the HTLS task-reply parser
 * that rcv_task_folder_get / rcv_task_folder_put run.
 *
 * Two halves:
 *
 *   1. SEND side: pack a representative GETFOLDER and PUTFOLDER
 *      request via hlpack (mirrors hx_get_folder / hx_put_folder in
 *      files.c) and verify each named chunk lands in the buffer
 *      with the expected type code, length, and payload bytes. This
 *      regression-locks the opcode and chunk IDs we shipped — every
 *      one of them is a magic number drawn from the Hotline 1.5
 *      protocol catalogue (HTLC_HDR_FILE_GETFOLDER = 0xd2,
 *      HTLC_DATA_FILE_NFILES = 0xdc, etc.). Rename one by accident
 *      and every folder transfer breaks against every server.
 *
 *   2. RECEIVE side: hand-build a synthetic HTLS reply (the kind a
 *      server sends after accepting GETFOLDER / PUTFOLDER) and walk
 *      it through the dh_start / dh_getint cycle the rcv handlers
 *      use. Verifies HTXF_REF / HTXF_SIZE / QUEUE / FILE_NFILES all
 *      decode to the expected u32 values regardless of chunk order.
 *
 * The actual rcv_task_folder_get / rcv_task_folder_put in rcv.c
 * couple the parse to xfers[] bookkeeping + signal emission, which
 * isn't testable without dragging in the whole signal/UI stack.
 * This Tier 2 test exercises the *wire parse* — the part that
 * actually depends on the protocol catalogue.
 */

#include "config.h"
#include <string.h>
#include <stdarg.h>
#include <glib.h>
#include "protocol.h"
#include "hotline.h"
#include "proto_helpers.h"
#include "wire_fixture.h"

/* ---------- send-side: pack via hlpack ---------- */

/* Pack straight into htlc->in so the dh_start walker can read it —
 * hlpack now returns a fresh buffer (there's no htlc->out send buffer). */
static void
hlpack_v (struct htlc_conn *htlc, guint32 type, guint32 flag, int hc, ...)
{
    va_list ap;
    va_start (ap, hc);
    gsize len = 0;
    guint8 *buf = hlpack (htlc, type, flag, hc, ap, &len);
    va_end (ap);

    g_free (hx_test_in (htlc)->buf);
    hx_test_in (htlc)->buf = buf;
    hx_test_in (htlc)->pos = len;
    hx_test_in (htlc)->len = len;
}

static void
htlc_init (struct htlc_conn *htlc, guint32 starting_trans)
{
    memset (htlc, 0, sizeof (*htlc));
    htlc->trans = starting_trans;
}

static void
htlc_free (struct htlc_conn *htlc)
{
    g_free (hx_test_in (htlc)->buf);
    hx_test_in (htlc)->buf = NULL;
}

/* Walk the packed message in hx_test_in(htlc)->buf and assert it has the
 * expected opcode in the header. */
static void
assert_packed_opcode (struct htlc_conn *htlc, guint32 expected)
{
    g_assert_cmpuint (hx_test_in (htlc)->pos, >=, SIZEOF_HL_HDR);
    const struct hl_hdr *h = (const struct hl_hdr *)hx_test_in (htlc)->buf;
    g_assert_cmphex (g_ntohl (h->type), ==, expected);
}

/* ---------- GETFOLDER request ---------- */

/* GETFOLDER without DIR — single FILE_NAME chunk. Matches the
 * files.c hx_get_folder branch when rdir is empty / root. */
static void
test_getfolder_request_name_only (void)
{
    struct htlc_conn htlc;
    htlc_init (&htlc, 42);

    const char *name = "Oni Tracks";
    hlpack_v (&htlc, HTLC_HDR_FILE_GETFOLDER, 0, /*hc=*/1,
              (int)HTLC_DATA_FILE_NAME, (int)strlen (name), (guint8 *)name);

    assert_packed_opcode (&htlc, HTLC_HDR_FILE_GETFOLDER);

    int found = 0;
    dh_start (hx_test_in (&htlc)->buf, hx_test_in (&htlc)->pos)
    {
        found++;
        g_assert_cmphex (_type, ==, HTLC_DATA_FILE_NAME);
        g_assert_cmpuint (_len, ==, strlen (name));
        g_assert_cmpmem (dh->data, _len, name, strlen (name));
    }
    dh_end ();
    g_assert_cmpint (found, ==, 1);

    htlc_free (&htlc);
}

/* GETFOLDER with a DIR chunk for a non-root parent. Matches the
 * files.c hx_get_folder branch when rdir is non-empty. The DIR
 * chunk's body is opaque here (path_to_hldir's encoding is tested
 * separately in test_path_hldir); we only assert it survived the
 * round trip with the right type code and length. */
static void
test_getfolder_request_with_dir (void)
{
    struct htlc_conn htlc;
    htlc_init (&htlc, 99);

    const char *name = "leaf";
    const guint8 dir_chunk[] = { 0x00, 0x01, /* dc = 1 */
                                 0x00, 0x00, /* enc */
                                 0x05,       /* namelen */
                                 'f',  'i',  'l', 'e', 's' };

    hlpack_v (&htlc, HTLC_HDR_FILE_GETFOLDER, 0, /*hc=*/2,
              (int)HTLC_DATA_FILE_NAME, (int)strlen (name), (guint8 *)name,
              (int)HTLC_DATA_DIR, (int)sizeof (dir_chunk), dir_chunk);

    assert_packed_opcode (&htlc, HTLC_HDR_FILE_GETFOLDER);

    int saw_name = 0, saw_dir = 0;
    dh_start (hx_test_in (&htlc)->buf, hx_test_in (&htlc)->pos)
    {
        switch (_type) {
        case HTLC_DATA_FILE_NAME:
            saw_name++;
            g_assert_cmpmem (dh->data, _len, name, strlen (name));
            break;
        case HTLC_DATA_DIR:
            saw_dir++;
            g_assert_cmpuint (_len, ==, sizeof (dir_chunk));
            g_assert_cmpmem (dh->data, _len, dir_chunk, sizeof (dir_chunk));
            break;
        default:
            g_error ("unexpected chunk type 0x%04x", _type);
        }
    }
    dh_end ();
    g_assert_cmpint (saw_name, ==, 1);
    g_assert_cmpint (saw_dir, ==, 1);

    htlc_free (&htlc);
}

/* ---------- PUTFOLDER request ---------- */

/* PUTFOLDER without DIR — three chunks (FILE_NAME + HTXF_SIZE +
 * FILE_NFILES). The two numeric chunks are u32-big-endian; verify
 * the bytes laid down land in dh_getint as expected. */
static void
test_putfolder_request_name_size_nfiles (void)
{
    struct htlc_conn htlc;
    htlc_init (&htlc, 7);

    const char *name = "uploads";
    /* hx_put_folder htonl's the numbers before passing — replicate
     * that here so the packed bytes match the production caller. */
    guint32 size_n = g_htonl (12345);
    guint32 nfiles_n = g_htonl (3);

    hlpack_v (&htlc, HTLC_HDR_FILE_PUTFOLDER, 0, /*hc=*/3,
              (int)HTLC_DATA_FILE_NAME, (int)strlen (name), (guint8 *)name,
              (int)HTLC_DATA_HTXF_SIZE, 4, &size_n, (int)HTLC_DATA_FILE_NFILES,
              4, &nfiles_n);

    assert_packed_opcode (&htlc, HTLC_HDR_FILE_PUTFOLDER);

    int saw_name = 0, saw_size = 0, saw_nfiles = 0;
    guint32 got_size = 0, got_nfiles = 0;
    dh_start (hx_test_in (&htlc)->buf, hx_test_in (&htlc)->pos)
    {
        switch (_type) {
        case HTLC_DATA_FILE_NAME:
            saw_name++;
            g_assert_cmpmem (dh->data, _len, name, strlen (name));
            break;
        case HTLC_DATA_HTXF_SIZE:
            saw_size++;
            g_assert_cmpuint (_len, ==, 4);
            dh_getint (got_size);
            break;
        case HTLC_DATA_FILE_NFILES:
            saw_nfiles++;
            g_assert_cmpuint (_len, ==, 4);
            dh_getint (got_nfiles);
            break;
        default:
            g_error ("unexpected chunk type 0x%04x", _type);
        }
    }
    dh_end ();

    g_assert_cmpint (saw_name, ==, 1);
    g_assert_cmpint (saw_size, ==, 1);
    g_assert_cmpint (saw_nfiles, ==, 1);
    g_assert_cmpuint (got_size, ==, 12345);
    g_assert_cmpuint (got_nfiles, ==, 3);

    htlc_free (&htlc);
}

/* PUTFOLDER with DIR + FILE_NAME + HTXF_SIZE + FILE_NFILES (4
 * chunks — the long form of hx_put_folder). */
static void
test_putfolder_request_with_dir (void)
{
    struct htlc_conn htlc;
    htlc_init (&htlc, 7);

    const char *name = "uploads";
    const guint8 dir_chunk[] = { 0x00, 0x01, /* dc = 1 */
                                 0x00, 0x00, /* enc */
                                 0x05,       /* namelen */
                                 'p',  'u',  'b', 'l', 'i' };
    guint32 size_n = g_htonl (1);
    guint32 nfiles_n = g_htonl (1);

    hlpack_v (&htlc, HTLC_HDR_FILE_PUTFOLDER, 0, /*hc=*/4,
              (int)HTLC_DATA_FILE_NAME, (int)strlen (name), (guint8 *)name,
              (int)HTLC_DATA_DIR, (int)sizeof (dir_chunk), dir_chunk,
              (int)HTLC_DATA_HTXF_SIZE, 4, &size_n, (int)HTLC_DATA_FILE_NFILES,
              4, &nfiles_n);

    assert_packed_opcode (&htlc, HTLC_HDR_FILE_PUTFOLDER);

    int chunks = 0;
    int saw_dir = 0;
    dh_start (hx_test_in (&htlc)->buf, hx_test_in (&htlc)->pos)
    {
        chunks++;
        if (_type == HTLC_DATA_DIR) {
            saw_dir = 1;
            g_assert_cmpmem (dh->data, _len, dir_chunk, sizeof (dir_chunk));
        }
    }
    dh_end ();
    g_assert_cmpint (chunks, ==, 4);
    g_assert_cmpint (saw_dir, ==, 1);

    htlc_free (&htlc);
}

/* ---------- receive-side: rcv_task_folder_get reply parser ---------- */

/* Synthetic HTLS TASK reply carrying HTXF_SIZE + HTXF_REF +
 * FILE_NFILES — the chunks rcv_task_folder_get walks. The actual
 * function couples the walk to xfers[] / signal emission, which
 * isn't testable in isolation; this exercises the wire-parse half,
 * the part that depends on the protocol catalogue. */
static void
test_folder_get_reply_parse (void)
{
    struct htlc_conn htlc;
    memset (&htlc, 0, sizeof htlc);

    wire_fixture_init (&htlc, HTLS_HDR_TASK, /*trans=*/123, /*flag=*/0);

    /* The server's order is implementation-defined; rcv handles
     * arbitrary order. Mix it up to prove that. */
    guint32 size_n = g_htonl (8192);
    guint32 ref_n = g_htonl (0xdeadbeef);
    guint32 nfiles_n = g_htonl (5);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_HTXF_SIZE, 4, &size_n);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_FILE_NFILES, 4, &nfiles_n);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_HTXF_REF, 4, &ref_n);

    guint32 ref = 0, size = 0, queue = 0, nfiles = 0;
    dh_start (hx_test_in (&htlc)->buf, hx_test_in (&htlc)->pos)
    {
        switch (_type) {
        case HTLS_DATA_HTXF_SIZE:
            dh_getint (size);
            break;
        case HTLS_DATA_HTXF_REF:
            dh_getint (ref);
            break;
        case HTLS_DATA_QUEUE:
            dh_getint (queue);
            break;
        case HTLS_DATA_FILE_NFILES:
            dh_getint (nfiles);
            break;
        }
    }
    dh_end ();

    g_assert_cmphex (ref, ==, 0xdeadbeefu);
    g_assert_cmpuint (size, ==, 8192);
    g_assert_cmpuint (nfiles, ==, 5);
    g_assert_cmpuint (queue, ==, 0); /* no QUEUE chunk present */

    wire_fixture_free (&htlc);
}

/* HTLS TASK with a QUEUE chunk — the server is parking the
 * request behind other transfers. rcv_task_folder_get records
 * htxf->queue and waits for the unsolicited HTLS_HDR_XFER_QUEUE
 * "you're up" message; until then no HTXF subchannel opens. */
static void
test_folder_get_reply_with_queue (void)
{
    struct htlc_conn htlc;
    memset (&htlc, 0, sizeof htlc);

    wire_fixture_init (&htlc, HTLS_HDR_TASK, /*trans=*/200, /*flag=*/0);

    guint32 ref_n = g_htonl (1);
    guint32 size_n = g_htonl (1024);
    guint32 queue_n = g_htonl (3); /* third in line */
    wire_fixture_add_chunk (&htlc, HTLS_DATA_HTXF_REF, 4, &ref_n);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_HTXF_SIZE, 4, &size_n);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_QUEUE, 4, &queue_n);

    guint32 ref = 0, size = 0, queue = 0;
    dh_start (hx_test_in (&htlc)->buf, hx_test_in (&htlc)->pos)
    {
        switch (_type) {
        case HTLS_DATA_HTXF_REF:
            dh_getint (ref);
            break;
        case HTLS_DATA_HTXF_SIZE:
            dh_getint (size);
            break;
        case HTLS_DATA_QUEUE:
            dh_getint (queue);
            break;
        }
    }
    dh_end ();

    g_assert_cmpuint (ref, ==, 1);
    g_assert_cmpuint (size, ==, 1024);
    g_assert_cmpuint (queue, ==, 3);

    wire_fixture_free (&htlc);
}

/* rcv_task_folder_put has a smaller chunk set than folder_get —
 * just HTXF_REF and (optionally) QUEUE. NFILES isn't sent on the
 * put reply (the client already knows the count, it told the
 * server). Pin that down — if a future server starts echoing back
 * NFILES we want to know, but for now the parser should be tolerant. */
static void
test_folder_put_reply_parse (void)
{
    struct htlc_conn htlc;
    memset (&htlc, 0, sizeof htlc);

    wire_fixture_init (&htlc, HTLS_HDR_TASK, /*trans=*/77, /*flag=*/0);

    guint32 ref_n = g_htonl (0xabad1dea);
    wire_fixture_add_chunk (&htlc, HTLS_DATA_HTXF_REF, 4, &ref_n);

    guint32 ref = 0, queue = 0;
    int chunks = 0;
    dh_start (hx_test_in (&htlc)->buf, hx_test_in (&htlc)->pos)
    {
        chunks++;
        switch (_type) {
        case HTLS_DATA_HTXF_REF:
            dh_getint (ref);
            break;
        case HTLS_DATA_QUEUE:
            dh_getint (queue);
            break;
        }
    }
    dh_end ();

    g_assert_cmpint (chunks, ==, 1);
    g_assert_cmphex (ref, ==, 0xabad1deau);
    g_assert_cmpuint (queue, ==, 0);

    wire_fixture_free (&htlc);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/folder_xfer/getfolder_request_name_only",
                     test_getfolder_request_name_only);
    g_test_add_func ("/folder_xfer/getfolder_request_with_dir",
                     test_getfolder_request_with_dir);
    g_test_add_func ("/folder_xfer/putfolder_request_name_size_nfiles",
                     test_putfolder_request_name_size_nfiles);
    g_test_add_func ("/folder_xfer/putfolder_request_with_dir",
                     test_putfolder_request_with_dir);
    g_test_add_func ("/folder_xfer/folder_get_reply_parse",
                     test_folder_get_reply_parse);
    g_test_add_func ("/folder_xfer/folder_get_reply_with_queue",
                     test_folder_get_reply_with_queue);
    g_test_add_func ("/folder_xfer/folder_put_reply_parse",
                     test_folder_put_reply_parse);

    return g_test_run ();
}
