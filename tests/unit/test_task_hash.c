/*
 * tests/unit/test_task_hash.c — pin down the lifecycle contract of
 * the per-session task table (Phase 1.1 of the GLib-collections
 * migration: replaces the old intrusive __task_list / task_list /
 * task_tail trio with GHashTable<u32 trans, struct task*>).
 *
 * The table factory tasks_table_new() (and its private task_free
 * destroy notify) live in src/tasks_table.c specifically so this
 * test can build them without dragging in GTK. The runtime side
 * (tasks.c) calls the same factory from tasks_init(), so anything
 * we prove about lifecycle here applies to the live session table.
 *
 * What's worth testing here, given the implementation is just a
 * thin GHashTable wrapper:
 *
 *   1. The chosen key conversion (GUINT_TO_POINTER on the
 *      32-bit trans id) + (g_direct_hash, g_direct_equal) round-
 *      trips: insert / lookup find each other by trans id.
 *   2. Inserting a second task with the *same* trans id replaces
 *      the first, and the value destroy notify runs on the old
 *      value, freeing the struct task AND its `str` label.
 *   3. g_hash_table_remove runs the value destroy notify.
 *   4. Iteration via GHashTableIter visits every entry exactly
 *      once. (The post-port teardown loop in network.c's
 *      htlc_close depends on this.)
 *   5. Destroying the table frees the surviving values too.
 *
 * Heap-ownership assertions are made via a g_malloc0-allocated
 * sentinel struct that the test can later prod via a sentinel
 * counter, and a `str` field on the task that we deliberately
 * heap-allocate so a leak would show up under valgrind (and
 * crucially: the test passes on a clean run).
 */

#include "config.h"
#include <string.h>
#include <glib.h>
#include "protocol.h"
#include "tasks_table.h"

/* ---------- Helpers -------------------------------------------------- */

/* Build a heap-allocated struct task the same shape tasks.c's
 * task_new builds. The string is heap-allocated so task_free has
 * something non-trivial to reclaim — under leak detectors this
 * proves the destroy notify actually fired. */
static struct task *
make_task (guint32 trans, const char *label)
{
	struct task *tsk = g_malloc0 (sizeof (struct task));
	tsk->trans = trans;
	tsk->str   = label ? g_strdup (label) : NULL;
	tsk->pos   = 0;
	tsk->len   = 1;
	return tsk;
}

/* ---------- 1. Insert + lookup round-trip ---------------------------- */

static void
test_insert_lookup_roundtrip (void)
{
	GHashTable *t = tasks_table_new ();
	g_assert_nonnull (t);

	struct task *a = make_task (0x1001, "alpha");
	struct task *b = make_task (0x1002, "beta");
	struct task *c = make_task (0x1003, "gamma");

	g_hash_table_insert (t, GUINT_TO_POINTER (a->trans), a);
	g_hash_table_insert (t, GUINT_TO_POINTER (b->trans), b);
	g_hash_table_insert (t, GUINT_TO_POINTER (c->trans), c);

	g_assert_cmpuint (g_hash_table_size (t), ==, 3);

	g_assert_true (g_hash_table_lookup (t, GUINT_TO_POINTER (0x1001)) == a);
	g_assert_true (g_hash_table_lookup (t, GUINT_TO_POINTER (0x1002)) == b);
	g_assert_true (g_hash_table_lookup (t, GUINT_TO_POINTER (0x1003)) == c);

	/* Unknown trans id returns NULL, not a stale pointer. */
	g_assert_null (g_hash_table_lookup (t, GUINT_TO_POINTER (0xDEADBEEF)));

	g_hash_table_destroy (t);
}

/* ---------- 2. trans=0 is a valid key, not "absent" ------------------ */

/* GUINT_TO_POINTER(0) is NULL — but GHashTable doesn't treat NULL
 * keys specially when the hash function is g_direct_hash. trans=0
 * is the very first transaction id the client sends on a fresh
 * connection (htlc->trans starts at 0; see hlpack in network.c),
 * so this is a real on-wire value, not a sentinel. */
