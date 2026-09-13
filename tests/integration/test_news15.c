/*
 * tests/integration/test_news15.c — the 1.5+ threaded-news opcodes,
 * run against every server in the matrix that advertises them.
 *
 * Covered here:
 *   HTLC_HDR_NEWSDIRLIST   — folder/category listing under a path
 *   HTLC_HDR_MAKENEWSDIR   — create a folder (NEWSPATH parent + FILE_NAME)
 *   HTLC_HDR_MAKECATEGORY  — create a category (NEWSPATH parent + CATEGORY)
 *   HTLC_HDR_DELNEWSDIRCAT — remove either
 *
 * Article listing and article bodies live in test_news_catlist.c.
 *
 * Why this fans across the matrix
 * -------------------------------
 * These four opcodes are where the three server families disagree most,
 * and every disagreement so far has been invisible against mhxd alone:
 *
 *   - MAKENEWSDIR takes the *parent* path plus a separate FILE_NAME. Send
 *     the new name as the path's last component instead and mhxd answers
 *     ENOENT, while Mobius walks into a nil map, panics its connection
 *     handler, and sends nothing back at all — the request just never
 *     completes. The create round-trip below asserts a reply always
 *     arrives, which is the assertion that pins that shape.
 *   - Janus's guest account has no news write bits, so its half of the
 *     round-trip is the refusal path rather than the success path. That
 *     is asserted too, not skipped: a server that silently swallows a
 *     denied request is as broken as one that swallows an allowed one.
 *
 * Which half of the round-trip a server runs is decided by the access
 * bitmap it sent us at login, not by a per-server flag here — so
 * re-granting a bit in a container's config changes what the test
 * demands without touching this file.
 *
 * Server state
 * ------------
 * Creation mutates the server's news tree, so every name this test
 * makes carries a per-run random suffix and is deleted on the way out.
 * A run that dies mid-way leaves its folder behind; that is what the
 * container rebuild is for, and no assertion here depends on the tree
 * being otherwise empty.
 */

#include "config.h"
#include <string.h>
#include <unistd.h>
#include <glib.h>
#include "compat.h"
#include "hotline.h"
#include "protocol.h"
#include "proto_helpers.h"
#include "hl_access.h"
#include "integration_harness.h"
#include "server_matrix.h"

/* ---- NEWSPATH packing ---------------------------------------------- */

/*
 * Pack a component list into the wire HTLC_DATA_NEWSPATH blob, byte for
 * byte what src/path_hldir.c::path_to_hldir emits for the same path:
 *
 *   u16 count
 *   per component: u16 zero, u8 namelen, name bytes
 *
 * The root is a bare count of zero — two bytes — which is what
 * path_to_hldir returns for "/" (it skips empty components and finds no
 * trailing one). Servers read that as "the newsdir root".
 *
 * Returns g_malloc'd bytes; caller frees. *outlen gets the total size.
 */
static guint8 *
build_news_path (const char *const *parts, guint nparts, guint16 *outlen)
{
    guint16 total = 2;
    for (guint i = 0; i < nparts; i++) {
        total = (guint16)(total + 3 + strlen (parts[i]));
    }

    guint8 *buf = g_malloc (total);
    guint16 count = g_htons ((guint16)nparts);
    memcpy (buf, &count, 2);

    guint16 pos = 2;
    for (guint i = 0; i < nparts; i++) {
        guint8 nlen = (guint8)strlen (parts[i]);
        buf[pos] = 0;
        buf[pos + 1] = 0;
        buf[pos + 2] = nlen;
        memcpy (&buf[pos + 3], parts[i], nlen);
        pos = (guint16)(pos + 3 + nlen);
    }

    *outlen = total;
    return buf;
}

/* ---- Directory-listing walker --------------------------------------- */

typedef enum {
    NEWS_ENTRY_NONE = 0,
    NEWS_ENTRY_FOLDER,
    NEWS_ENTRY_CATEGORY,
} news_entry_kind;

/*
 * Pull one entry's name and kind out of an HTLC_DATA_CATEGORYITEM chunk.
 *
 *   u16 ntype   2 = folder (bundle), 3 = category
 *   u16 count
 *   category only: u8 guid[16], u8 addsn[4], u8 deletesn[4]
 *   u8  namelen
 *   u8  name[namelen]
 *
 * The 24 bytes of GUID / serial numbers are present for a category and
 * absent for a folder — a fixed-offset reader gets folders wrong, which
 * is the shape bug worth having a test notice. Mirrors
 * hotline_proto::parse::parse_news_categoryitem; returns NEWS_ENTRY_NONE
 * on an unknown ntype or any truncation, exactly as that parser returns
 * None.
 */
