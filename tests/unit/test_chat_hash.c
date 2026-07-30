/*
 * tests/unit/test_chat_hash.c — pin down the lifecycle contract of
 * the per-session chat table (Phase 1.3 of the GLib-collections
 * migration: replaces the chat_front / chat_tail / chat_list /
 * __chat_list sentinel quartet on the session with
 * GHashTable<u32 cid, struct chat*>).
 *
 * struct chat is defined in session.h alongside types that pull in
 * gtk + adwaita; the chat itself has no GtkWidget fields but its
 * neighbours in the header do. Rather than linking chat.c (which
 * does pull in the whole UI stack) the test exercises a layout-
 * compatible stub that mirrors only the destroy-relevant shape:
 * intrusive __user_list sentinel + a user_list pointer + a tail
 * pointer. The runtime destroy notify in chat.c walks the same
 * shape; if the layout assumption breaks, the integration suite
 * (which goes against mhxd through the real struct chat) catches it.
 *
 * Contract pinned down here:
 *
 *   1. (g_direct_hash, g_direct_equal) on guint32 cids round-trips
 *      through insert / lookup.
 *   2. cid=0 (the public chat) is a real key, not a sentinel —
 *      contains addressable / removable / iterable like any other.
 *   3. A second insert with the same cid replaces the first; the
 *      old value's user-list nodes AND the chat itself are freed.
 *   4. g_hash_table_remove on a cid runs the destroy notify and
 *      reclaims the chat (and its users) heap state.
 *   5. Destroying the table reclaims every surviving chat.
 *   6. The destroy notify walks the intrusive user list properly
 *      and frees each heap-allocated hx_user_stub node.
 */

#include "config.h"
#include <string.h>
#include <glib.h>

/* Layout-compatible stub of struct hx_user. Real hx_user_struct
 * carries icon / color / name and an `ignore` bit; the destroy
 * notify only walks next/prev and frees, so those extras are
 * irrelevant here. */
struct hx_user_stub {
    struct hx_user_stub *next, *prev;
    guint16 uid;
};

/* Layout-compatible stub of struct chat — exactly the fields the
 * runtime chat_free destroy notify touches. */
struct chat_stub {
    guint32 cid;
    guint32 nusers;
    struct hx_user_stub __user_list;
    struct hx_user_stub *user_list;
    struct hx_user_stub *user_tail;
    char subject[256];
};

static void
chat_stub_free (gpointer p)
{
    struct chat_stub *chat = p;
    struct hx_user_stub *u, *unext;

    if (!chat) {
        return;
    }
    if (chat->user_list) {
        for (u = chat->user_list->next; u; u = unext) {
            unext = u->next;
            g_free (u);
        }
    }
    g_free (chat);
}

static GHashTable *
chat_table_new_for_test (void)
{
    return g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL,
                                  chat_stub_free);
}

static struct chat_stub *
make_chat (guint32 cid)
{
    struct chat_stub *c = g_malloc0 (sizeof (struct chat_stub));
    c->cid = cid;
    c->user_list = &c->__user_list;
    c->user_tail = &c->__user_list;
    return c;
}

/* Append a heap-allocated user onto chat->__user_list (mirrors
 * what users.c hx_user_new does for the real list). The test
 * doesn't care about ordering, just that the destroy notify
 * walks the chain end-to-end. */
static void
add_user (struct chat_stub *c, guint16 uid)
{
    struct hx_user_stub *u = g_malloc0 (sizeof (struct hx_user_stub));
    u->uid = uid;
    u->prev = c->user_tail;
    c->user_tail->next = u;
    c->user_tail = u;
    c->nusers++;
}

/* ---- 1. Insert + lookup by cid ------------------------------------ */

static void
test_insert_lookup_by_cid (void)
{
    GHashTable *t = chat_table_new_for_test ();
    struct chat_stub *pub = make_chat (0); /* public chat */
    struct chat_stub *priv = make_chat (0xA17);

    g_hash_table_insert (t, GUINT_TO_POINTER (0u), pub);
    g_hash_table_insert (t, GUINT_TO_POINTER (0xA17u), priv);

    g_assert_cmpuint (g_hash_table_size (t), ==, 2);
    g_assert_true (g_hash_table_lookup (t, GUINT_TO_POINTER (0u)) == pub);
    g_assert_true (g_hash_table_lookup (t, GUINT_TO_POINTER (0xA17u)) == priv);
    g_assert_null (g_hash_table_lookup (t, GUINT_TO_POINTER (0xBADu)));

    g_hash_table_destroy (t);
}

/* ---- 2. cid=0 (public chat) is a real entry, not a sentinel -------- */

/* In the pre-migration scheme cid=0 was structurally special — the
 * embedded __chat_list sentinel on the session. In the migrated
 * scheme it's a regular hashtable entry, just one that chats_init()
 * seeds at startup so chat_with_cid(sess, 0) is never NULL. The
 * test verifies cid=0 behaves like any other entry: present in
 * size, contains(), and iteration. */
static void
test_cid_zero_is_a_real_entry (void)
{
    GHashTable *t = chat_table_new_for_test ();
    struct chat_stub *pub = make_chat (0);

    g_hash_table_insert (t, GUINT_TO_POINTER (0u), pub);
    g_assert_true (g_hash_table_contains (t, GUINT_TO_POINTER (0u)));
    g_assert_cmpuint (g_hash_table_size (t), ==, 1);

    /* Iteration must visit it. */
    GHashTableIter iter;
    gpointer key, val;
    g_hash_table_iter_init (&iter, t);
    g_assert_true (g_hash_table_iter_next (&iter, &key, &val));
    g_assert_cmpuint (GPOINTER_TO_UINT (key), ==, 0);
    g_assert_true (val == pub);
    g_assert_false (g_hash_table_iter_next (&iter, &key, &val));

    g_hash_table_destroy (t);
}

