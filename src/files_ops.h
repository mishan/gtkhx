/*
 * files_ops.h — cross-panel transfer orchestration for the
 * orthodox files browser.
 *
 * Phase 3: a single `Copy` action moves the active panel's
 * selection to the inactive panel's current path. The four
 * cases:
 *
 *   local  → remote   upload via hx_put_file (XFER_PUT)
 *   remote → local    download via xfer_new (XFER_GET)
 *   local  → local    GIO copy
 *   remote → remote   not implemented in Phase 3 — toasts
 *                     "not supported yet" (would need a
 *                     server-side HTLC_HDR_FILECOPY plus
 *                     gracefully falling back to download-
 *                     then-upload otherwise)
 *
 * The operation works on a single HxFileEntry — multi-select
 * isn't wired through yet (the panels still use GtkSingleSelection).
 * Phase 4 will switch to multi-select and iterate.
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
    HX_OPS_ERR_UNSUPPORTED,        /* remote→remote in Phase 3 */
    HX_OPS_ERR_FOLDER_UNSUPPORTED, /* recursive copy: deferred */
    HX_OPS_ERR_LOCAL_FAIL          /* GIO copy failed */
} HxOpsResult;

extern HxOpsResult hx_files_ops_copy (HxFilesProvider *src,
                                      HxFilesProvider *dst, HxFileEntry *entry);

/* Translates an HxOpsResult into a user-readable, already-
 * localised message suitable for an AdwToast or a g_warning. */
extern const char *hx_files_ops_result_message (HxOpsResult r);

G_END_DECLS

#endif /* HX_FILES_OPS_H */