static news_entry_kind
categoryitem_name (const guint8 *body, guint16 len, char *name, gsize name_cap)
{
    if (len < 5) {
        return NEWS_ENTRY_NONE;
    }
    guint16 ntype = (guint16)((body[0] << 8) | body[1]);

    gsize off;
    news_entry_kind kind;
    if (ntype == 2) {
        kind = NEWS_ENTRY_FOLDER;
        off = 4; /* ntype + count */
    } else if (ntype == 3) {
        kind = NEWS_ENTRY_CATEGORY;
        off = 28; /* ntype + count + guid + addsn + deletesn */
    } else {
        return NEWS_ENTRY_NONE;
    }

    if (len < off + 1) {
        return NEWS_ENTRY_NONE;
    }
    gsize nlen = body[off];
    off++;
    if (len < off + nlen || nlen + 1 > name_cap) {
        return NEWS_ENTRY_NONE;
    }
    memcpy (name, &body[off], nlen);
    name[nlen] = 0;
    return kind;
}

/*
 * The older HTLC_DATA_NEWSFOLDERITEM shape: u8 ntype (1 = folder) then
 * the name to the end of the chunk, no length prefix.
 */
static news_entry_kind
folderitem_name (const guint8 *body, guint16 len, char *name, gsize name_cap)
{
    if (len < 1) {
        return NEWS_ENTRY_NONE;
    }
    gsize nlen = len - 1;
    if (nlen + 1 > name_cap) {
        return NEWS_ENTRY_NONE;
    }
    memcpy (name, &body[1], nlen);
    name[nlen] = 0;
    return body[0] == 1 ? NEWS_ENTRY_FOLDER : NEWS_ENTRY_CATEGORY;
}

/*
 * Walk the dirlist reply sitting in htlc's receive buffer. Returns the
 * number of entries that parsed; if `want` is non-NULL, *found_kind is
 * set to the kind of the entry with that name (NEWS_ENTRY_NONE if it
 * isn't there).
 *
 * Both chunk types can carry either kind, so both are walked — the same
 * normalisation parse_dirlist does.
 */
static int
scan_dirlist (const struct htlc_conn *htlc, const char *want,
              news_entry_kind *found_kind)
{
    int entries = 0;
    if (found_kind) {
        *found_kind = NEWS_ENTRY_NONE;
    }

    dh_start (hx_test_in (htlc)->buf, hx_test_in (htlc)->pos)
    {
        char name[256];
        news_entry_kind kind = NEWS_ENTRY_NONE;

        if (_type == HTLC_DATA_CATEGORYITEM) {
            kind = categoryitem_name (dh->data, _len, name, sizeof (name));
        } else if (_type == HTLC_DATA_NEWSFOLDERITEM) {
            kind = folderitem_name (dh->data, _len, name, sizeof (name));
        } else {
            continue;
        }

        if (kind == NEWS_ENTRY_NONE) {
            continue;
        }
        entries++;
        if (want && found_kind && !strcmp (name, want)) {
            *found_kind = kind;
        }
    }
    dh_end ();

    return entries;
}

/* ---- Request helpers ------------------------------------------------ */

/*
 * Send one news request whose body is a NEWSPATH plus at most one named
 * payload chunk, then drain to its TASK reply.
 *
 * Returns TRUE when a reply for our transaction arrived, whatever it
 * says — the caller decides whether an error flag is the right answer.
 * FALSE means the server never replied, which is a failure on every
 * path: a request that completes neither way leaves a real client
 * waiting on a task that never settles.
 */
static gboolean
send_news_request (int fd, struct htlc_conn *htlc, guint32 opcode,
                   const char *const *parts, guint nparts, guint16 payload_tag,
                   const char *payload)
{
    guint16 pathlen = 0;
    guint8 *path = build_news_path (parts, nparts, &pathlen);
    guint32 trans = htlc->trans;
    gboolean sent;

    if (payload) {
        sent = integration_send_message (
            fd, htlc, opcode, /*flag=*/0, /*hc=*/2, (int)HTLC_DATA_NEWSPATH,
            (int)pathlen, path, (int)payload_tag, (int)strlen (payload),
            (guint8 *)payload);
    } else {
        sent = integration_send_message (fd, htlc, opcode, /*flag=*/0,
                                         /*hc=*/1, (int)HTLC_DATA_NEWSPATH,
                                         (int)pathlen, path);
    }
    g_free (path);

    g_assert_true (sent);
    return integration_drain_until_task_trans (fd, htlc, trans, 64);
}

