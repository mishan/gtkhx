/*
 * files_remote_provider.h — Hotline files-server backend for the
 * orthodox files browser.
 *
 * Phase 2: implements HxFilesProvider against the existing wire
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

G_END_DECLS

#endif /* HX_FILES_REMOTE_PROVIDER_H */
