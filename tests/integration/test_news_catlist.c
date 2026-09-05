/*
 * tests/integration/test_news_catlist.c — list the articles inside a
 * threaded-news category (HTLC_HDR_NEWSCATLIST), on every rig server
 * that ships a seeded news tree.
 *
 * test_news15.c covers the level above this — listing the folders and
 * categories under a path, and creating them. This goes one level
 * deeper, into a category's article list.
 *
 * The reply carries a single HTLC_DATA_CATLIST chunk holding the whole
 * article list, not one chunk per article:
 *
 *   u32 opaque
 *   u32 post_count
 *   u16 opaque
 *   per post: 22-byte header, pstring subject, pstring sender, parts
 *
 * so counting chunks proves only that the server answered — an empty
 * category sends that same one chunk with a count of zero. What this
 * test reads is the count itself, which is the number the client's
 * parser (hotline_proto::parse::parse_catlist) drives its post loop
 * from.
 *
 * Fanned across HX_TEST_CAP_NEWS_15_FIXTURES rather than
 * HX_TEST_CAP_NEWS_15: a server can implement the opcode perfectly and
 * still have an empty tree, and "the seeded category has articles" is a
 * claim about the container, not about the protocol. mhxd ships
 * cat_irasshaimase under run/hxd/newsdir/ and the Mobius container
 * seeds a category of the same name, so the same request reads the same
 * shape on both.
 */

#include "config.h"
#include <string.h>
#include <unistd.h>
#include <glib.h>
#include "compat.h"
#include "hotline.h"
#include "protocol.h"
#include "proto_helpers.h"
#include "integration_harness.h"
#include "server_matrix.h"

/*
 * The category both seeded rigs carry. mhxd stores categories as
 * directories named cat_<name>, so the on-disk cat_irasshaimase is the
 * category "irasshaimase" on the wire; the Mobius fixture names it
 * directly in ThreadedNews.yaml.
 */
#define SEEDED_CATEGORY "irasshaimase"

/* ---- NEWSPATH packing ---------------------------------------------- */

/*
 * A single-component HTLC_DATA_NEWSPATH blob — u16 count, then
 * {u16 zero, u8 namelen, name} — the same bytes
 * src/path_hldir.c::path_to_hldir emits for "/<name>".
 *
 * Returns g_malloc'd bytes; caller frees.
 */
static guint8 *
build_news_path_one (const char *name, guint16 *outlen)
{
    guint8 nlen = (guint8)strlen (name);
    guint16 total = (guint16)(5 + nlen);
    guint8 *buf = g_malloc (total);
    guint16 count = g_htons (1);

    memcpy (buf, &count, 2);
    buf[2] = 0;
    buf[3] = 0;
    buf[4] = nlen;
    memcpy (&buf[5], name, nlen);

    *outlen = total;
    return buf;
}

/* ---- CATLIST body ---------------------------------------------------- */

/*
 * Read post_count out of the first HTLC_DATA_CATLIST chunk. Returns
 * FALSE when there is no such chunk or its body is shorter than the
 * 10-byte threadlist header — the same two conditions on which
 * parse_catlist returns None, which is what makes a truncated header
 * worth failing on rather than reading as "zero articles".
 */
static gboolean
catlist_post_count (const struct htlc_conn *htlc, guint32 *out_count)
{
    gboolean found = FALSE;

    dh_start (hx_test_in (htlc)->buf, hx_test_in (htlc)->pos)
    {
        if (_type != HTLC_DATA_CATLIST || found) {
            continue;
        }
        if (_len < 10) {
            break;
        }
        /* u32 opaque, u32 post_count, u16 opaque. */
        HN32 (out_count, &dh->data[4]);
        found = TRUE;
    }
    dh_end ();

    return found;
}

/* Send a NEWSCATLIST for `category` and drain to its TASK reply.
 * Returns FALSE only when no reply arrived at all. */
static gboolean
request_catlist (int fd, struct htlc_conn *htlc, const char *category)
{
    guint16 pathlen = 0;
    guint8 *path = build_news_path_one (category, &pathlen);
    guint32 trans = htlc->trans;

    gboolean sent = integration_send_message (
        fd, htlc, HTLC_HDR_NEWSCATLIST, /*flag=*/0, /*hc=*/1,
        (int)HTLC_DATA_NEWSPATH, (int)pathlen, path);
    g_free (path);

    g_assert_true (sent);
    return integration_drain_until_task_trans (fd, htlc, trans, 64);
}

/* ---- Subtests -------------------------------------------------------- */

