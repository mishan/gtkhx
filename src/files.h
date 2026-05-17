#ifndef HX_FILES_H
#define HX_FILES_H

/* Phase 5: legacy single-pane GtkHList file browser was retired
 * with the new orthodox-file-manager browser in files_browser.c /
 * files_panel.c / files_{local,remote}_provider.c / files_ops.c.
 * files.c is now just wire helpers + a couple of utilities still
 * used by the new browser (icon picker, kind formatter, hldir
 * encoder, the file-info dialog).
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

extern char *human_size (char *sizstr, guint32 size);

/* File-info dialog. Called from gtkhx.c::on_file_info_signal when
 * a HTLS_HDR_FILE_GETINFO reply arrives. The new files browser's
 * Get Info button fires the wire request via hx_file_info; this
 * is the receiving end that builds the dialog. */
extern void output_file_info (char *path, char *name, char *creator, char *type,
                              char *comments, char *modified, char *created,
                              guint32 size);

/* Emit the file-list GtkhxSession signal so the file_list-signal
 * handler in gtkhx.c (and through it the remote-provider in the
 * new files browser) can pick up a parsed cfl. Called from
 * rcv.c::rcv_task_file_list. */
extern void cfl_print (struct cached_filelist *cfl, void *data);
extern struct cached_filelist *cfl_lookup (const char *path);

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

extern guint8 *path_to_hldir (const char *path, guint16 *hldirlen, int is_file);
/* dirchar_basename is a thin wrapper around path_basename that uses
 * the dir_char global. The unit-testable underlying function lives
 * in path_util.h. */
#include "path_util.h"
extern char *dirchar_basename (char *path);
extern void dirchar_fix (char *lpath);
extern void dirmask (char *dst, char *src, char *mask);
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
extern void hx_file_link (struct htlc_conn *htlc, char *src_path,
                          char *dst_path);
extern void hx_file_move (struct htlc_conn *htlc, char *src_path,
                          char *dst_path);

#endif
