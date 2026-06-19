/*
 * files_remote_provider.h — Hotline files-server backend for the
 * orthodox files browser.
 *
 * implements HxFilesProvider against the existing wire
 * helpers in files.c (hx_list_dir, hx_make_dir, hx_file_delete,
 * hx_file_move). Listings come in via the GtkhxSession::file-list
 * signal; the gtkhx.c handler routes replies matching our pending
 * fetches to this module before falling through to the legacy
 * single-pane UI.
 */

#ifndef HX_FILES_REMOTE_PROVIDER_H
#define HX_FILES_REMOTE_PROVIDER_H 1

#include <glib-object.h>
#include "files_provider.h"

G_BEGIN_DECLS

#define HX_TYPE_REMOTE_FILES_PROVIDER (hx_remote_files_provider_get_type ())
G_DECLARE_FINAL_TYPE (HxRemoteFilesProvider, hx_remote_files_provider, HX,
                      REMOTE_FILES_PROVIDER, GObject)

extern HxRemoteFilesProvider *hx_remote_files_provider_new (void);

/* Reply-routing hook. Called from gtkhx.c::on_file_list_signal
 * before the legacy output_file_list path. Returns TRUE if the
 * `data` carrier matches an in-flight remote-provider request —
 * the provider has parsed the chunks into HxFileEntry rows and
 * emitted "navigated". FALSE leaves it for the legacy handler. */
extern gboolean hx_remote_files_provider_handle_file_list (gpointer cfl,
                                                           gpointer fh,
                                                           gpointer data);

/* Error counterpart. Called from rcv.c::rcv_task_file_list's
 * task_inerror short-circuit so the provider can clear its
 * listing, flip listing_error TRUE, and emit "navigated" — the
 * panel then picks up the new state via its existing handler and
 * updates the empty-state messaging. Returns TRUE when claimed,
 * FALSE when `data` isn't one of ours. */
extern gboolean hx_remote_files_provider_handle_file_list_error (gpointer cfl,
                                                                 gpointer data);

/* TRUE iff the provider's most recent FILE_LIST request came back
 * as a task error. Panel queries this when rendering the empty-
 * state hint in the status footer. */
extern gboolean
hx_remote_files_provider_has_listing_error (HxRemoteFilesProvider *self);

/* Drop every row and reset the listing-error flag. Called from
 * the connection-state hook on DISCONNECTED so the remote panel
 * doesn't show stale content while the user is logged out — the
 * panel's update_status path picks up the empty listing + the
 * provider's "not connected" unavailable-reason and paints the
 * disconnected hint. Re-emits "navigated" so the panel's status
 * footer updates. */
extern void
hx_remote_files_provider_clear_listing (HxRemoteFilesProvider *self);

G_END_DECLS

#endif /* HX_FILES_REMOTE_PROVIDER_H */
