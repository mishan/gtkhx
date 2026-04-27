#ifndef HX_TASKS_H
#define HX_TASKS_H

extern GtkWidget *tasks_window;

extern void create_tasks (session *sess);
extern void output_xfer_queue (session *sess, struct htxf_conn *htxf);
extern void gtask_delete_tsk (session *sess, guint32 trans);
extern void gtask_delete_htxf (session *sess, struct htxf_conn *htxf);
extern void task_update (session *sess, struct task *tsk);
extern void create_tasks_window (GtkWidget *widget, gpointer data);
extern void file_update (session *sess, struct htxf_conn *htxf);
extern void task_delete (session *sess, struct task *tsk);
extern void task_error (struct htlc_conn *htlc);
extern void conn_task_update(session *sess, int stat);
extern struct task *task_new (struct htlc_conn *htlc, void (*rcv)(), void *ptr, void *data, const char *str);
extern struct task *task_with_trans (session *sess, guint32 trans);
extern void track_prog_update (session *sess, char *str, int num, int total);
extern void trackconn_prog_update (session *sess, char *str, int num, int total);

#endif
