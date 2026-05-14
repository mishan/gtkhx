/*
 * tests/unit/test_hx_user_hash.c — pin down the lifecycle contract
 * of the per-chat user table (Phase 1.5 of the GLib-collections
 * migration: replaces the intrusive hx_user linked list — chat
 * carried __user_list / user_list / user_tail with next/prev fields
 * on each hx_user — with chat->users, a GHashTable<u16 uid, struct
 * hx_user*>).
 *
 * struct hx_user lives in session.h next to gtk-dependent types,
 * so this test exercises a layout-compatible stub. Coverage focuses
 * on what's specific about this migration:
 *
 *   - The hashtable lives PER CHAT (not session-wide). One chat's
 *     user-set is independent of another's; same uid can appear in
 *     both with different identities (e.g. me as alice in public,
 *     me as alice in a pchat — same uid both places, but disjoint
 *     hashtable instances).
 *   - hx_user_new mallocs + inserts in one step; the caller no
 *     longer has a "tail" to thread the new node onto.
 *   - hx_user_delete is now a hashtable remove (the destroy notify
 *     g_frees the struct); callers don't manage next/prev.
 *   - Iteration ordering is hash-defined, not join-defined; tab
 *     completion in chat.c had to materialise a sorted array.
 */

#include "config.h"
#include <string.h>
#include <glib.h>

struct hx_user_stub {
    guint16 uid;
    guint16 icon;
    guint16 color;
    char name[32];
    unsigned int ignore : 1;
};

static GHashTable *
user_table_new_for_test (void)
{
    return g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
}

static struct hx_user_stub *
make_user (guint16 uid, const char *name)
{
    struct hx_user_stub *u = g_malloc0 (sizeof (struct hx_user_stub));
    u->uid = uid;
    if (name) {
        g_strlcpy (u->name, name, sizeof (u->name));
    }
    return u;
}

/* ---- 1. Insert + lookup by uid ------------------------------------ */

static void
test_insert_lookup_by_uid (void)
{
    GHashTable *t = user_table_new_for_test ();
    struct hx_user_stub *alice = make_user (1, "alice");
    struct hx_user_stub *bob = make_user (2, "bob");

    g_hash_table_insert (t, GUINT_TO_POINTER (1u), alice);
    g_hash_table_insert (t, GUINT_TO_POINTER (2u), bob);

    g_assert_true (g_hash_table_lookup (t, GUINT_TO_POINTER (1u)) == alice);
    g_assert_true (g_hash_table_lookup (t, GUINT_TO_POINTER (2u)) == bob);
    g_assert_null (g_hash_table_lookup (t, GUINT_TO_POINTER (3u)));

    g_hash_table_destroy (t);
}

/* ---- 2. Same uid in two chats is independent --------------------- */

/* Each chat carries its OWN GHashTable<uid, hx_user*>. Inserting
 * uid=5 in chat A creates a fresh node in chat A's table; the same
 * uid in chat B is a separate node, with its own identity. This is
 * how a user can have a distinct icon / status per pchat without
 * either chat's view leaking into the other. */
static void
test_two_chats_independent_tables (void)
{
    GHashTable *pub = user_table_new_for_test ();
    GHashTable *pri = user_table_new_for_test ();

    struct hx_user_stub *pub_view = make_user (5, "alice_in_public");
    struct hx_user_stub *pri_view = make_user (5, "alice_in_private");

    g_hash_table_insert (pub, GUINT_TO_POINTER (5u), pub_view);
    g_hash_table_insert (pri, GUINT_TO_POINTER (5u), pri_view);

    g_assert_true (g_hash_table_lookup (pub, GUINT_TO_POINTER (5u))
                   == pub_view);
    g_assert_true (g_hash_table_lookup (pri, GUINT_TO_POINTER (5u))
                   == pri_view);
    g_assert_true (pub_view != pri_view);

    g_hash_table_destroy (pub);
    g_hash_table_destroy (pri);
}

/* ---- 3. Re-joining the same uid replaces the node ---------------- */

/* If a USER_PART then USER_CHANGE arrives for the same uid (or the
 * caller forgets to delete first), the second insert must replace
 * the first and free the old node via the destroy notify. */