/* The error text from a task-error reply, or "" when the server sent an
 * error flag with no DATA_ERROR chunk. Caller-owned storage. */
static void
reply_error_text (const struct htlc_conn *htlc, char *out, gsize cap)
{
    gsize len = 0;
    if (!task_error_extract (hx_test_in (htlc)->buf, hx_test_in (htlc)->pos,
                             out, cap, &len)) {
        out[0] = 0;
    }
}

/* ---- Per-server subtests -------------------------------------------- */

/*
 * A root listing has to come back, be well-formed, and not claim
 * failure. Deliberately says nothing about how many entries there are:
 * the three rigs seed different trees, and the create round-trip below
 * is what proves entries can appear at all.
 */
static void
test_root_dirlist (gconstpointer data)
{
    const hx_test_server *srv = data;
    struct htlc_conn htlc;

    int fd = integration_open_login_to_caps_or_skip (
        srv, &htlc, "News15 Tier-3", 412, /*caps=*/0);
    if (fd < 0) {
        return;
    }

    const char *const *root = NULL;
    if (!send_news_request (fd, &htlc, HTLC_HDR_NEWSDIRLIST, root, 0, 0,
                            NULL)) {
        g_test_fail_printf ("%s: no reply to a root NEWSDIRLIST", srv->name);
        goto out;
    }

    if (hdr_flag (&htlc) & 1) {
        char err[256];
        reply_error_text (&htlc, err, sizeof (err));
        g_test_fail_printf ("%s refused a root NEWSDIRLIST: \"%s\" — the "
                            "matrix entry claims HX_TEST_CAP_NEWS_15, so "
                            "either the server has threaded news disabled "
                            "or the cap is wrong.",
                            srv->name, err);
        goto out;
    }

    int entries = scan_dirlist (&htlc, NULL, NULL);
    g_test_message ("%s: root listing parsed %d entries", srv->name, entries);

out:
    integration_release_htlc (&htlc);
    integration_close (fd);
}

/*
 * The seeded root listing, on whichever server is the default target.
 * mhxd ships cat_irasshaimase under newsdir/ and the Mobius container
 * seeds a matching category, so the default target always has at least
 * one entry — which is the assertion the matrix-wide test above can't
 * make.
 */
static void
test_seeded_root_listing (void)
{
    struct htlc_conn htlc;
    int fd = integration_open_login_or_skip (&htlc, "News15 Seeded", 412);
    if (fd < 0) {
        return;
    }

    const char *const *root = NULL;
    if (!send_news_request (fd, &htlc, HTLC_HDR_NEWSDIRLIST, root, 0, 0,
                            NULL)) {
        g_test_fail_printf ("no reply to a root NEWSDIRLIST");
        goto out;
    }
    if (hdr_flag (&htlc) & 1) {
        char err[256];
        reply_error_text (&htlc, err, sizeof (err));
        g_test_fail_printf ("root NEWSDIRLIST refused: \"%s\"", err);
        goto out;
    }

    g_assert_cmpint (scan_dirlist (&htlc, NULL, NULL), >, 0);

out:
    integration_release_htlc (&htlc);
    integration_close (fd);
}

/*
 * Create a folder at the root, a category at the root, and a category
 * inside the new folder; check each shows up in the listing with the
 * right kind; delete them again.
 *
 * On a server whose account lacks the write bit, each create is
 * asserted to come back as a refusal instead — same reply-must-arrive
 * requirement, opposite verdict.
 */
