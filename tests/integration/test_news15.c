/*
 * tests/integration/test_news15.c — exercise the 1.5+ threaded
 * news opcodes against mhxd.
 *
 * mhxd's tnews.c implements:
 *   HTLC_HDR_NEWSDIRLIST  → tnews_send_dirlist (folder/category
 *                           listing under a path)
 *   HTLC_HDR_NEWSCATLIST  → tnews_send_catlist (article list in
 *                           a category)
 *   HTLC_HDR_GETTHREAD    → fetch one article body
 *
 * The shipped run/hxd/newsdir/ has at least 'cat_irasshaimase' in
 * its root, so listing the root will find that entry.
 *
 * For the dirlist test:
 *   1. Login.
 *   2. Send HTLC_HDR_NEWSDIRLIST with no chunks (mhxd's
 *      rcv_news_listdir interprets `htlc->in.pos == SIZEOF_HL_HDR`
 *      as 'list the configured newsdir root').
 *   3. Drain to the TASK reply matching our trans.
 *   4. Verify at least one HTLS_DATA_NEWS_DIRLIST_EXTENDED chunk
 *      appears.
 *
 * mhxd gates the whole flow on `hxd_cfg.operation.tnews`, which is
 * `yes` in the shipped config. If a server has it disabled, mhxd
 * sends a task-error and we surface that in the test log without
 * failing.
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

/* mhxd's HTLS_DATA_NEWS_DIRLIST_EXTENDED — the folder/category
 * metadata chunk type that comes back in the dirlist reply. Not in
 * GtkHx's hotline.h yet (the existing news15 parser uses different
 * chunk types). The constant is borrowed verbatim from
 * mhxd/src/common/hotline.h. */
#define HTLS_DATA_NEWS_DIRLIST_EXTENDED ((guint16)0x0143)

static void
test_news15_root_dirlist (void)
{
    struct htlc_conn htlc;
    int fd = integration_open_login_or_skip (&htlc, "News15 Tier-3", 412);
    if (fd < 0) {
        return;
    }

    guint32 our_trans = htlc.trans;
    g_assert_true (integration_send_message (fd, &htlc, HTLC_HDR_NEWSDIRLIST,
                                             /*flag=*/0, /*hc=*/0));

    g_assert_true (integration_drain_until_task_trans (
        fd, &htlc, our_trans, 64));

    if (hdr_flag (&htlc) & 1) {
        char err[256];
        gsize err_len = 0;
        if (task_error_extract (htlc.in.buf, htlc.in.pos, err, sizeof (err), &err_len)) {
            g_test_message ("news15 dirlist refused: \"%s\" "
                            "(server may have tnews disabled)",
                            err);
        } else {
            g_test_message ("news15 dirlist refused (no error chunk)");
        }
        integration_release_htlc (&htlc);
        integration_close (fd);
        return;
    }

    /* Walk the chunks; we expect at least one
	 * HTLS_DATA_NEWS_DIRLIST_EXTENDED. The shipped run/hxd/newsdir
	 * has cat_irasshaimase pre-seeded as a category. */
    int dirlist_chunks = 0;
    dh_start (htlc.in.buf, htlc.in.pos)
    {
        if (_type == HTLS_DATA_NEWS_DIRLIST_EXTENDED) {
            dirlist_chunks++;
        }
    }
    dh_end ();
    g_test_message ("news15 dirlist returned %d entries", dirlist_chunks);
    g_assert_cmpint (dirlist_chunks, >, 0);

    integration_release_htlc (&htlc);
    integration_close (fd);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/integration/news15/root_dirlist",
                     test_news15_root_dirlist);

    return g_test_run ();
}
