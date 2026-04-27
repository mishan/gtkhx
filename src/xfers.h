#ifndef HX_XFERS_H
#define HX_XFERS_H

extern int nxfers;
extern struct htxf_conn **xfers;


//extern size_t resource_len (const char *path);
extern void xfer_go (struct htxf_conn *htxf);
extern int xfer_go_timer (void *__arg);
extern struct htxf_conn *xfer_new (const char *path, const char *remotepath, guint16 type);
extern void xfer_up (int num);
extern int xfer_down (int num);
extern int xfer_num (struct htxf_conn *htxf);
extern void xfer_ready_write (struct htxf_conn *htxf);
extern void xfer_tasks_update (struct htlc_conn *htlc);
extern void xfers_delete_all(void);
extern void xfer_delete(struct htxf_conn *htxf);
extern struct htxf_conn *htxf_with_ref(guint32 ref);
//extern inline int comment_len (const char *path);

#endif