static void
test_create_roundtrip (gconstpointer data)
{
    const hx_test_server *srv = data;
    struct htlc_conn htlc;
    char err[256];

    int fd = integration_open_login_to_caps_or_skip (
        srv, &htlc, "News15 Create", 412, /*caps=*/0);
    if (fd < 0) {
        return;
    }

    /* htlc->access holds the on-wire 8 access bytes in memory order;
     * hl_access_has takes a byte pointer, so cast through one. */
    const guint8 *acc = (const guint8 *)&htlc.access;
    gboolean may_make_folder
        = hl_access_has (acc, HL_ACCESS_CREATE_NEWS_BUNDLES);
    gboolean may_make_cat = hl_access_has (acc, HL_ACCESS_CREATE_CATEGORIES);
    gboolean may_drop_folder
        = hl_access_has (acc, HL_ACCESS_DELETE_NEWS_BUNDLES);
    gboolean may_drop_cat = hl_access_has (acc, HL_ACCESS_DELETE_CATEGORIES);

    g_test_message ("%s: guest may create folder=%d category=%d, "
                    "delete folder=%d category=%d",
                    srv->name, may_make_folder, may_make_cat, may_drop_folder,
                    may_drop_cat);

    /* One suffix for the whole run so a leftover tree is obviously one
     * aborted run rather than three. */
    guint32 tag = g_random_int ();
    gchar *folder = g_strdup_printf ("t3-fldr-%08x", tag);
    gchar *category = g_strdup_printf ("t3-cat-%08x", tag);
    gchar *nested = g_strdup_printf ("t3-sub-%08x", tag);
    const char *const in_folder[] = { folder };
    const char *const in_nested[] = { folder, nested };
    const char *const at_root_cat[] = { category };
    const char *const *root = NULL;
    news_entry_kind kind;

    /* ---- folder at the root ---- */
    if (!send_news_request (fd, &htlc, HTLC_HDR_MAKENEWSDIR, root, 0,
                            HTLC_DATA_FILE_NAME, folder)) {
        g_test_fail_printf (
            "%s: no reply to MAKENEWSDIR. The request has to settle one way "
            "or the other — a client that registered a task for it waits "
            "forever otherwise.",
            srv->name);
        goto out;
    }

    if (!may_make_folder) {
        if (!(hdr_flag (&htlc) & 1)) {
            g_test_fail_printf ("%s: guest has no create-folder bit but "
                                "MAKENEWSDIR reported success",
                                srv->name);
            goto out;
        }
        reply_error_text (&htlc, err, sizeof (err));
        g_test_message ("%s: MAKENEWSDIR refused as expected: \"%s\"",
                        srv->name, err);
    } else {
        if (hdr_flag (&htlc) & 1) {
            reply_error_text (&htlc, err, sizeof (err));
            g_test_fail_printf ("%s: MAKENEWSDIR refused: \"%s\"", srv->name,
                                err);
            goto out;
        }
        if (!send_news_request (fd, &htlc, HTLC_HDR_NEWSDIRLIST, root, 0, 0,
                                NULL)) {
            g_test_fail_printf ("%s: no reply listing the root after "
                                "creating a folder",
                                srv->name);
            goto out;
        }
        scan_dirlist (&htlc, folder, &kind);
        if (kind != NEWS_ENTRY_FOLDER) {
            g_test_fail_printf ("%s: created folder \"%s\" is %s in the root "
                                "listing",
                                srv->name, folder,
                                kind == NEWS_ENTRY_NONE ? "absent"
                                                        : "listed as a "
                                                          "category");
            goto out;
        }
    }

    /* ---- category at the root ---- */
    if (!send_news_request (fd, &htlc, HTLC_HDR_MAKECATEGORY, root, 0,
                            HTLC_DATA_CATEGORY, category)) {
        g_test_fail_printf ("%s: no reply to MAKECATEGORY", srv->name);
        goto out;
    }

    if (!may_make_cat) {
        if (!(hdr_flag (&htlc) & 1)) {
            g_test_fail_printf ("%s: guest has no create-category bit but "
                                "MAKECATEGORY reported success",
                                srv->name);
            goto out;
        }
        reply_error_text (&htlc, err, sizeof (err));
        g_test_message ("%s: MAKECATEGORY refused as expected: \"%s\"",
                        srv->name, err);
    } else {
        if (hdr_flag (&htlc) & 1) {
            reply_error_text (&htlc, err, sizeof (err));
            g_test_fail_printf ("%s: MAKECATEGORY refused: \"%s\"", srv->name,
                                err);
            goto out;
        }
        if (!send_news_request (fd, &htlc, HTLC_HDR_NEWSDIRLIST, root, 0, 0,
                                NULL)) {
            g_test_fail_printf ("%s: no reply listing the root after "
                                "creating a category",
                                srv->name);
            goto out;
        }
        scan_dirlist (&htlc, category, &kind);
        if (kind != NEWS_ENTRY_CATEGORY) {
            g_test_fail_printf ("%s: created category \"%s\" is %s in the "
                                "root listing",
                                srv->name, category,
                                kind == NEWS_ENTRY_NONE ? "absent"
                                                        : "listed as a "
                                                          "folder");
            goto out;
        }
    }

    /* ---- category one level down ----
     *
     * The nested case is the one that separates "the server took our
     * path" from "the server ignored our path and created at the root",
     * and it only means anything once the folder above actually exists. */
    if (may_make_folder && may_make_cat) {
        if (!send_news_request (fd, &htlc, HTLC_HDR_MAKECATEGORY, in_folder, 1,
                                HTLC_DATA_CATEGORY, nested)) {
            g_test_fail_printf ("%s: no reply to a nested MAKECATEGORY",
                                srv->name);
            goto out;
        }
        if (hdr_flag (&htlc) & 1) {
            reply_error_text (&htlc, err, sizeof (err));
            g_test_fail_printf ("%s: nested MAKECATEGORY refused: \"%s\"",
                                srv->name, err);
            goto out;
        }
        if (!send_news_request (fd, &htlc, HTLC_HDR_NEWSDIRLIST, in_folder, 1,
                                0, NULL)) {
            g_test_fail_printf ("%s: no reply listing inside the new folder",
                                srv->name);
            goto out;
        }
        scan_dirlist (&htlc, nested, &kind);
        if (kind != NEWS_ENTRY_CATEGORY) {
            g_test_fail_printf (
                "%s: category \"%s\" is %s inside \"%s\"", srv->name, nested,
                kind == NEWS_ENTRY_NONE ? "absent" : "listed as a folder",
                folder);
            goto out;
        }
    }

    /* ---- put the tree back ----
     *
     * Deepest first, so a server that refuses to remove a non-empty
     * folder still sees an empty one. Failures here are reported but
     * don't fail the test: they leave debris the container rebuild
     * clears, and the create assertions above have already run. */
    if (may_make_folder && may_make_cat && may_drop_cat) {
        if (!send_news_request (fd, &htlc, HTLC_HDR_DELNEWSDIRCAT, in_nested, 2,
                                0, NULL)) {
            g_test_message ("%s: no reply removing the nested category",
                            srv->name);
        }
    }
    if (may_make_cat && may_drop_cat) {
        if (!send_news_request (fd, &htlc, HTLC_HDR_DELNEWSDIRCAT, at_root_cat,
                                1, 0, NULL)) {
            g_test_message ("%s: no reply removing the root category",
                            srv->name);
        }
    }
    if (may_make_folder && may_drop_folder) {
        if (!send_news_request (fd, &htlc, HTLC_HDR_DELNEWSDIRCAT, in_folder,
                                1, 0, NULL)) {
            g_test_message ("%s: no reply removing the folder", srv->name);
        }
    }

out:
    g_free (folder);
    g_free (category);
    g_free (nested);
    integration_release_htlc (&htlc);
    integration_close (fd);
}

