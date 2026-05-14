#ifndef HX_XFERS_H
#define HX_XFERS_H

extern int nxfers;
extern struct htxf_conn **xfers;

//extern size_t resource_len (const char *path);
extern void xfer_go (struct htxf_conn *htxf);
extern int xfer_go_timer (void *__arg);
/* Construct a transfer. The remote location is given as a (dir,
 * name, name_len) triple rather than a single joined path so that
 * names containing `/` (the default dir_char) survive untouched on
 * the wire — see protocol.h's comment on struct htxf_conn for the
 * full reasoning. */
extern struct htxf_conn *xfer_new (const char *path, const char *remotedir,
                                   const char *remotename, gsize remotename_len,
                                   guint16 type, int preview,
                                   guint32 srv_data_size);
extern void xfer_up (int num);
extern int xfer_down (int num);
extern int xfer_num (struct htxf_conn *htxf);
extern void xfer_ready_write (struct htxf_conn *htxf);
extern void xfer_tasks_update (struct htlc_conn *htlc);
extern void xfers_delete_all (void);
extern void xfer_delete (struct htxf_conn *htxf);
extern struct htxf_conn *htxf_with_ref (guint32 ref);
extern void hlclient_reap_pid (pid_t pid, int status);
//extern inline int comment_len (const char *path);

#endif
