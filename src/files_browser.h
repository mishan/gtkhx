/*
 * files_browser.h — orthodox two-panel files browser.
 *
 * One toplevel window, two side-by-side files_panel widgets — one
 * local, one remote by default. Tab switches the active panel.
 * Headerbar carries the cross-panel actions (Refresh / MkDir /
 * Delete on the active panel, F5/Copy across panels).
 */

#ifndef HX_FILES_BROWSER_H
#define HX_FILES_BROWSER_H 1

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Declared rather than included: this header needs the name, not the layout,
 * and session.h drags in the whole GTK-bearing session surface. A consumer
 * that dereferences one includes session.h itself. */
typedef struct _session session;

/* Open the Files panel for `sess` — the session whose files it lists.
 *
 * Idempotent, and the second call is not a second browser: the browser is
 * still a singleton, so an existing one is brought forward and `sess` is
 * ignored. This is the gtkhx-ui `files` Rust shell (raise-if-open + dock
 * registration via dock_bridge); gtkhx_files_build_content below is its C
 * content-build hook, mirroring news_browser.c. */
extern void open_files_browser (session *sess);

/* Build the browser's content box for `sess`, which it keeps for its lifetime
 * and routes every send through.
 *
 * `sess` must be non-NULL — see the note on the definition.
 *
 * A NULL return means "don't embed anything", and has two causes: a browser
 * already exists (the shell's raise-if-open covers that), or `sess` was NULL,
 * which also logs. */
extern GtkWidget *gtkhx_files_build_content (session *sess);

G_END_DECLS

#endif /* HX_FILES_BROWSER_H */
