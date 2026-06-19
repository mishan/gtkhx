/*
 * files_browser.h — orthodox two-panel files browser.
 *
 * one toplevel window, two side-by-side
 * files_panel widgets, both bound to local-files providers.
 * Tab switches the active panel. Headerbar carries the
 * cross-panel actions (Phase 1 just has Refresh / MkDir / Delete
 * scoped to the active panel — copy/move between panels comes
 * in Phase 3).
 *
 * Phase 2 will introduce remote providers and a per-panel side
 * selector (local ↔ remote). The plumbing in files_browser.c is
 * already side-agnostic in design — Phase 1 just instantiates
 * two locals.
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
