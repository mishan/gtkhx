/*
 * files_provider.h — abstract backend interface for the files browser.
 *
 * The panel widget talks to backends — a local GIO-based one and
 * the remote Hotline one — through this GInterface, so the two sides
 * differ only in the implementation underneath.
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
G_DECLARE_INTERFACE (HxFilesProvider, hx_files_provider, HX, FILES_PROVIDER,
                     GObject)

struct _HxFilesProviderInterface {
    GTypeInterface parent;

    /* Read accessors. None of these transfer ownership; all
     * three return pointers to internal state. */
    GListModel *(*get_listing) (HxFilesProvider *self);
    const char *(*get_current_path) (HxFilesProvider *self);
    const char *(*get_label) (HxFilesProvider *self);

    /* Navigation. `navigate` is async; `reload` re-fires whatever
     * the current path is; `navigate_up` walks to the parent
     * directory (no-op at the FS / Hotline root). */
    void (*navigate) (HxFilesProvider *self, const char *path);
    void (*reload) (HxFilesProvider *self);
    void (*navigate_up) (HxFilesProvider *self);

    /* Mutations. `name` is the leaf entry name; the provider
     * joins it onto current_path. `err` is filled on synchronous
     * failure paths; asynchronous failures fire "error" instead. */
    gboolean (*mkdir) (HxFilesProvider *self, const char *name, GError **err);
    gboolean (*delete_entry) (HxFilesProvider *self, const char *name,
                              GError **err);
    gboolean (*rename) (HxFilesProvider *self, const char *old_name,
                        const char *new_name, GError **err);

    /* Optional capability check. NULL == ready. A non-NULL
     * return is a localized message explaining why the panel
     * can't list right now (e.g., "Not connected" for the
     * remote provider before login). The panel paints this in
     * place of the file list. */
    const char *(*get_unavailable_reason) (HxFilesProvider *self);

    /* Activate a single non-directory entry — fires the
     * provider-appropriate default action for "the user pressed
     * Enter (or double-clicked) on this row". Local: launch
     * with the desktop's default app via xdg-open. Remote:
     * download to the configured download folder (the closest
     * analogue to xdg-open's "do the obvious thing" semantic
     * for a remote file). Optional; NULL means "no action on
     * activate".
     *
     * Preview is a separate action — see preview_entry below.
     * Misha asked for Enter-on-remote to download rather than
     * preview because download is the action users almost
     * always want on a remote row, and preview stays one F-key
     * (F3) or one button click away. */
    void (*activate_entry) (HxFilesProvider *self, HxFileEntry *e);

    /* Explicit preview action — fired by the headerbar Preview
     * button and F3/Ctrl+P. Local: same as activate_entry
     * (xdg-open); the OS default app for the type IS the
     * preview. Remote: stream the file into the in-app preview
     * window (the old activate_entry body). Optional; default
     * fallback is to call activate_entry, which gives the
     * "preview means open by default" behaviour for any
     * provider that doesn't override it. */
    void (*preview_entry) (HxFilesProvider *self, HxFileEntry *e);
};

/* Thin wrappers that dispatch through the vtable. Use these from
 * the panel and browser code; never call iface->* directly. */
extern GListModel *hx_files_provider_get_listing (HxFilesProvider *self);
extern const char *hx_files_provider_get_current_path (HxFilesProvider *self);
extern const char *hx_files_provider_get_label (HxFilesProvider *self);
extern void hx_files_provider_navigate (HxFilesProvider *self,
                                        const char *path);
extern void hx_files_provider_reload (HxFilesProvider *self);
extern void hx_files_provider_navigate_up (HxFilesProvider *self);
extern gboolean hx_files_provider_mkdir (HxFilesProvider *self,
                                         const char *name, GError **err);
extern gboolean hx_files_provider_delete (HxFilesProvider *self,
                                          const char *name, GError **err);
extern gboolean hx_files_provider_rename (HxFilesProvider *self,
                                          const char *old_name,
                                          const char *new_name, GError **err);
extern const char *
hx_files_provider_get_unavailable_reason (HxFilesProvider *self);

/* Default row-activate action (Enter / double-click). Local
 * launches xdg-open; remote queues a download. No-op if the
 * provider didn't override activate_entry. */
extern void hx_files_provider_activate_entry (HxFilesProvider *self,
                                              HxFileEntry *e);

/* Explicit preview action (F3 / Ctrl+P / Preview button).
 * Local providers that don't override fall back to
 * activate_entry — for local files preview-via-xdg-open is the
 * right answer. Remote providers stream into the in-app preview
 * window. */
extern void hx_files_provider_preview_entry (HxFilesProvider *self,
                                             HxFileEntry *e);

/* Sanitize a wire-supplied remote file name to a safe local
 * basename. The Hotline name's bytes ride to the wire untouched
 * (it can legitimately contain '/' under the Classic-Mac
 * convention), but when we use it to build an on-disk path we
 * must defang against path-traversal: a hostile server could
 * supply "../../etc/passwd" or similar and escape the user's
 * chosen download folder. This function strips path separators
 * (replacing '/' and '\\' with '_'), rejects pure-dot names
 * ("." and ".."), and falls back to "download" for empty /
 * dangerous inputs. Caller frees with g_free. */
extern char *hx_files_provider_safe_local_basename (const char *remote_name);

G_END_DECLS

#endif /* HX_FILES_PROVIDER_H */
