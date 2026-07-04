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

/* Open the unified files browser. Action-style signature so the GAction in
 * toolbar.c can use it directly. Idempotent: a second call brings the existing
 * panel forward. This is the gtkhx-ui `files` Rust shell (raise-if-open + dock
 * registration via dock_bridge); gtkhx_files_build_content is its C
 * content-build hook, returning the browser content box (or NULL if already
 * built), mirroring news_browser.c. */
extern void open_files_browser (void);
extern GtkWidget *gtkhx_files_build_content (void);

G_END_DECLS

#endif /* HX_FILES_BROWSER_H */
