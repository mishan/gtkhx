/*
 * files_browser.h — the two-panel files browser's C entry points. The
 * browser is gtkhx-ui's `files` module; see docs/files-browser.md.
 */

#ifndef HX_FILES_BROWSER_H
#define HX_FILES_BROWSER_H 1

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Declared rather than included: this header needs the name, not the layout,
 * and session.h drags in the whole GTK-bearing session surface. A consumer
 * that dereferences one includes session.h itself. */
typedef struct _session session;

/* Open the Files window for `sess` — the session whose files it lists —
 * or raise it if it's open. One window per connection. This is the
 * gtkhx-ui `files` module. */
extern void open_files_browser (session *sess);

/* Retitle `sess`'s Files window from what the session is now called — the
 * server's name, once login has delivered it. No-op without a window. */
extern void gtkhx_files_window_refresh_title (session *sess);

G_END_DECLS

#endif /* HX_FILES_BROWSER_H */
