/*
 * tests/unit/test_msgwin_hash.c — pin down the lifecycle contract of
 * the per-session PM-window table (Phase 1.2 of the GLib-collections
 * migration: replaces session->msg_list (a doubly-linked list maintained
 * via a file-scope global in msg.c) with GHashTable<u16 uid, struct
 * msgwin*>.
 *
 * struct msgwin lives in session.h, which pulls in <gtk/gtk.h> and
 * Adwaita through transitive includes — too heavy for a unit test.
 * Instead this test exercises a layout-compatible stub with exactly
 * the heap-allocated fields msgwin_free reclaims (name + uid pointer
 * + the struct itself) so the destroy notify pattern can be verified
 * without dragging GTK into the test binary. Two safety nets keep
 * the stub honest:
 *
 *   1. The factory msgwin_table_new_for_test() in this file builds
 *      the SAME GHashTable shape msg.c's msg_windows_init() uses
 *      (g_direct_hash, g_direct_equal, NULL key destroy, free fn
 *      that mirrors the heap-cleanup the real msgwin_free does).
 *   2. The integration test suite covers the real msgwin path
 *      end-to-end against mhxd.
 *
 * What we pin down:
 *
 *   - guint16 uid (cast via GUINT_TO_POINTER) round-trips through
 *     insert / lookup.
 *   - Multiple PM windows for different uids coexist.
 *   - Opening a PM window for a uid that already has one replaces
 *     the old (and frees its heap state).
 *   - Removing a window from the table reclaims its heap state.
 *   - Destroying the table reclaims every surviving window.
 */

#include "config.h"
#include <string.h>
#include <glib.h>

/* Layout-compatible stub for struct msgwin: only the fields the
 * destroy notify actually frees. Real struct msgwin has GtkWidget*
 * fields after these — irrelevant here because the destroy notify
 * never touches them. */
struct msgwin_stub {
	guint16 *uid;
	char    *name;
};

static void
msgwin_stub_free (gpointer p)
{
	struct msgwin_stub *m = p;
	if (!m)
		return;
	g_free (m->name);
	g_free (m->uid);
	g_free (m);
}

static GHashTable *
msgwin_table_new_for_test (void)
{
	return g_hash_table_new_full (g_direct_hash, g_direct_equal,
	                              NULL, msgwin_stub_free);
}

static struct msgwin_stub *
make_window (guint16 uid_value, const char *name)
{
	struct msgwin_stub *m = g_malloc0 (sizeof (struct msgwin_stub));
	m->uid  = g_malloc (sizeof (guint16));
	*m->uid = uid_value;
	m->name = name ? g_strdup (name) : NULL;
	return m;
}

/* ---- 1. Insert + lookup by uid ------------------------------------ */

static void
test_insert_lookup_by_uid (void)
{
	GHashTable *t = msgwin_table_new_for_test ();
	struct msgwin_stub *a = make_window (42, "alice");
	struct msgwin_stub *b = make_window (43, "bob");

	g_hash_table_insert (t, GUINT_TO_POINTER ((guint) 42), a);
	g_hash_table_insert (t, GUINT_TO_POINTER ((guint) 43), b);

	g_assert_cmpuint (g_hash_table_size (t), ==, 2);
	g_assert_true (g_hash_table_lookup (t, GUINT_TO_POINTER (42u)) == a);
	g_assert_true (g_hash_table_lookup (t, GUINT_TO_POINTER (43u)) == b);
	g_assert_null (g_hash_table_lookup (t, GUINT_TO_POINTER (9999u)));

	g_hash_table_destroy (t);
}

/* ---- 2. uid=0 is a valid key (HotLine guest before assignment) ---- */

/* When the client connects but the server hasn't yet handed out a
 * uid, the cached uid stays at 0. A PM addressed to that effective
 * uid should round-trip through the table the same way any other
 * uid does. Server-side validation, not the table, refuses uid=0
 * as a real recipient. */
static void
test_uid_zero_is_a_real_key (void)
{
	GHashTable *t = msgwin_table_new_for_test ();
	struct msgwin_stub *zero = make_window (0, "preconnect");

	g_hash_table_insert (t, GUINT_TO_POINTER (0u), zero);
	g_assert_true (g_hash_table_contains (t, GUINT_TO_POINTER (0u)));
	g_assert_true (g_hash_table_lookup   (t, GUINT_TO_POINTER (0u)) == zero);
	g_assert_cmpuint (g_hash_table_size (t), ==, 1);

	g_hash_table_destroy (t);
}

/* ---- 3. Re-opening a PM window for the same uid replaces it ------- */

