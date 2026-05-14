#ifndef HX_FILES_H
#define HX_FILES_H

extern void destroy_gfl_list (void);
extern void open_files (void);
extern char *human_size (char *sizstr, guint32 size);
extern void output_file_list (struct cached_filelist *cfl,
                              struct hl_filelist_hdr *fh, void *data);
extern void output_file_info (char *path, char *name, char *creator, char *type,
                              char *comments, char *modified, char *created,
                              guint32 size);
extern void cfl_print (struct cached_filelist *cfl, void *data);
extern struct cached_filelist *cfl_lookup (const char *path);

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

extern void hx_list_dir (struct htlc_conn *htlc, const char *path, int reload,
                         int recurs, void *data);
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
extern void hx_file_link (struct htlc_conn *htlc, char *src_path,
                          char *dst_path);
extern void hx_file_move (struct htlc_conn *htlc, char *src_path,
                          char *dst_path);

/* Phase 5+ (GLib-collections): no more next/prev. Open file-browser
 * windows live in a GList<struct gfile_list*> on `gfile_list`.
 * (GList rather than GHashTable because the legacy file_samewin=false
 * path allows multiple windows to share the same remote path, which
 * a path-keyed hashtable can't represent. N stays small — handful of
 * open browsers — so linear lookups are fine.) */
struct gfile_list {
    struct cached_filelist *cfl;
    struct path_hist *path_list;
    int row, column;
    GtkWidget *hlist, *window, *up_btn;
    char in_use;
};

extern GList *gfile_list;

#endif
