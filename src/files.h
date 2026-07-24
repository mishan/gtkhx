#ifndef HX_FILES_H
#define HX_FILES_H

/* The orthodox file-manager browser lives in files_browser.c /
 * files_panel.c / files_{local,remote}_provider.c / files_ops.c.
 * files.c is wire helpers + a few utilities the browser uses
 * (icon picker, kind formatter, hldir encoder, the file-info
 * dialog).
 *
 * Forward decls so consumers that don't pull in the protocol
 * headers (e.g. files_entry.c, which only needs the ICON_*
 * constants) still compile. */
struct htlc_conn;
struct cached_filelist;
struct hl_filelist_hdr;

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
 * tasks.c / files.c / progress labels all pick them up from one
 * place. Re-included here so historical includers of files.h don't
 * have to chase a second header. */
#include "human_readable.h"

/* File-info dialog. Called from gtkhx.c::on_file_info_signal when
 * a HTLS_HDR_FILE_GETINFO reply arrives. The new files browser's
 * Get Info button fires the wire request via hx_file_info; this
 * is the receiving end that builds the dialog. */
/* date_modify / date_create are the raw 8-byte Hotline date stamps from the
 * FILE_GETINFO reply; output_file_info decodes + locale-formats them for
 * display (the model side emits them raw). */
extern void output_file_info (char *path, char *name, char *creator, char *type,
                              char *comments, const guint8 *date_modify,
                              const guint8 *date_create, guint64 size);

/* Rust-owned struct cached_filelist (hxfiles-recv crate) — the opaque handle +
 * accessor facade. Allocate with hx_cfl_new, reach the fields through these, free
 * with hx_cfl_free. The FILE_LIST reply's fh accumulation + the file-list emit
 * live in the Rust rcv_task_file_list; C touches cfl only to start a listing
 * (files_remote_provider.c) and to drive the recursive engine below. */
extern struct cached_filelist *hx_cfl_new (void);
extern void         hx_cfl_free (struct cached_filelist *cfl);
extern const char  *hx_cfl_path (const struct cached_filelist *cfl);
extern void         hx_cfl_set_path (struct cached_filelist *cfl, const char *path);
extern const void  *hx_cfl_fh (const struct cached_filelist *cfl);
extern guint32      hx_cfl_fhlen (const struct cached_filelist *cfl);
extern guint        hx_cfl_completing (const struct cached_filelist *cfl);
extern void         hx_cfl_set_completing (struct cached_filelist *cfl, guint completing);
extern void        *hx_cfl_filter_argv (const struct cached_filelist *cfl);
extern void         hx_cfl_set_filter_argv (struct cached_filelist *cfl, void *argv);

/* Recursive folder-listing / GET_R engine for one FILE_LIST entry, invoked by
 * the Rust rcv_task_file_list when cfl is in a recursive mode (completing > 1).
 * The Rust handler decides folder-vs-file (via hotline_proto's FTYPE_FLDR) and
 * passes `is_folder`. A folder re-issues FILE_LIST for the subfolder; a leaf in
 * COMPLETE_GET_R mode mkdir's the local tree and xfer_new's the download. Reads
 * the Rust-owned cfl through the accessors above. */
extern void hx_cfl_complete_entry (struct htlc_conn *htlc,
                                   struct cached_filelist *cfl, int is_folder,
                                   const guint8 *fname, gsize fnlen,
                                   guint32 fsize);

/* Pick the cicn icon id for a Hotline file-list entry. `ftype` is
 * the raw 4-byte FourCC from the wire (NOT byte-swapped); `name`
 * + `name_len` are the entry's filename (UTF-8 OK, but the
 * drop-box heuristic only checks ASCII subsequences). Returns
 * one of the ICON_* constants above. */
extern guint16 icon_of_ftype_and_name (const char *ftype, const char *name,
                                       gsize name_len);
extern guint16 icon_of_fh (struct hl_filelist_hdr *fh);

/* Human-readable type label for a Hotline file-list entry's
 * 4-byte FourCC. Returns a static localized string for known
 * codes ("Text Document", "JPEG Image", "MP3 Audio", etc.) or
 * the raw FourCC as a non-static copy otherwise. The boolean
 * out-parameter `is_static` tells the caller whether the
 * returned pointer is owned (must g_free) or borrowed.
 *
 * Folder type "fldr" returns _("Folder") as static. */
extern const char *kind_of_ftype (const char *ftype, gboolean *is_static);

extern guint8 dir_char;

/* path_to_hldir + dirmask now live in src/path_hldir.c, but the
 * extern declarations stay here so callers don't have to chase a
 * second header. */
#include "path_hldir.h"
/* dirchar_basename is a thin wrapper around path_basename that uses
 * the dir_char global. The unit-testable underlying function lives
 * in path_util.h. */
#include "path_util.h"
extern char *dirchar_basename (char *path);
extern void dirchar_fix (char *lpath);
extern int exists_remote (char *path);

extern void hx_file_delete (struct htlc_conn *htlc, char *path);
extern void hx_make_dir (struct htlc_conn *htlc, char *path);
/* Request info on the file located at (dir_path, file_name).
 * Keeping the directory and filename separate on the API surface —
 * rather than a single joined `dir/name` string — is what lets
 * names containing `/` (which is otherwise dir_char) survive intact
 * on the wire FILE_NAME chunk. Otherwise the embedded slash gets
 * reinterpreted as a directory boundary on the round-trip through
 * path_to_hldir. */
extern void hx_file_info (struct htlc_conn *htlc, const char *dir_path,
                          const char *file_name, gsize file_name_len);
extern void hx_put_file (struct htlc_conn *htlc, char *lpath, char *rpath);
/* Download a remote folder tree to lpath_root. The server replies
 * with HTLS_DATA_HTXF_SIZE / HTLS_DATA_HTXF_REF and the worker
 * spun up via xfer_ready_write drives the FILE_NEXT/FILE_SEND
 * state machine in folder_get_thread. lpath_root is the *parent*
 * directory locally; the folder named `name` will be created
 * under it as the local root for the tree. */
extern void hx_get_folder (struct htlc_conn *htlc, const char *lpath_root,
                           const char *rdir, const char *name, gsize name_len);
/* Upload a local folder tree to the server. lpath is the local
 * source folder; rdir is the remote parent directory; name is
 * the folder's basename as it should appear remotely. The server
 * creates the destination folder root and we stream the contents
 * over the HTXF subchannel via folder_put_thread. */
extern void hx_put_folder (struct htlc_conn *htlc, const char *lpath,
                           const char *rdir, const char *name, gsize name_len);
extern void hx_file_link (struct htlc_conn *htlc, char *src_path,
                          char *dst_path);
extern void hx_file_move (struct htlc_conn *htlc, char *src_path,
                          char *dst_path);

#endif
