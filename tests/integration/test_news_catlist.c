/*
 * tests/integration/test_news_catlist.c — list articles in a
 * seeded threaded-news category via HTLC_HDR_NEWSCATLIST.
 *
 * The news15 root-listing test (test_news15.c) covers the level
 * above this — listing categories that exist directly under the
 * newsdir root. This test goes one level deeper: ask mhxd for the
 * articles inside the seeded `irasshaimase` category. The shipped
 * upstream run/hxd/newsdir/cat_irasshaimase/ has at least one
 * article (numbered file `1`), so the catlist reply should carry
 * at least one entry.
 *
 * Wire format for the request, from mhxd/src/hxd/tnews.c
 * (cat_to_path) — HTLC_DATA_NEWS_DIR is a packed path blob:
 *
 *   bytes 0-1   : component count       (big-endian u_int16_t)
 *   bytes 2-3   : reserved / unknown    (zeroes)
 *   per non-last component:
 *     1 byte    : name length
 *     N bytes   : name
 *     2 bytes   : trailing (zeroes)
 *   last component (no trailing):
 *     1 byte    : name length
 *     N bytes   : name
 *
 * cat_to_path treats a single-component path specially: it
 * prepends "cat_" before joining, so a payload of "irasshaimase"
 * with count=1 becomes the on-disk path
 * <newsdir>/cat_irasshaimase. That's exactly the seeded directory.
 *
 * Tolerant of:
 *   - tnews disabled on the server (mhxd replies with task-error;
 *     we log and exit clean)
 *   - empty / missing category (still TASK flag=0 with zero
 *     listing chunks; we log the count and don't insist on > 0
 *     since a future seed change shouldn't fail us)
 *
 * What we DO assert is the strict positive case: when tnews is on
 * AND the seeded category exists, at least one listing chunk
 * comes back. mhxd's reply uses HTLS_DATA_NEWS_CAT_LIST chunks
 * (constant verbatim from mhxd's hotline.h, like the *_EXTENDED
 * one in test_news15.c).
 */

#include "config.h"
#include <string.h>
#include <netinet/in.h>
#include <unistd.h>
#include <glib.h>
#include "compat.h"
#include "hotline.h"
#include "protocol.h"
#include "proto_helpers.h"
#include "integration_harness.h"

/* mhxd's chunk types. Not in GtkHx's hotline.h yet — values
 * verbatim from mhxd/src/common/hotline.h (HTLC_DATA_NEWS_DIR
 * = 0x0145, HTLS_DATA_NEWS_CATLIST = 0x0141). 0x0142 is
 * HTLC_DATA_NEWS_CATNAME, NOT the catlist response chunk —
 * easy to misremember. */
#define HTLC_DATA_NEWS_DIR ((guint16)0x0145)
#define HTLS_DATA_NEWS_CATLIST ((guint16)0x0141)

/* Build a single-component HTLC_DATA_NEWS_DIR blob for `name`.
 * Returns g_malloc'd bytes; caller frees. *outlen receives the
 * total size. */
static guint8 *
build_news_dir_one (const char *name, guint16 *outlen)
{
    guint8 nlen = (guint8)strlen (name);
    /* 2 (count) + 2 (reserved) + 1 (nlen) + name */
    guint16 total = 5 + nlen;
    guint8 *buf = g_malloc (total);
    guint16 count = htons (1);

    memcpy (buf, &count, 2);
    buf[2] = 0;
    buf[3] = 0;
    buf[4] = nlen;
    memcpy (&buf[5], name, nlen);

    *outlen = total;
    return buf;
}

static void
test_news_catlist_seeded_irasshaimase (void)
{
    struct htlc_conn htlc;
    int fd = integration_open_login_or_skip (&htlc, "CatList Tier-3", 412);
    if (fd < 0) {
        return;
    }

    guint16 dirlen = 0;
    guint8 *dir = build_news_dir_one ("irasshaimase", &dirlen);
    guint32 our_trans = htlc.trans;

    g_assert_true (integration_send_message (
        fd, &htlc, HTLC_HDR_NEWSCATLIST, /*flag=*/0, /*hc=*/1,
        (int)HTLC_DATA_NEWS_DIR, (int)dirlen, dir));
    g_free (dir);

    g_assert_true (integration_drain_until_task_trans (
        fd, &htlc, our_trans, 64));

    if (hdr_flag (&htlc) & 1) {
        char err[256];
        gsize err_len = 0;
        if (task_error_extract (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, err, sizeof (err), &err_len)) {
            g_test_message ("news catlist refused: \"%s\" "
                            "(server may have tnews disabled)",
                            err);
        }
        integration_release_htlc (&htlc);
        integration_close (fd);
        return;
    }

    int catlist_chunks = 0;
    dh_start (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos)
    {
        if (_type == HTLS_DATA_NEWS_CATLIST) {
            catlist_chunks++;
        }
    }
    dh_end ();
    g_test_message ("news catlist returned %d entries for "
                    "cat_irasshaimase",
                    catlist_chunks);
    g_assert_cmpint (catlist_chunks, >, 0);

    integration_release_htlc (&htlc);
    integration_close (fd);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/integration/news_catlist/seeded_irasshaimase",
                     test_news_catlist_seeded_irasshaimase);
    return g_test_run ();
}
