#ifndef HX_FILES_H
#define HX_FILES_H

/* The orthodox file-manager browser lives in files_browser.c /
 * files_panel.c / files_{local,remote}_provider.c / files_ops.c. The wire
 * senders it drives and the file-info dialog are Rust
 * (hxhandlers::send::files, gtkhx-ui/src/file_info.rs); this header is
 * their C ABI plus the icon ids the files UI shares.
 *
 * Forward decls so consumers that don't pull in the protocol
 * headers still compile. */
struct htlc_conn;
struct cached_filelist;

/* Mac-classic cicn icon numbers used across the files UI. The
 * numeric values are the cicn resource IDs inside the bundled
 * .rsrc files load_icon walks. */
#define ICON_FILE 400
#define ICON_FOLDER 401
#define ICON_FOLDER_IN 421
#define ICON_FILE_HTft 402
#define ICON_FILE_SIT 403
#define ICON_FILE_TEXT 404
#define ICON_FILE_IMAGE 406
#define ICON_FILE_APPL 407
#define ICON_FILE_HTLC 408
#define ICON_FILE_SITP 409
#define ICON_FILE_alis 422
#define ICON_FILE_DISK 423
#define ICON_FILE_NOTE 424
#define ICON_FILE_MOOV 425
#define ICON_FILE_ZIP 426

/* human_size + LONGEST_HUMAN_READABLE moved to human_readable.h so
 * tasks.c / the files browser / progress labels all pick them up from one
 * place. Re-included here so historical includers of files.h don't
 * have to chase a second header. */
#include "human_readable.h"

/* File-info dialog. Called from gtkhx.c::on_file_info_signal when
 * a HTLS_HDR_FILE_GETINFO reply arrives. The new files browser's
 * Get Info button fires the wire request via hx_file_info; this
 * is the receiving end that builds the dialog. Implemented in Rust now
 * (gtkhx-ui/src/file_info.rs); this decl keeps the C ABI the on_file_info_signal
 * adapter links against. date_modify / date_create are the raw 8-byte Hotline
 * date stamps from the FILE_GETINFO reply (the model emits them raw); the dialog
 * decodes + locale-formats them for display. */
extern void output_file_info (char *path, char *name, char *creator, char *type,
                              char *comments, const guint8 *date_modify,
                              const guint8 *date_create, guint64 size);

/* A listing, as the file-list signal carries it (hxhandlers::recv::files):
 * the folder it lists, and its entries into a GListStore of HxFileEntry. */
extern const char *hx_cfl_path (const struct cached_filelist *cfl);
extern void hx_cfl_populate (const struct cached_filelist *cfl,
                             GListStore *store);

/* FILE_LIST for `path`; the reply comes back on the file-list signal with
 * `provider` as its data. */
extern void hx_list_dir (struct htlc_conn *htlc, const char *path,
                         gpointer provider);

/* path_to_hldir, re-exported so files callers don't have to chase a
 * second header. */
#include "path_hldir.h"

extern void hx_file_delete (struct htlc_conn *htlc, char *path);
extern void hx_make_dir (struct htlc_conn *htlc, char *path);
/* Request info on the file located at (dir_path, file_name).
 * Keeping the directory and filename separate on the API surface —
 * rather than a single joined `dir/name` string — is what lets
 * names containing `/` (which is otherwise the separator) survive intact
 * on the wire FILE_NAME chunk. Otherwise the embedded slash gets
 * reinterpreted as a directory boundary on the round-trip through
 * path_to_hldir. */
extern void hx_file_info (struct htlc_conn *htlc, const char *dir_path,
                          const char *file_name, gsize file_name_len);
/* Uploads land in the remote folder rdir under the local file's or
 * folder's own name. */
extern void hx_put_file (struct htlc_conn *htlc, const char *lpath,
                         const char *rdir);
/* Download the remote folder `name` in rdir into the local folder lpath.
 * The worker spun up via xfer_ready_write drives the FILE_NEXT/FILE_SEND
 * state machine in folder_get_thread. */
extern void hx_get_folder (struct htlc_conn *htlc, const char *lpath,
                           const char *rdir, const char *name, gsize name_len);
extern void hx_put_folder (struct htlc_conn *htlc, const char *lpath,
                           const char *rdir);
extern void hx_file_move (struct htlc_conn *htlc, char *src_path,
                          char *dst_path);

#endif
