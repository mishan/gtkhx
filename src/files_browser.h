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

/* Open the unified files browser. Action-style signature so the
 * GAction in toolbar.c can use it directly. Idempotent: a second
 * call brings the existing window forward. */
extern void open_files_browser (void);

G_END_DECLS

#endif /* HX_FILES_BROWSER_H */