/* ---- 3. Insert-on-duplicate replaces + frees old ------------------- */

/* If a future caller re-creates a chat with an existing cid (the
 * current rcv.c path looks it up first and reuses, but the table
 * semantics must be correct regardless), the new chat must
 * displace the old AND the destroy notify must reclaim the old
 * chat's user list. */
static void
test_duplicate_cid_replaces_value (void)
{
    GHashTable *t = chat_table_new_for_test ();
    struct chat_stub *first = make_chat (7);
    struct chat_stub *second = make_chat (7);

    /* `first` carries three users; if we drop the destroy notify
     * those three g_malloc'd nodes leak. valgrind / ASan catch
     * the regression at runtime; the in-test assertions verify
     * the visible API stays consistent. */
    add_user (first, 100);
    add_user (first, 101);
    add_user (first, 102);

    g_hash_table_insert (t, GUINT_TO_POINTER (7u), first);
    g_assert_cmpuint (g_hash_table_size (t), ==, 1);

    g_hash_table_insert (t, GUINT_TO_POINTER (7u), second);
    g_assert_cmpuint (g_hash_table_size (t), ==, 1);
    g_assert_true (g_hash_table_lookup (t, GUINT_TO_POINTER (7u)) == second);

    g_hash_table_destroy (t);
}

/* ---- 4. Remove runs the destroy notify ---------------------------- */

static void
test_remove_runs_destroy_notify (void)
{
    GHashTable *t = chat_table_new_for_test ();
    struct chat_stub *a = make_chat (10);
    struct chat_stub *b = make_chat (11);
    add_user (a, 1);
    add_user (a, 2);
    add_user (b, 99);

    g_hash_table_insert (t, GUINT_TO_POINTER (10u), a);
    g_hash_table_insert (t, GUINT_TO_POINTER (11u), b);

    g_assert_true (g_hash_table_remove (t, GUINT_TO_POINTER (10u)));
    g_assert_cmpuint (g_hash_table_size (t), ==, 1);
    g_assert_null (g_hash_table_lookup (t, GUINT_TO_POINTER (10u)));
    g_assert_true (g_hash_table_lookup (t, GUINT_TO_POINTER (11u)) == b);

    g_assert_false (g_hash_table_remove (t, GUINT_TO_POINTER (999u)));

    g_hash_table_destroy (t);
}

/* ---- 5. Empty user list: destroy notify still works ---------------- */

/* The error-recovery path in rcv.c::rcv_task_user_list_switch
 * calls chat_delete on a chat that was just created and has no
 * users yet (user_list == user_tail == &__user_list, __user_list.next
 * is NULL). The destroy notify must handle that without
 * dereferencing past the end. */
static void
test_destroy_empty_chat (void)
{
    GHashTable *t = chat_table_new_for_test ();
    struct chat_stub *c = make_chat (0xBEEF);
    /* No users added — user_list->next is NULL. */
    g_hash_table_insert (t, GUINT_TO_POINTER (0xBEEFu), c);
    g_assert_true (g_hash_table_remove (t, GUINT_TO_POINTER (0xBEEFu)));
    g_assert_cmpuint (g_hash_table_size (t), ==, 0);
    g_hash_table_destroy (t);
}

/* ---- 6. Destroying the table reclaims everything ------------------- */

static void
test_destroy_reclaims_all (void)
{
    GHashTable *t = chat_table_new_for_test ();
    for (guint32 cid = 0; cid < 20; cid++) {
        struct chat_stub *c = make_chat (cid);
        add_user (c, cid + 1000);
        add_user (c, cid + 2000);
        g_hash_table_insert (t, GUINT_TO_POINTER (cid), c);
    }
    g_assert_cmpuint (g_hash_table_size (t), ==, 20);
    g_hash_table_destroy (t);
    /* Leaks would show up under ASan / valgrind. */
}

/* ---- 7. The full uint32 range is addressable ----------------------- */

static void
test_cid_full_range_distinct (void)
{
    GHashTable *t = chat_table_new_for_test ();
    const guint32 ids[] = {
        0, 1, 0xFFFF, 0x10000, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFE, 0xFFFFFFFF,
    };
    const guint n = G_N_ELEMENTS (ids);

    for (guint i = 0; i < n; i++) {
        g_hash_table_insert (t, GUINT_TO_POINTER (ids[i]), make_chat (ids[i]));
    }

    g_assert_cmpuint (g_hash_table_size (t), ==, n);
    for (guint i = 0; i < n; i++) {
        struct chat_stub *c
            = g_hash_table_lookup (t, GUINT_TO_POINTER (ids[i]));
        g_assert_nonnull (c);
        g_assert_cmpuint (c->cid, ==, ids[i]);
    }

    g_hash_table_destroy (t);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/chat_hash/insert_lookup_by_cid",
                     test_insert_lookup_by_cid);
    g_test_add_func ("/chat_hash/cid_zero_is_a_real_entry",
                     test_cid_zero_is_a_real_entry);
    g_test_add_func ("/chat_hash/duplicate_cid_replaces_value",
                     test_duplicate_cid_replaces_value);
    g_test_add_func ("/chat_hash/remove_runs_destroy_notify",
                     test_remove_runs_destroy_notify);
    g_test_add_func ("/chat_hash/destroy_empty_chat", test_destroy_empty_chat);
    g_test_add_func ("/chat_hash/destroy_reclaims_all",
                     test_destroy_reclaims_all);
    g_test_add_func ("/chat_hash/cid_full_range_distinct",
                     test_cid_full_range_distinct);

    return g_test_run ();
}
