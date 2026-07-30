/*
 * tasks_bridge.h — narrow C accessors into per-session / per-connection state
 * that the Rust `hxtask` crate (rust/crates/hxtask) reaches to manage the
 * transaction table. The crate owns the table lifecycle (tasks_table_new /
 * tasks_init / task_new / task_with_trans / task_delete); these shims let it
 * read/write the `sess->tasks` field and `htlc->trans` without a #[repr(C)]
 * mirror of the big session / htlc_conn structs.
 */

#ifndef HX_TASKS_BRIDGE_H
#define HX_TASKS_BRIDGE_H

#include <glib.h>
#include "gtkhx_session.h" /* the `session` typedef */

struct htlc_conn;

/* Linkable wrapper over the static-inline sess_from_htlc (session.h). */
session *hx_sess_from_htlc (struct htlc_conn *htlc);

/* sess->tasks (NULL until tasks_init). */
GHashTable *hx_session_tasks (session *sess);
void hx_session_set_tasks (session *sess, GHashTable *table);

/* htlc->trans — the current outbound transaction id task_new keys on. */
guint32 hx_htlc_trans (struct htlc_conn *htlc);

#endif /* HX_TASKS_BRIDGE_H */
