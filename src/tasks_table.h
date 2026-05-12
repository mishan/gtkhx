/*
 * tasks_table.h — factory for the per-session task GHashTable.
 *
 * Carved out of tasks.c so the unit tests can construct a table
 * identical to the one tasks_init() builds at runtime, without
 * dragging in GTK / Adwaita / the rest of the tasks.c UI machinery.
 *
 * The table maps a 32-bit transaction id (the protocol-level handle
 * the server uses to correlate replies to our outbound requests) to
 * a heap-allocated struct task. The value destroy notify (task_free,
 * private to tasks_table.c) frees the task struct AND its optional
 * heap-allocated `str` label, so g_hash_table_remove / _replace /
 * _destroy do the full job — callers never have to peek inside the
 * task to clean up first.
 */

#ifndef HX_TASKS_TABLE_H
#define HX_TASKS_TABLE_H

#include <glib.h>

/* Build a fresh task table. Caller owns the returned GHashTable and
 * must g_hash_table_destroy / g_hash_table_unref it when done. */
GHashTable *tasks_table_new (void);

#endif /* HX_TASKS_TABLE_H */
