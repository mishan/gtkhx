#ifndef HX_TASKS_H
#define HX_TASKS_H

/* Build the task queue's widgets. Once for the application: the queue is
 * shared by every connection (docs/multi-connection.md, "Global but tagged").
 * Idempotent, so a per-connection call site is harmless. */
extern void create_tasks (void);
extern void output_xfer_queue (session *sess, struct htxf_conn *htxf);
/* Drop every row belonging to `htlc`. For disconnect: the queue is shared by
 * every connection, so nothing else removes a departing connection's rows. */
extern void gtasks_delete_on_conn (struct htlc_conn *htlc);

/* Drop the tracker's progress rows. They belong to no connection, so the
 * per-connection sweep leaves them alone; this is what ends them when a fetch
 * stops early. */
extern void gtasks_delete_tracker_rows (void);

/* How many rows the shared queue holds for a connection. For the debug hooks
 * that exercise open-and-close headlessly. */
extern guint gtkhx_tasks_rows_for_conn (guint16 conn);

/* Re-label the rows with the connection each belongs to, and show or hide
 * those labels — they only appear above one connection. Call when the set of
 * connections changes, including when one learns its name at login. */
extern void gtkhx_tasks_refresh_tags (void);
extern void gtask_delete_tsk (session *sess, guint32 trans);
extern void gtask_delete_htxf (session *sess, struct htxf_conn *htxf);
/* Sever the gtask's pointer to htxf without removing the UI row.
 * Wired to the GtkhxSession::xfer-destroyed signal so the tasks
 * window can't crash on a dangling htxf pointer after the xfer
 * leaves the live xfers[] list. */
extern void gtask_clear_htxf (session *sess, struct htxf_conn *htxf);
extern void task_update (session *sess, struct task *tsk);
/* create_tasks_window is the gtkhx-ui `tasks` Rust shell (dock registration
 * via dock_bridge); these two are its C content-build + post-embed
 * lifecycle hooks, mirroring users_bridge.c. */
extern void create_tasks_window (GtkWidget *widget, gpointer data);
extern GtkWidget *gtkhx_tasks_build_content (session *sess);
extern void gtkhx_tasks_after_embed (session *sess);

/* Push one connection's tasks and transfers into the shared queue. For a
 * connection joining a panel another one already built. */
extern void gtkhx_tasks_sync_conn (session *sess);
extern void file_update (session *sess, struct htxf_conn *htxf);
extern void task_delete (session *sess, struct task *tsk);
extern void task_error (struct htlc_conn *htlc, const guint8 *frame,
                        gsize frame_len);
/* Pure-protocol-parsing half of task_error, broken out into
 * proto_helpers.c for the Tier 2 unit tests. */
#include "proto_helpers.h"
extern void conn_task_update (session *sess, int stat);
extern struct task *task_new (struct htlc_conn *htlc, rcv_task_fn rcv,
                              void *ptr, void *data, const char *str);
extern struct task *task_with_trans (session *sess, guint32 trans);
/* lazy-allocate the session's task GHashTable. Safe to
 * call multiple times — only the first call constructs the table.
 * gtkhx.c calls it before the first task_new at startup. */
extern void tasks_init (session *sess);
extern void track_prog_update (session *sess, char *str, int num, int total);
extern void trackconn_prog_update (session *sess, char *str, int num,
                                   int total);

#endif
