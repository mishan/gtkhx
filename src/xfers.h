#ifndef HX_XFERS_H
#define HX_XFERS_H

/* The xfers[] list + nxfers count moved to the Rust transfer registry
 * (hxhandlers::xfer). Reach the list through xfer_count / xfer_num /
 * htxf_with_ref / … below — the C globals are gone, so no `extern int nxfers`
 * here (a new user would fail at link, not silently bypass the Rust APIs). */

//extern size_t resource_len (const char *path);
extern void xfer_go (struct htxf_conn *htxf);
extern int xfer_go_timer (void *arg);
/* Construct a transfer. The remote location is given as a (dir,
 * name, name_len) triple rather than a single joined path so that
 * names containing `/` (the default dir_char) survive untouched on
 * the wire — see protocol.h's comment on struct htxf_conn for the
 * full reasoning. */
extern struct htxf_conn *xfer_new (const char *path, const char *remotedir,
                                   const char *remotename, gsize remotename_len,
                                   guint16 type, int preview,
                                   guint32 srv_data_size);
/* Folder-transfer variant. Same bookkeeping as xfer_new (enqueue
 * onto xfers[], stamp htlc/path/remote, emit file_update) but
 * skips xfer_go entirely — the caller (hx_get_folder /
 * hx_put_folder) drives the wire request itself because the
 * GETFOLDER / PUTFOLDER opcodes don't share xfer_go's resume +
 * rename heuristics. Sets opt.folder = 1 so the worker picks
 * folder_get_thread / folder_put_thread out of xfer_ready_write's
 * dispatcher. */
extern struct htxf_conn *xfer_new_folder (const char *path,
                                          const char *remotedir,
                                          const char *remotename,
                                          gsize remotename_len, guint16 type);
/* The xfers[] list + reorder/lookup/removal, construction / progress /
 * completion marshaling, and the worker dispatch + params moved to
 * Rust (hxhandlers::xfer). These decls keep the C ABI that tasks.c / rcv.c /
 * gtkhx.c still call in through. */
extern void post_file_update (struct htxf_conn *htxf);
extern void xfer_completion_entry (void *arg);
/* The last-ref GTK/preview + channel teardown (still C); the Rust
 * xfer_init registers it once via hx_htxf_set_destructor. htxf_destructor closes
 * the subchannel through the Rust xfer_close_channel (hxhandlers::xfer). */
extern void htxf_destructor (struct htxf_conn *htxf);
extern void xfer_close_channel (struct htxf_conn *htxf);
extern void xfer_registry_add (struct htxf_conn *htxf);
extern int xfer_count (void);
extern void xfer_remove_from_list (struct htxf_conn *htxf);
extern void xfer_up (int num);
extern int xfer_down (int num);
extern int xfer_num (struct htxf_conn *htxf);
extern void xfer_ready_write (struct htxf_conn *htxf);
extern void xfer_tasks_update (struct htlc_conn *htlc);
extern void xfers_delete_all (void);
extern void xfer_delete (struct htxf_conn *htxf);
extern struct htxf_conn *htxf_with_ref (guint32 ref);
//extern inline int comment_len (const char *path);

#endif