static void
test_seeded_category_has_articles (gconstpointer data)
{
    const hx_test_server *srv = data;
    struct htlc_conn htlc;

    int fd = integration_open_login_to_caps_or_skip (
        srv, &htlc, "CatList Tier-3", 412, /*caps=*/0);
    if (fd < 0) {
        return;
    }

    if (!request_catlist (fd, &htlc, SEEDED_CATEGORY)) {
        g_test_fail_printf ("%s: no reply to NEWSCATLIST for \"%s\"", srv->name,
                            SEEDED_CATEGORY);
        goto out;
    }

    if (hdr_flag (&htlc) & 1) {
        char err[256];
        gsize err_len = 0;
        if (!task_error_extract (hx_test_in (&htlc)->buf,
                                 hx_test_in (&htlc)->pos, err, sizeof (err),
                                 &err_len)) {
            err[0] = 0;
        }
        g_test_fail_printf ("%s refused NEWSCATLIST for the seeded "
                            "category \"%s\": \"%s\" — the matrix entry "
                            "claims HX_TEST_CAP_NEWS_15_FIXTURES, so the "
                            "container's news tree needs re-seeding.",
                            srv->name, SEEDED_CATEGORY, err);
        goto out;
    }

    guint32 count = 0;
    if (!catlist_post_count (&htlc, &count)) {
        g_test_fail_printf ("%s: NEWSCATLIST reply for \"%s\" carried no "
                            "well-formed HTLC_DATA_CATLIST chunk",
                            srv->name, SEEDED_CATEGORY);
        goto out;
    }

    g_test_message ("%s: \"%s\" lists %u article(s)", srv->name,
                    SEEDED_CATEGORY, count);
    if (count == 0) {
        g_test_fail_printf ("%s: seeded category \"%s\" is empty — the "
                            "container's news tree has drifted (creation "
                            "tests mutate it) and needs a rebuild.",
                            srv->name, SEEDED_CATEGORY);
    }

out:
    integration_release_htlc (&htlc);
    integration_close (fd);
}

/*
 * A category that isn't there has to come back as a reply — an error or
 * an empty list, the server's choice — and not as silence. Threaded
 * news has already produced one server that answers a malformed request
 * with nothing at all (see tests/mobius/README.md), and a request that
 * never settles leaves a real client's task pending forever, which is
 * far worse than a refusal.
 */
static void
test_missing_category_still_replies (gconstpointer data)
{
    const hx_test_server *srv = data;
    struct htlc_conn htlc;

    int fd = integration_open_login_to_caps_or_skip (
        srv, &htlc, "CatList Missing", 412, /*caps=*/0);
    if (fd < 0) {
        return;
    }

    gchar *absent = g_strdup_printf ("t3-absent-%08x", g_random_int ());
    if (!request_catlist (fd, &htlc, absent)) {
        g_test_fail_printf ("%s: NEWSCATLIST for a category that doesn't "
                            "exist got no reply at all",
                            srv->name);
    } else if (hdr_flag (&htlc) & 1) {
        g_test_message ("%s: absent category refused, as expected", srv->name);
    } else {
        guint32 count = 0;
        if (catlist_post_count (&htlc, &count) && count != 0) {
            g_test_fail_printf ("%s: NEWSCATLIST for the non-existent "
                                "category \"%s\" reported %u articles",
                                srv->name, absent, count);
        }
    }

    g_free (absent);
    integration_release_htlc (&htlc);
    integration_close (fd);
}

static void
test_matrix_is_empty (void)
{
    g_test_fail_printf (
        "no matrix entry advertises HX_TEST_CAP_NEWS_15_FIXTURES (after "
        "the GTKHX_TEST_SERVERS filter), so the article-listing subtests "
        "had nothing to run against.");
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    GPtrArray *servers = hx_test_servers_with (HX_TEST_CAP_NEWS_15_FIXTURES);
    if (!servers || servers->len == 0) {
        g_test_add_func ("/integration/news_catlist/matrix_populated",
                         test_matrix_is_empty);
        g_clear_pointer (&servers, g_ptr_array_unref);
        return g_test_run ();
    }

    for (guint i = 0; i < servers->len; i++) {
        const hx_test_server *srv = g_ptr_array_index (servers, i);
        gchar *p;

        p = g_strdup_printf ("/integration/news_catlist/%s/seeded_category",
                             srv->name);
        g_test_add_data_func (p, srv, test_seeded_category_has_articles);
        g_free (p);

        p = g_strdup_printf ("/integration/news_catlist/%s/missing_category",
                             srv->name);
        g_test_add_data_func (p, srv, test_missing_category_still_replies);
        g_free (p);
    }
    g_ptr_array_unref (servers);

    return g_test_run ();
}
