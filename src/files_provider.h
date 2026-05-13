/*
 * files_provider.h — abstract backend interface for the files browser.
 *
 * Phase 2 of the orthodox-files-browser plan introduces a remote
 * (Hotline) backend alongside the local-filesystem one. The panel
 * widget talks to providers through this GInterface, so the two
 * sides differ only in the implementation underneath.
 *
 * Contract:
 *
 *   - Each provider owns a `current_path` (string) and a `listing`
 *     (GListModel of HxFileEntry). The panel widget reads both
 *     directly; do not duplicate them on the panel side.
 *   - `navigate` is async-by-name. Local provider implementations
 *     can satisfy synchronously; remote ones fire an RPC and
 *     complete on reply. Either way, the "navigated" signal fires
 *     after the listing has been refreshed with the new contents.
 *   - "error" fires when an operation fails; the payload is a
 *     human-readable, already-localised string suitable for an
 *     AdwToast.
 *   - mkdir / delete / rename return TRUE if the request was
 *     successfully *issued* (synchronous local FS: this is also
 *     the result; async remote: any subsequent server-side
 *     failure surfaces via the "error" signal).
 */

#ifndef HX_FILES_PROVIDER_H
#define HX_FILES_PROVIDER_H 1

#include <gio/gio.h>
#include <glib-object.h>

#include "files_entry.h"

G_BEGIN_DECLS

#define HX_TYPE_FILES_PROVIDER (hx_files_provider_get_type ())
G_DECLARE_INTERFACE (HxFilesProvider, hx_files_provider,
                     HX, FILES_PROVIDER, GObject)

struct _HxFilesProviderInterface {
	GTypeInterface parent;

	/* Read accessors. None of these transfer ownership; all
	 * three return pointers to internal state. */
	GListModel *(*get_listing)      (HxFilesProvider *self);
	const char *(*get_current_path) (HxFilesProvider *self);
	const char *(*get_label)        (HxFilesProvider *self);

	/* Navigation. `navigate` is async; `reload` re-fires whatever
	 * the current path is; `navigate_up` walks to the parent
	 * directory (no-op at the FS / Hotline root). */
	void (*navigate)    (HxFilesProvider *self, const char *path);
	void (*reload)      (HxFilesProvider *self);
	void (*navigate_up) (HxFilesProvider *self);

	/* Mutations. `name` is the leaf entry name; the provider
	 * joins it onto current_path. `err` is filled on synchronous
	 * failure paths; asynchronous failures fire "error" instead. */
	gboolean (*mkdir)        (HxFilesProvider *self,
	                           const char *name, GError **err);
	gboolean (*delete_entry) (HxFilesProvider *self,
	                           const char *name, GError **err);
	gboolean (*rename)       (HxFilesProvider *self,
	                           const char *old_name,
	                           const char *new_name, GError **err);

	/* Optional capability check. NULL == ready. A non-NULL
	 * return is a localized message explaining why the panel
	 * can't list right now (e.g., "Not connected" for the
	 * remote provider before login). The panel paints this in
	 * place of the file list. */
	const char *(*get_unavailable_reason) (HxFilesProvider *self);

	/* Activate a single non-directory entry — fires the
	 * provider-appropriate default action. Local: launch
	 * with the desktop's default app via xdg-open. Remote:
	 * download into the preview pipeline. Optional; NULL
	 * means "no action on activate". */
	void (*activate_entry) (HxFilesProvider *self, HxFileEntry *e);
};

/* Thin wrappers that dispatch through the vtable. Use these from
 * the panel and browser code; never call iface->* directly. */
extern GListModel *hx_files_provider_get_listing      (HxFilesProvider *self);
extern const char *hx_files_provider_get_current_path (HxFilesProvider *self);
extern const char *hx_files_provider_get_label        (HxFilesProvider *self);
extern void        hx_files_provider_navigate         (HxFilesProvider *self,
                                                       const char *path);
extern void        hx_files_provider_reload           (HxFilesProvider *self);
extern void        hx_files_provider_navigate_up      (HxFilesProvider *self);
extern gboolean    hx_files_provider_mkdir            (HxFilesProvider *self,
                                                       const char *name,
                                                       GError    **err);
extern gboolean    hx_files_provider_delete           (HxFilesProvider *self,
                                                       const char *name,
                                                       GError    **err);
extern gboolean    hx_files_provider_rename           (HxFilesProvider *self,
                                                       const char *old_name,
                                                       const char *new_name,
                                                       GError    **err);
extern const char *hx_files_provider_get_unavailable_reason (HxFilesProvider *self);

/* Default action on a non-directory entry. Provider decides
 * what that means — local launches xdg-open, remote streams
 * into the preview window. No-op if the provider didn't
 * override activate_entry. */
extern void hx_files_provider_activate_entry (HxFilesProvider *self,
                                              HxFileEntry     *e);

G_END_DECLS

#endif /* HX_FILES_PROVIDER_H */