/* ---- Registration ---------------------------------------------------- */

/*
 * Stands in for the matrix subtests when the matrix produced no target.
 * A binary that registered nothing at all still exits 0, which reads as
 * a pass — so the empty case gets a test of its own that says why.
 */
static void
test_matrix_is_empty (void)
{
    g_test_fail_printf (
        "no matrix entry advertises HX_TEST_CAP_NEWS_15 (after the "
        "GTKHX_TEST_SERVERS filter), so the threaded-news subtests had "
        "nothing to run against.");
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/integration/news15/seeded_root_listing",
                     test_seeded_root_listing);

    GPtrArray *servers = hx_test_servers_with (HX_TEST_CAP_NEWS_15);
    if (!servers || servers->len == 0) {
        g_test_add_func ("/integration/news15/matrix_populated",
                         test_matrix_is_empty);
        g_clear_pointer (&servers, g_ptr_array_unref);
        return g_test_run ();
    }

    for (guint i = 0; i < servers->len; i++) {
        const hx_test_server *srv = g_ptr_array_index (servers, i);
        gchar *p;

        p = g_strdup_printf ("/integration/news15/%s/root_dirlist", srv->name);
        g_test_add_data_func (p, srv, test_root_dirlist);
        g_free (p);

        p = g_strdup_printf ("/integration/news15/%s/create_roundtrip",
                             srv->name);
        g_test_add_data_func (p, srv, test_create_roundtrip);
        g_free (p);
    }
    g_ptr_array_unref (servers);

    return g_test_run ();
}