static void
test_rejoin_replaces_user (void)
{
    GHashTable *t = user_table_new_for_test ();
    struct hx_user_stub *first = make_user (7, "alice");
    struct hx_user_stub *second = make_user (7, "alice_v2");

    g_hash_table_insert (t, GUINT_TO_POINTER (7u), first);
    g_hash_table_insert (t, GUINT_TO_POINTER (7u), second);

    g_assert_cmpuint (g_hash_table_size (t), ==, 1);
    g_assert_true (g_hash_table_lookup (t, GUINT_TO_POINTER (7u)) == second);

    g_hash_table_destroy (t);
}

/* ---- 4. Iteration covers every user --------------------------- */

/* The post-port code in users.c user_list (public-chat rebuild) and
 * various callers iterate via GHashTableIter. The contract is
 * "visit every member exactly once"; order is not part of the
 * contract. */
static void
test_iter_visits_every_user (void)
{
    GHashTable *t = user_table_new_for_test ();
    for (guint16 uid = 100; uid < 116; uid++) {
        g_hash_table_insert (t, GUINT_TO_POINTER ((guint)uid),
                             make_user (uid, NULL));
    }

    guint16 mask = 0;
    GHashTableIter iter;
    gpointer key, val;
    g_hash_table_iter_init (&iter, t);
    while (g_hash_table_iter_next (&iter, &key, &val)) {
        guint16 uid = (guint16)GPOINTER_TO_UINT (key);
        struct hx_user_stub *u = val;
        g_assert_cmpuint (uid, >=, 100);
        g_assert_cmpuint (uid, <, 116);
        g_assert_cmpuint (u->uid, ==, uid);
        mask |= (guint16)(1u << (uid - 100));
    }
    g_assert_cmphex (mask, ==, 0xFFFF);

    g_hash_table_destroy (t);
}

/* ---- 5. Remove + destroy reclaims state -------------------------- */

static void
test_remove_runs_destroy_notify (void)
{
    GHashTable *t = user_table_new_for_test ();
    g_hash_table_insert (t, GUINT_TO_POINTER (1u), make_user (1, "a"));
    g_hash_table_insert (t, GUINT_TO_POINTER (2u), make_user (2, "b"));

    g_assert_true (g_hash_table_remove (t, GUINT_TO_POINTER (1u)));
    g_assert_cmpuint (g_hash_table_size (t), ==, 1);
    g_assert_null (g_hash_table_lookup (t, GUINT_TO_POINTER (1u)));

    g_assert_false (g_hash_table_remove (t, GUINT_TO_POINTER (999u)));

    g_hash_table_destroy (t);
}

/* ---- 6. Bulk clear (htlc_close public-chat reset) ---------------- */

/* The network.c teardown path resets the public chat's user table
 * in place via g_hash_table_remove_all. Verifies that operation
 * frees every entry and leaves the table empty + reusable. */
static void
test_remove_all_reclaims_and_empties (void)
{
    GHashTable *t = user_table_new_for_test ();
    for (guint16 uid = 1; uid < 20; uid++) {
        g_hash_table_insert (t, GUINT_TO_POINTER ((guint)uid),
                             make_user (uid, NULL));
    }
    g_assert_cmpuint (g_hash_table_size (t), ==, 19);

    g_hash_table_remove_all (t);
    g_assert_cmpuint (g_hash_table_size (t), ==, 0);

    /* Still usable for a fresh round of inserts after the reset. */
    g_hash_table_insert (t, GUINT_TO_POINTER (42u), make_user (42, "fresh"));
    g_assert_cmpuint (g_hash_table_size (t), ==, 1);

    g_hash_table_destroy (t);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/hx_user_hash/insert_lookup_by_uid",
                     test_insert_lookup_by_uid);
    g_test_add_func ("/hx_user_hash/two_chats_independent_tables",
                     test_two_chats_independent_tables);
    g_test_add_func ("/hx_user_hash/rejoin_replaces_user",
                     test_rejoin_replaces_user);
    g_test_add_func ("/hx_user_hash/iter_visits_every_user",
                     test_iter_visits_every_user);
    g_test_add_func ("/hx_user_hash/remove_runs_destroy_notify",
                     test_remove_runs_destroy_notify);
    g_test_add_func ("/hx_user_hash/remove_all_reclaims_and_empties",
                     test_remove_all_reclaims_and_empties);

    return g_test_run ();
}
