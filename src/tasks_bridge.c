/*
 * tasks_bridge.c — see tasks_bridge.h.
 *
 * Field accessors the Rust `hxtask` crate externs to reach the not-yet-ported C
 * session / htlc_conn structs, plus the _Static_asserts that pin `struct task`'s
 * layout so the crate's #[repr(C)] mirror can't silently drift.
 */

#include "config.h"

#include <glib.h>
#include <stddef.h>

#include "protocol.h"   /* struct htlc_conn, struct task */
#include "session.h"    /* struct _session (for ->tasks) */
#include "tasks_bridge.h"

/* sess_from_htlc is a static-inline container_of in session.h — not a linkable
 * symbol — so wrap it for the crate. */
session *
hx_sess_from_htlc (struct htlc_conn *htlc)
{
    return sess_from_htlc (htlc);
}

GHashTable *
hx_session_tasks (session *sess)
{
    return sess->tasks;
}

void
hx_session_set_tasks (session *sess, GHashTable *table)
{
    sess->tasks = table;
}

guint32
hx_htlc_trans (struct htlc_conn *htlc)
{
    return htlc->trans;
}

/* Pin struct task's layout (protocol.h) so the #[repr(C)] Task mirror in
 * rust/crates/hxtask/src/lib.rs stays in lockstep. LP64. */
_Static_assert (offsetof (struct task, trans) == 0, "task.trans offset");
_Static_assert (offsetof (struct task, pos) == 4, "task.pos offset");
_Static_assert (offsetof (struct task, len) == 8, "task.len offset");
_Static_assert (offsetof (struct task, data) == 16, "task.data offset");
_Static_assert (offsetof (struct task, str) == 24, "task.str offset");
_Static_assert (offsetof (struct task, ptr) == 32, "task.ptr offset");
_Static_assert (offsetof (struct task, ptr_free) == 40, "task.ptr_free offset");
_Static_assert (offsetof (struct task, rcv) == 48, "task.rcv offset");
_Static_assert (sizeof (struct task) == 56, "task size");
