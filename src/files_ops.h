/*
 * files_ops.h — cross-panel transfer orchestration for the
 * orthodox files browser.
 *
 * A single `Copy` action transfers the active panel's selection
 * to the inactive panel's current path:
 *
 *   local  → remote   upload via hx_put_file (XFER_PUT). Folders
 *                     are walked locally with GFileEnumerator;
 *                     each subdir gets an hx_make_dir and each
 *                     file an hx_put_file. Gated on
 *                     HL_ACCESS_UPLOAD_FILES for files plus
 *                     HL_ACCESS_UPLOAD_FOLDERS for folder roots.
 *   remote → local    download via xfer_new (XFER_GET). Folder
 *                     downloads aren't wired yet — the legacy
 *                     rcv.c COMPLETE_GET_R path mkdirs relative
 *                     to cwd, which puts files in the wrong place
 *                     under Flatpak; a follow-up will rewire it
 *                     to honour download_path.
 *   local  → local    GIO. Folders are walked with
 *                     GFileEnumerator and g_file_copy'd file by
 *                     file.
 *   remote → remote   not supported — Hotline has no FILE_COPY
 *                     opcode, and the SYMLINK alternative shares
 *                     bytes on disk so it isn't a real copy. The
 *                     orthodox-FM convention of "drag-within-
 *                     same-volume = move" gives the user the
 *                     expected behaviour: files_browser.c's
 *                     on_drop routes remote→remote DnD to
 *                     hx_file_move directly.
 */

#ifndef HX_FILES_OPS_H
#define HX_FILES_OPS_H 1

#include <glib.h>

#include "files_entry.h"
#include "files_provider.h"

G_BEGIN_DECLS

/* Outcome of the copy request. The actual transfer is async
 * (Hotline) or quick-but-blocking (local) — this only reports
 * whether we managed to *issue* it. */
typedef enum {
    HX_OPS_OK = 0,
    HX_OPS_ERR_NO_SOURCE,          /* src entry NULL / unreadable */
    HX_OPS_ERR_NO_TARGET,          /* dst provider NULL / no path */
    HX_OPS_ERR_NOT_CONNECTED,      /* remote provider without
	                                 * a live connection */
    HX_OPS_ERR_NO_PERMISSION,      /* access bit not set */
    HX_OPS_ERR_UNSUPPORTED,        /* dispatch fallthrough */
    HX_OPS_ERR_FOLDER_UNSUPPORTED, /* remote→local recursion: deferred */
    HX_OPS_ERR_LOCAL_FAIL          /* GIO copy failed */
} HxOpsResult;

extern HxOpsResult hx_files_ops_copy (HxFilesProvider *src,
                                      HxFilesProvider *dst, HxFileEntry *entry);

/* Translates an HxOpsResult into a user-readable, already-
 * localised message suitable for an AdwToast or a g_warning. */
extern const char *hx_files_ops_result_message (HxOpsResult r);

G_END_DECLS

#endif /* HX_FILES_OPS_H */