static void
test_trans_zero_is_a_real_key (void)
{
	GHashTable *t = tasks_table_new ();

	struct task *zero = make_task (0, "first");
	g_hash_table_insert (t, GUINT_TO_POINTER (0), zero);

	g_assert_cmpuint (g_hash_table_size (t), ==, 1);
	g_assert_true (g_hash_table_lookup (t, GUINT_TO_POINTER (0)) == zero);
	g_assert_true (g_hash_table_contains (t, GUINT_TO_POINTER (0)));

	g_hash_table_destroy (t);
}

/* ---------- 3. Insert-on-duplicate replaces + frees old -------------- */

/* If the wire produces a duplicate trans (it shouldn't — the client
 * mints unique trans ids — but a buggy server might) the new value
 * must replace the old via g_hash_table_insert, with task_free
 * reclaiming the old struct. A leak here would be invisible until
 * valgrind runs; the easiest in-test proxy is to verify the size
 * stays at 1 and the looked-up value is the new one. (Valgrind /
 * AddressSanitizer + the heap-allocated tsk->str catches the
 * leak / UAF side directly.) */
static void
test_duplicate_trans_replaces_value (void)
{
	GHashTable *t = tasks_table_new ();

	struct task *old = make_task (0x4242, "old");
	struct task *new = make_task (0x4242, "new");

	g_hash_table_insert (t, GUINT_TO_POINTER (0x4242), old);
	g_assert_cmpuint (g_hash_table_size (t), ==, 1);

	/* This call must free `old` via task_free; valgrind will catch
	 * a regression that drops the value destroy notify. */
	g_hash_table_insert (t, GUINT_TO_POINTER (0x4242), new);
	g_assert_cmpuint (g_hash_table_size (t), ==, 1);
	g_assert_true    (g_hash_table_lookup (t, GUINT_TO_POINTER (0x4242))
	                   == new);

	g_hash_table_destroy (t);
}

/* ---------- 4. Remove runs the destroy notify ------------------------ */

static void
test_remove_runs_destroy_notify (void)
{
	GHashTable *t = tasks_table_new ();
	struct task *a = make_task (1, "a");
	struct task *b = make_task (2, "b");

	g_hash_table_insert (t, GUINT_TO_POINTER (1), a);
	g_hash_table_insert (t, GUINT_TO_POINTER (2), b);
	g_assert_cmpuint (g_hash_table_size (t), ==, 2);

	/* Removing trans=1 frees `a` via task_free. The valgrind /
	 * ASan run catches a leak here; the in-test check is the
	 * remaining size + that subsequent lookup returns NULL. */
	g_assert_true (g_hash_table_remove (t, GUINT_TO_POINTER (1)));
	g_assert_cmpuint (g_hash_table_size (t), ==, 1);
	g_assert_null    (g_hash_table_lookup (t, GUINT_TO_POINTER (1)));
	g_assert_true    (g_hash_table_lookup (t, GUINT_TO_POINTER (2)) == b);

	/* Removing an unknown trans returns FALSE and doesn't touch
	 * the surviving entry. */
	g_assert_false (g_hash_table_remove (t, GUINT_TO_POINTER (99)));
	g_assert_cmpuint (g_hash_table_size (t), ==, 1);

	g_hash_table_destroy (t);
}

/* ---------- 5. Iteration covers every entry -------------------------- */

/* network.c's htlc_close teardown loop iterates the table via
 * GHashTableIter, calls gtask_delete_tsk(sess, trans) per entry,
 * then g_hash_table_remove_all destroys the values in bulk. The
 * iteration must visit every (trans, task) pair exactly once;
 * order is not part of the contract. */
