/*
 * preview.h — file-transfer preview window.
 *
 * The HTXF subchannel can stream a file's data fork into an
 * in-process viewer (instead of writing it to disk) when the user
 * picks "Preview" instead of "Download":
 *
 *   - hxhandlers::recv::xfer::download_ready builds the window on the
 *     main thread with hx_preview_new() and stashes the handle on
 *     htxf->preview.
 *   - The download worker reads the FILP header and calls
 *     hx_preview_set_info(), which picks a viewer by type, creator
 *     and filename, and says how much of the data fork to stream.
 *   - The worker streams that much through hx_preview_chunk(), then
 *     calls hx_preview_done() so the viewer can commit (an image
 *     decodes only once it has every byte).
 *
 * Threading: hx_preview_set_info, _chunk, and _done are all safe to
 * call from the HTXF worker; the implementation marshals to the
 * main thread internally. hx_preview_new must run on main (it
 * builds widgets).
 */

#ifndef HX_PREVIEW_H
#define HX_PREVIEW_H 1

#include <glib.h>

typedef struct hx_preview hx_preview;

/* Main thread. Creates the preview window with a "Loading…"
 * placeholder. The chosen viewer is installed once
 * hx_preview_set_info() arrives.
 *
 * Returns a struct at refcount=2: one ref is held internally by
 * the preview window (dropped in its close-request handler); the
 * other belongs to the caller and must be released with
 * hx_preview_unref() when the caller is done with the pointer.
 * (The HTXF worker stashes the returned pointer on htxf->preview
 * and drops the ref in htxf_unref.) */
extern hx_preview *hx_preview_new (const char *name);

/* Drop one ref. The struct is freed when the last ref goes away.
 * Safe to call with NULL, and from any thread: the refcount is
 * atomic and the last unref frees no widgets. */
extern void hx_preview_unref (hx_preview *p);

/* Worker thread (any thread). The file's type/creator, from the FILP
 * header, and the length of its data fork. Returns how many bytes of
 * the data fork to stream: all of it, the start of a long text, or
 * none of a file too large for its viewer. */
extern guint64 hx_preview_set_info (hx_preview *p, const char *type,
                                    const char *creator, guint64 size);

/* Worker thread (any thread). Stream a chunk of the file's data
 * fork. Bytes are copied; caller's buffer can be reused on return. */
extern void hx_preview_chunk (hx_preview *p, const char *buf, gsize len);

/* Worker thread (any thread). End-of-stream signal — the viewer
 * commits any pending state. */
extern void hx_preview_done (hx_preview *p);

/* Main thread. Called when the user closes the window before
 * hx_preview_done, to cancel the transfer: with the window gone,
 * the rest of the file has nowhere to go. */
typedef void (*hx_preview_cancel_fn) (void *user_data);
extern void hx_preview_set_cancel_cb (hx_preview *p, hx_preview_cancel_fn fn,
                                      void *user_data);

/* Main thread. Download the whole file, for a preview that shows only
 * part of it or none; returns FALSE when it can't, the connection
 * having gone. destroy frees user_data with the preview. */
typedef gboolean (*hx_preview_download_fn) (void *user_data);
extern void hx_preview_set_download_cb (hx_preview *p,
                                        hx_preview_download_fn fn,
                                        void *user_data,
                                        GDestroyNotify destroy);

#endif /* HX_PREVIEW_H */
