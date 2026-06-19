/*
 * files_local_provider.h — local filesystem backend for the
 * orthodox files browser.
 *
 * read-only listing + mkdir / delete / rename. The
 * provider owns a `current_path` and a `listing` GListModel that
 * the panel widget consumes directly via GtkColumnView's
 * GtkSelectionModel chain.
 *
 * Phase 2 will introduce HxRemoteFilesProvider with the same
 * surface (probably extracted into a real GInterface at that
 * point — for now duck-typing across the two providers is enough
 * since the panel widget only knows about HxLocalFilesProvider).
 */

#ifndef HX_FILES_LOCAL_PROVIDER_H
#define HX_FILES_LOCAL_PROVIDER_H 1

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define HX_TYPE_LOCAL_FILES_PROVIDER (hx_local_files_provider_get_type ())
G_DECLARE_FINAL_TYPE (HxLocalFilesProvider, hx_local_files_provider, HX,
                      LOCAL_FILES_PROVIDER, GObject)

/* Construct a provider rooted at `initial_path`. Caller can pass
 * NULL to default to $XDG_DOWNLOAD_DIR (and falls back further
 * to $HOME if XDG isn't set). */
extern HxLocalFilesProvider *
hx_local_files_provider_new (const char *initial_path);

/* Read accessors. The returned listing is a live GListModel —
 * connect to its items-changed if you need to react to entry
 * updates; the provider emits "navigated" on each whole-listing
 * change. */
extern GListModel *
hx_local_files_provider_get_listing (HxLocalFilesProvider *self);
extern const char *
hx_local_files_provider_get_current_path (HxLocalFilesProvider *self);
extern const char *
hx_local_files_provider_get_label (HxLocalFilesProvider *self);

/* Navigate to a new directory. Absolute path. Async-by-name (no
 * actual async — local FS reads are blocking-but-fast); on
 * completion the "navigated" signal fires with the new path. On
 * error the "error" signal fires with a human-readable message
 * and the provider's current path stays put. */
extern void hx_local_files_provider_navigate (HxLocalFilesProvider *self,
                                              const char *path);

/* Re-read the current path. Used after a mutation. */
extern void hx_local_files_provider_reload (HxLocalFilesProvider *self);

/* Walk one directory up. No-op at the FS root. */
extern void hx_local_files_provider_navigate_up (HxLocalFilesProvider *self);

/* Mutations. `name` is the entry's display name; the provider
 * joins it onto the current path. */
extern gboolean hx_local_files_provider_mkdir (HxLocalFilesProvider *self,
                                               const char *name, GError **err);
extern gboolean hx_local_files_provider_delete (HxLocalFilesProvider *self,
                                                const char *name, GError **err);
extern gboolean hx_local_files_provider_rename (HxLocalFilesProvider *self,
                                                const char *old_name,
                                                const char *new_name,
                                                GError **err);

G_END_DECLS

#endif /* HX_FILES_LOCAL_PROVIDER_H */
