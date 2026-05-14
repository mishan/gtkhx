/*
 * tests/unit/test_gchat_hash.c — pin down the lifecycle contract of
 * the per-session gtkhx_chat (UI side) table (Phase 1.4 of the
 * GLib-collections migration: replaces session->gchat_list, a
 * doubly-linked list of open chat / pchat windows, with
 * GHashTable<u32 cid, struct gtkhx_chat*>).
 *
 * The runtime destroy notify (gchat_free in chat.c) is just g_free —
 * the GtkWidget subtree owned by gchat->window is reclaimed when
 * the parent window is destroyed by the close-request handler. So
 * the table contract here is the simplest of the Phase 1 batch:
 * g_direct_hash / g_direct_equal on the cid + a g_free value destroy.
 *
 * struct gtkhx_chat sits in session.h alongside gtk-dependent types,
 * so this test uses a layout-compatible stub. Coverage focuses on
 * the patterns specific to the gchats table:
 *
 *   1. cid=0 (public chat) coexists with multiple pchat entries.
 *   2. Iteration visits public + pchats; key=0 is recognisable.
 *   3. Removing a pchat does not perturb the public entry.
 *   4. The teardown sweep (gtkutil.c close_connected_windows) —
 *      collect non-public keys, then bulk-remove without mutating
 *      mid-iteration — leaves the public entry in place.
 */

#include "config.h"
#include <string.h>
#include <glib.h>

/* Layout-compatible stub of struct gtkhx_chat. The runtime gchat_free
 * just g_frees the struct — none of the widget pointers matter. */
struct gchat_stub {
    void *window;
    void *output;
    void *input;
    guint32 cid;
};

static GHashTable *
gchat_table_new_for_test (void)
{
    return g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
}

static struct gchat_stub *
make_gchat (guint32 cid)
{
    struct gchat_stub *g = g_malloc0 (sizeof (struct gchat_stub));
    g->cid = cid;
    return g;
}

/* ---- 1. Public + pchats coexist ----------------------------------- */

static void
test_public_and_pchats_coexist (void)
{
    GHashTable *t = gchat_table_new_for_test ();
    struct gchat_stub *pub = make_gchat (0);
    struct gchat_stub *pa = make_gchat (0xA01);
    struct gchat_stub *pb = make_gchat (0xA02);

    g_hash_table_insert (t, GUINT_TO_POINTER (0u), pub);
    g_hash_table_insert (t, GUINT_TO_POINTER (0xA01u), pa);
    g_hash_table_insert (t, GUINT_TO_POINTER (0xA02u), pb);

    g_assert_cmpuint (g_hash_table_size (t), ==, 3);
    g_assert_true (g_hash_table_lookup (t, GUINT_TO_POINTER (0u)) == pub);
    g_assert_true (g_hash_table_lookup (t, GUINT_TO_POINTER (0xA01u)) == pa);
    g_assert_true (g_hash_table_lookup (t, GUINT_TO_POINTER (0xA02u)) == pb);

    g_hash_table_destroy (t);
}

/* ---- 2. Iteration recognises public vs pchat by key=0 ------------- */

/* options.c reinit_gtktexts walks gchats and gates the public-chat
 * branch on `key == 0 && !chat.open` — this is the iteration shape
 * that drives all four settings change handlers in that file. */
static void
test_iter_classifies_by_key (void)
{
    GHashTable *t = gchat_table_new_for_test ();
    g_hash_table_insert (t, GUINT_TO_POINTER (0u), make_gchat (0));
    g_hash_table_insert (t, GUINT_TO_POINTER (10u), make_gchat (10));
    g_hash_table_insert (t, GUINT_TO_POINTER (20u), make_gchat (20));

    int public_count = 0, pchat_count = 0;
    GHashTableIter iter;
    gpointer key, val;
    g_hash_table_iter_init (&iter, t);
    while (g_hash_table_iter_next (&iter, &key, &val)) {
        struct gchat_stub *g = val;
        if (GPOINTER_TO_UINT (key) == 0) {
            public_count++;
            g_assert_cmpuint (g->cid, ==, 0);
        } else {
            pchat_count++;
            g_assert_cmpuint (g->cid, !=, 0);
        }
    }
    g_assert_cmpint (public_count, ==, 1);
    g_assert_cmpint (pchat_count, ==, 2);

    g_hash_table_destroy (t);
}