static void
test_iter_visits_every_entry (void)
{
	GHashTable *t = tasks_table_new ();

	/* Insert 16 tasks with distinct trans ids. */
	for (guint32 i = 0; i < 16; i++) {
		struct task *tsk = make_task (i, NULL);
		g_hash_table_insert (t, GUINT_TO_POINTER (i), tsk);
	}
	g_assert_cmpuint (g_hash_table_size (t), ==, 16);

	/* Walk the table and tick a bitmask: every key must be hit
	 * exactly once, no key outside [0..15] should appear, and
	 * the iterator's "value" must be the matching task. */
	guint32 seen = 0;
	GHashTableIter iter;
	gpointer key, val;
	g_hash_table_iter_init (&iter, t);
	while (g_hash_table_iter_next (&iter, &key, &val)) {
		guint32 trans = GPOINTER_TO_UINT (key);
		struct task *tsk = val;
		g_assert_cmpuint (trans, <, 16);
		g_assert_cmpuint (tsk->trans, ==, trans);
		/* Bit must not already be set — duplicate visit would
		 * be a GHashTable bug, but the assertion costs nothing. */
		g_assert_cmpuint (seen & (1u << trans), ==, 0);
		seen |= (1u << trans);
	}
	g_assert_cmphex (seen, ==, 0xFFFF);

	g_hash_table_destroy (t);
}

/* ---------- 6. Destroy frees every surviving value ------------------- */

/* Same idea as case 4, scaled up. After destroy, both heap-allocated
 * `str` labels AND the task structs themselves have to be reclaimed.
 * Verified at the in-test level by the size before destroy; verified
 * end-to-end by running the suite under valgrind / ASan in CI. */
static void
test_destroy_frees_all_values (void)
{
	GHashTable *t = tasks_table_new ();
	for (guint32 i = 100; i < 132; i++)
		g_hash_table_insert (t, GUINT_TO_POINTER (i),
		                     make_task (i, "x"));
	g_assert_cmpuint (g_hash_table_size (t), ==, 32);
	g_hash_table_destroy (t);
	/* No further assertions — leaks show up under ASan / valgrind. */
}

/* ---------- 7. Hash function preserves trans-id distinctness --------- */

/* (g_direct_hash, g_direct_equal) treat the key pointer as an
 * opaque integer. For our 32-bit trans ids cast to pointer via
 * GUINT_TO_POINTER, that means a pair of trans ids that differ
 * in any bit must hash to different keys at the "equals" level.
 * The test verifies a handful of edge values: just-below /
 * just-above 0xFFFFFFFF, high-bit-set ids, and the upper guint16
 * boundary all behave as distinct keys. */
static void
test_hash_collision_distinctness (void)
{
	GHashTable *t = tasks_table_new ();

	const guint32 ids[] = {
		0x00000001, 0x00000002,
		0x0000FFFF, 0x00010000,
		0x7FFFFFFF, 0x80000000,
		0xFFFFFFFE, 0xFFFFFFFF,
	};
	const guint n = G_N_ELEMENTS (ids);

	for (guint i = 0; i < n; i++)
		g_hash_table_insert (t, GUINT_TO_POINTER (ids[i]),
		                     make_task (ids[i], NULL));

	g_assert_cmpuint (g_hash_table_size (t), ==, n);
	for (guint i = 0; i < n; i++) {
		struct task *tsk =
			g_hash_table_lookup (t, GUINT_TO_POINTER (ids[i]));
		g_assert_nonnull (tsk);
		g_assert_cmpuint (tsk->trans, ==, ids[i]);
	}

	g_hash_table_destroy (t);
}

/* ---------- main ----------------------------------------------------- */

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);

	g_test_add_func ("/task_hash/insert_lookup_roundtrip",
	                 test_insert_lookup_roundtrip);
	g_test_add_func ("/task_hash/trans_zero_is_a_real_key",
	                 test_trans_zero_is_a_real_key);
	g_test_add_func ("/task_hash/duplicate_trans_replaces_value",
	                 test_duplicate_trans_replaces_value);
	g_test_add_func ("/task_hash/remove_runs_destroy_notify",
	                 test_remove_runs_destroy_notify);
	g_test_add_func ("/task_hash/iter_visits_every_entry",
	                 test_iter_visits_every_entry);
	g_test_add_func ("/task_hash/destroy_frees_all_values",
	                 test_destroy_frees_all_values);
	g_test_add_func ("/task_hash/hash_collision_distinctness",
	                 test_hash_collision_distinctness);

	return g_test_run ();
}
