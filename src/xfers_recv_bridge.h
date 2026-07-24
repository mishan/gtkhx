#ifndef GTKHX_XFERS_RECV_BRIDGE_H
#define GTKHX_XFERS_RECV_BRIDGE_H

#include <glib.h>

/* C-side collaborators for the Rust file-transfer receive handlers
 * (hxxfer-recv crate; the rcv_task_file_* / _folder_* / _banner_get /
 * _file_getinfo bodies moved out of rcv.c).
 *
 * struct htxf_conn is refcounted and touched from both the main thread and the
 * per-transfer worker, so it stays C-owned. The Rust handlers own the native
 * parse (hotline_proto::parse::*) and the dispatch decision; these shims own the
 * transfer-specific state mutation — the xfers[] membership check, the
 * retry/delete error policy, the field stamping + preview construction, and the
 * local-filesystem probes. */

struct htlc_conn;
struct htxf_conn;

/* Is htxf still a live entry in the xfers[] array? (Downloads gate their reply
 * on this — a since-cancelled transfer's reply is dropped.) */
extern int hx_xfer_in_list (struct htxf_conn *htxf);

/* Task-error reply for a download (file_get / folder_get): re-arm the 1 s retry
 * timer when htxf->opt.retry is set, else drop the transfer. */
extern void hx_xfer_get_error (struct htlc_conn *htlc, struct htxf_conn *htxf);

/* Task-error reply for an upload (file_put / folder_put): the server rejected
 * the put outright, so drop the transfer. */
extern void hx_xfer_put_error (struct htlc_conn *htlc, struct htxf_conn *htxf);

/* Apply a successful FILE_GET reply: stamp ref / total_size / queue + the HTXF
 * subchannel target, build the preview window when opt.preview is set, and run
 * the shared hx_xfer_announce tail. */
extern void hx_xfer_file_get_apply (struct htlc_conn *htlc,
                                     struct htxf_conn *htxf, guint32 ref,
                                     guint64 total_size, guint32 queue);

/* Apply a successful FOLDER_GET reply: like hx_xfer_file_get_apply but no
 * preview; total_size is already normalised (0 -> 1) by the caller. */
extern void hx_xfer_folder_get_apply (struct htlc_conn *htlc,
                                       struct htxf_conn *htxf, guint32 ref,
                                       guint64 total_size, guint32 queue);

/* Apply a successful FILE_PUT reply: stamp resume offsets / queue / ref, probe
 * the local file (stat / resource_len / comment_len) to size the upload, and run
 * the announce tail. */
extern void hx_xfer_file_put_apply (struct htlc_conn *htlc,
                                     struct htxf_conn *htxf, guint32 ref,
                                     guint32 queue, guint32 data_pos,
                                     guint32 rsrc_pos);

/* Apply a successful FOLDER_PUT reply: stamp ref / queue + subchannel target and
 * run the announce tail (per-file resume happens inside the worker). */
extern void hx_xfer_folder_put_apply (struct htlc_conn *htlc,
                                       struct htxf_conn *htxf, guint32 ref,
                                       guint32 queue);

/* Format the two Hotline date stamps and fire the file-info signal
 * (rcv_task_file_getinfo). Strings arrive as (ptr, len) slices (not
 * NUL-terminated); the dates are 8 raw wire bytes each. */
extern void hx_xfer_file_info_apply (const char *path, const guint8 *name,
                                     gsize name_len, const guint8 *type,
                                     gsize type_len, const guint8 *creator,
                                     gsize creator_len, const guint8 *comment,
                                     gsize comment_len,
                                     const guint8 *date_create,
                                     const guint8 *date_modify, guint64 size);

#endif /* GTKHX_XFERS_RECV_BRIDGE_H */