/* ---- 3. Removing a pchat leaves the public alone ------------------ */

static void
test_remove_pchat_preserves_public (void)
{
    GHashTable *t = gchat_table_new_for_test ();
    struct gchat_stub *pub = make_gchat (0);
    g_hash_table_insert (t, GUINT_TO_POINTER (0u), pub);
    g_hash_table_insert (t, GUINT_TO_POINTER (0xC0u), make_gchat (0xC0));

    g_assert_true (g_hash_table_remove (t, GUINT_TO_POINTER (0xC0u)));
    g_assert_cmpuint (g_hash_table_size (t), ==, 1);
    g_assert_true (g_hash_table_lookup (t, GUINT_TO_POINTER (0u)) == pub);

    g_hash_table_destroy (t);
}

/* ---- 4. Teardown pattern: collect-then-remove --------------------- */

/* This mirrors close_connected_windows in gtkutil.c. The naïve
 * approach — iter + g_hash_table_iter_remove — would also work, but
 * the runtime code destroys the GTK widget BEFORE removing the entry
 * (so the cid is still readable for follow-up logging), which means
 * we iterate first, accumulate non-public keys, then bulk-remove.
 * Verify that pattern is sound: the public entry stays. */
static void
test_teardown_collect_then_remove (void)
{
    GHashTable *t = gchat_table_new_for_test ();
    struct gchat_stub *pub = make_gchat (0);
    g_hash_table_insert (t, GUINT_TO_POINTER (0u), pub);
    for (guint32 i = 1; i < 6; i++) {
        g_hash_table_insert (t, GUINT_TO_POINTER (i), make_gchat (i));
    }

    g_assert_cmpuint (g_hash_table_size (t), ==, 6);

    GList *to_close = NULL;
    GHashTableIter iter;
    gpointer key, val;
    g_hash_table_iter_init (&iter, t);
    while (g_hash_table_iter_next (&iter, &key, &val)) {
        if (GPOINTER_TO_UINT (key) != 0) {
            to_close = g_list_prepend (to_close, key);
        }
    }
    for (GList *l = to_close; l; l = l->next) {
        g_hash_table_remove (t, l->data);
    }
    g_list_free (to_close);

    g_assert_cmpuint (g_hash_table_size (t), ==, 1);
    g_assert_true (g_hash_table_lookup (t, GUINT_TO_POINTER (0u)) == pub);

    g_hash_table_destroy (t);
}

/* ---- 5. Destroying the table reclaims all entries ----------------- */

static void
test_destroy_reclaims_all (void)
{
    GHashTable *t = gchat_table_new_for_test ();
    for (guint32 i = 0; i < 10; i++) {
        g_hash_table_insert (t, GUINT_TO_POINTER (i), make_gchat (i));
    }
    g_assert_cmpuint (g_hash_table_size (t), ==, 10);
    g_hash_table_destroy (t);
    /* Leaks would show up under ASan / valgrind. */
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/gchat_hash/public_and_pchats_coexist",
                     test_public_and_pchats_coexist);
    g_test_add_func ("/gchat_hash/iter_classifies_by_key",
                     test_iter_classifies_by_key);
    g_test_add_func ("/gchat_hash/remove_pchat_preserves_public",
                     test_remove_pchat_preserves_public);
    g_test_add_func ("/gchat_hash/teardown_collect_then_remove",
                     test_teardown_collect_then_remove);
    g_test_add_func ("/gchat_hash/destroy_reclaims_all",
                     test_destroy_reclaims_all);

    return g_test_run ();
}
