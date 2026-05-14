/*
 * preview.h — file-transfer preview window.
 *
 * The HTXF subchannel can stream a file's data fork into an
 * in-process viewer (instead of writing it to disk) when the user
 * picks "Preview" instead of "Download". The viewer pipeline is a
 * tiny dispatcher built around a set of per-format viewer plugins:
 *
 *   - rcv.c::rcv_task_file_get constructs the preview window on the
 *     main thread (window scaffolding only — placeholder body) via
 *     hx_preview_new() and stashes the handle on htxf->preview.
 *   - The worker thread (xfers.c::get_thread) reads the FILP
 *     wrapper header, learns the file's type/creator, and calls
 *     hx_preview_set_info(). The preview module marshals to main,
 *     picks a viewer based on type/creator/filename, and swaps the
 *     placeholder body for the viewer-specific widget (GtkTextView
 *     for plain text, GtkPicture for images, ...).
 *   - The worker streams each chunk via hx_preview_chunk(); the
 *     viewer's per-chunk callback runs on main (the preview module
 *     handles the worker→main marshal).
 *   - When the worker finishes the data fork, hx_preview_done()
 *     gives the viewer a chance to commit anything pending (a
 *     GdkPixbufLoader's close call, for instance, only materialises
 *     the final image when the loader is closed).
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
 * hx_preview_set_info() arrives. */
extern hx_preview *hx_preview_new (const char *name);

/* Worker thread (any thread). Sets the type/creator metadata used
 * by the dispatcher. Called by the HTXF worker after it parses the
 * FILP wrapper. Triggers the actual viewer selection — by the time
 * the first chunk arrives, the viewer is in place. */
extern void hx_preview_set_info (hx_preview *p, const char *type,
                                 const char *creator);

/* Worker thread (any thread). Stream a chunk of the file's data
 * fork. Bytes are copied; caller's buffer can be reused on return. */
extern void hx_preview_chunk (hx_preview *p, const char *buf, gsize len);

/* Worker thread (any thread). End-of-stream signal — the viewer
 * commits any pending state (close the pixbuf loader, etc). */
extern void hx_preview_done (hx_preview *p);

#endif /* HX_PREVIEW_H */
