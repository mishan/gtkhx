#ifndef HX_XFERS_H
#define HX_XFERS_H

/* The file-transfer worker shell is now entirely Rust (hxhandlers::xfer): the
 * xfers[] registry, xfer_go's wire-request build, construction + progress /
 * completion marshaling, the worker dispatch, and the last-ref teardown. Only
 * the struct htxf_conn storage + refcount/cancel lifecycle live in
 * hxnet::xfer_handle (reached through the htxf_accessors.c seam).
 *
 * This header is the remaining C ABI surface — the transfer entry points that
 * C code (tasks.c / files*.c / gtkhx.c) still calls in through; each resolves to
 * a Rust #[no_mangle] export. The Rust-internal entry points (xfer_ready_write,
 * xfer_go_timer, the registry ops, the teardown) are declared Rust-side by their
 * callers and don't appear here. */

extern void xfer_go (struct htxf_conn *htxf);
/* Construct a transfer on `htlc`. The remote location is given as a (dir,
 * name, name_len) triple rather than a single joined path so that
 * names containing `/` (the default dir_char) survive untouched on
 * the wire — see protocol.h's comment on struct htxf_conn for the
 * full reasoning.
 *
 * `htlc` is the connection the transfer runs on, and the caller picks it. It
 * used to be read from the focused connection inside the constructor, which
 * meant a download started from a background server's pane went to the
 * foreground one — invisible while there was only ever one. */
extern struct htxf_conn *xfer_new (struct htlc_conn *htlc, const char *path,
                                   const char *remotedir,
                                   const char *remotename, gsize remotename_len,
                                   guint16 type, int preview,
                                   guint32 srv_data_size);
/* Folder-transfer variant. Same bookkeeping as xfer_new (enqueue,
 * stamp htlc/path/remote, emit file_update) but skips xfer_go
 * entirely — the caller (hx_get_folder / hx_put_folder) drives the
 * GETFOLDER / PUTFOLDER wire request itself, since those opcodes
 * don't share xfer_go's resume + rename heuristics. Sets opt.folder
 * so the worker picks the folder thread.
 *
 * Pass the same `htlc` the caller sends GETFOLDER / PUTFOLDER on: the request
 * and the transfer it opens have to be the same connection. */
extern struct htxf_conn *xfer_new_folder (struct htlc_conn *htlc,
                                          const char *path,
                                          const char *remotedir,
                                          const char *remotename,
                                          gsize remotename_len, guint16 type);
/* Tasks-window reorder + lookup + broadcast + cancel. */
extern void xfer_up (int num);
extern int xfer_down (int num);
extern int xfer_num (struct htxf_conn *htxf);
extern void xfer_tasks_update (struct htlc_conn *htlc);
extern void xfers_delete_all (void);
extern void xfer_delete (struct htxf_conn *htxf);
extern struct htxf_conn *htxf_with_ref (guint32 ref);

#endif
