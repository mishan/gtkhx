/*
 * tasks_table.c — see tasks_table.h.
 *
 * The whole point of this file is to keep the GHashTable factory and
 * its value-destroy notify GTK-free, so the unit tests in
 * tests/unit/test_task_hash.c can verify the lifecycle contract
 * (insert / replace-on-duplicate / remove / destroy → task_free
 * runs and reclaims tsk->str + the task struct itself) without
 * pulling in tasks.c, which depends on the GTK widgets that draw
 * the Tasks window.
 */

#include "config.h"
#include <glib.h>
#include "protocol.h"
#include "tasks_table.h"

/* task lifecycle on GHashTable.
 *
 * task_free() is the GDestroyNotify the hashtable invokes whenever a
 * value is replaced (g_hash_table_insert with an existing key),
 * removed (g_hash_table_remove / _foreach_remove / _iter_remove), or
 * the table itself is destroyed. Frees the task's owned heap state
 * (the optional `str` label, the optional per-task `ptr` context if
 * a destructor was registered) and the task struct itself.
 *
 * The ptr_free hook lets callers tie a per-task heap context's
 * lifetime to the task entry. The biggest win is disconnect-time
 * cleanup: hx_htlc_close walks sess->tasks with
 * g_hash_table_remove_all, which fires task_free per surviving
 * entry — previously the rcv_task callback's owned context (e.g.
 * inline-media upload/download ctx) leaked because the callback
 * never ran. With ptr_free wired up the context is reclaimed
 * automatically. */
static void
task_free (gpointer p)
{
    struct task *tsk = p;
    if (!tsk) {
        return;
    }
    if (tsk->ptr && tsk->ptr_free) {
        tsk->ptr_free (tsk->ptr);
    }
    g_free (tsk->str);
    g_free (tsk);
}

GHashTable *
tasks_table_new (void)
{
    /* trans is a guint32; we cast it to gpointer via GUINT_TO_POINTER
	 * at insert time, so g_direct_hash / g_direct_equal are the
	 * correct hash + equality functions. Keys are pointer-sized
	 * integers, not heap allocations — no key destroy notify. */
    return g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL,
                                  task_free);
}