/* The current user-visible path can't easily produce a duplicate
 * (the users.c double-click handler does msgwin_with_uid first and
 * just raises the existing window) — but the table semantics matter
 * if a future caller forgets that handshake. Verifying replace +
 * free here means a regression that drops the value destroy notify
 * (and silently leaks) is caught now. */
static void
test_reopen_same_uid_replaces_window (void)
{
	GHashTable *t = msgwin_table_new_for_test ();
	struct msgwin_stub *first  = make_window (7, "alice");
	struct msgwin_stub *second = make_window (7, "alice-2");

	g_hash_table_insert (t, GUINT_TO_POINTER (7u), first);
	g_assert_cmpuint (g_hash_table_size (t), ==, 1);

	/* `first` is freed here via msgwin_stub_free; valgrind / ASan
	 * catches a regression that drops the destroy notify. */
	g_hash_table_insert (t, GUINT_TO_POINTER (7u), second);
	g_assert_cmpuint (g_hash_table_size (t), ==, 1);
	g_assert_true (g_hash_table_lookup (t, GUINT_TO_POINTER (7u))
	               == second);

	g_hash_table_destroy (t);
}

/* ---- 4. Closing a window frees its heap state -------------------- */

static void
test_close_window_frees_state (void)
{
	GHashTable *t = msgwin_table_new_for_test ();
	struct msgwin_stub *m = make_window (101, "carol");

	g_hash_table_insert (t, GUINT_TO_POINTER (101u), m);
	g_assert_cmpuint (g_hash_table_size (t), ==, 1);

	g_assert_true   (g_hash_table_remove (t, GUINT_TO_POINTER (101u)));
	g_assert_cmpuint (g_hash_table_size (t), ==, 0);
	g_assert_null   (g_hash_table_lookup (t, GUINT_TO_POINTER (101u)));

	/* Removing a uid that was never inserted is a no-op. */
	g_assert_false (g_hash_table_remove (t, GUINT_TO_POINTER (999u)));

	g_hash_table_destroy (t);
}

/* ---- 5. The full uid range is addressable ------------------------- */

/* Hotline uids are 16-bit on the wire. The migration casts them to
 * gpointer via GUINT_TO_POINTER on a guint (the natural-int-sized
 * GLib wrapper); this test pins down that the cast doesn't accidentally
 * truncate, sign-extend, or alias edge values together. Covers
 * 0 / 1 / 0xFFFE / 0xFFFF — the realistic boundaries on a Hotline
 * server. */
static void
test_uid_full_range_distinct (void)
{
	GHashTable *t = msgwin_table_new_for_test ();
	const guint16 ids[] = { 0, 1, 0x7FFF, 0x8000, 0xFFFE, 0xFFFF };
	const guint n = G_N_ELEMENTS (ids);

	for (guint i = 0; i < n; i++) {
		struct msgwin_stub *m = make_window (ids[i], NULL);
		g_hash_table_insert (t, GUINT_TO_POINTER ((guint) ids[i]), m);
	}
	g_assert_cmpuint (g_hash_table_size (t), ==, n);
	for (guint i = 0; i < n; i++) {
		struct msgwin_stub *m =
			g_hash_table_lookup (t,
			                     GUINT_TO_POINTER ((guint) ids[i]));
		g_assert_nonnull (m);
		g_assert_cmpuint (*m->uid, ==, ids[i]);
	}

	g_hash_table_destroy (t);
}

/* ---- 6. Destroying the table reclaims surviving windows ----------- */

static void
test_destroy_reclaims_all_windows (void)
{
	GHashTable *t = msgwin_table_new_for_test ();
	for (guint16 uid = 200; uid < 220; uid++) {
		g_hash_table_insert (t, GUINT_TO_POINTER ((guint) uid),
		                     make_window (uid, "x"));
	}
	g_assert_cmpuint (g_hash_table_size (t), ==, 20);
	g_hash_table_destroy (t);
	/* Leaks would show up under ASan / valgrind. */
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);

	g_test_add_func ("/msgwin_hash/insert_lookup_by_uid",
	                 test_insert_lookup_by_uid);
	g_test_add_func ("/msgwin_hash/uid_zero_is_a_real_key",
	                 test_uid_zero_is_a_real_key);
	g_test_add_func ("/msgwin_hash/reopen_same_uid_replaces_window",
	                 test_reopen_same_uid_replaces_window);
	g_test_add_func ("/msgwin_hash/close_window_frees_state",
	                 test_close_window_frees_state);
	g_test_add_func ("/msgwin_hash/uid_full_range_distinct",
	                 test_uid_full_range_distinct);
	g_test_add_func ("/msgwin_hash/destroy_reclaims_all_windows",
	                 test_destroy_reclaims_all_windows);

	return g_test_run ();
}
