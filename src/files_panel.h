/*
 * files_panel.h — one panel of the orthodox files browser.
 *
 * A self-contained vertical box widget that owns a path-entry
 * row, a sortable GtkColumnView, and a status footer. Bound to
 * an HxLocalFilesProvider (Phase 1; Phase 2 adds remote).
 *
 * The panel has a notion of "active": when the active panel
 * gets a CSS class for its accent border, and the browser's
 * cross-panel operations source their selection from it.
 */

#ifndef HX_FILES_PANEL_H
#define HX_FILES_PANEL_H 1

#include <gtk/gtk.h>

#include "files_entry.h"
#include "files_provider.h"

G_BEGIN_DECLS

typedef struct _files_panel files_panel;

/* Callback fired when the user picks a different side (Local /
 * Remote) from the panel's side dropdown. The handler is expected
 * to construct a fresh provider of the requested kind and call
 * files_panel_set_provider on `p`. The panel doesn't own provider
 * construction itself — the browser is the single source of truth
 * for which providers exist, so it handles the create + assign.
 *
 * `want_local` is TRUE if the user picked Local, FALSE if Remote.
 * If the handler ignores the request (e.g. the requested kind is
 * already the active provider), the dropdown reverts. */
typedef void (*files_panel_swap_cb) (files_panel *p, gboolean want_local,
                                     gpointer user_data);

/* Construct a panel bound to `provider`. The panel takes a ref
 * on the provider and releases it on panel_free. Initial list
 * is triggered here so the widget is populated by the time the
 * browser presents it.
 *
 * `swap_cb` is invoked when the user picks a different side from
 * the panel's side dropdown. NULL hides the dropdown — the panel
 * is then locked to the side its initial provider sets.
 *
 * Inline rename is handled entirely inside the panel (the row's
 * GtkEditableLabel does the edit + commit), so the constructor
 * no longer needs a rename callback. The headerbar Rename button
 * + F2 shortcut still go through the browser's open_rename_dialog
 * helper for the "I want a dialog" workflow. */
extern files_panel *files_panel_new (HxFilesProvider *provider,
                                     files_panel_swap_cb swap_cb,
                                     gpointer swap_cb_user_data);

/* Replace the panel's provider in place. Used by the side-
 * selector swap path: the panel disconnects from the old
 * provider, releases its ref, takes a ref on the new one, and
 * rewires the model / signals / path-completion popover to
 * match. The widget tree (root box, path row, frame, column
 * view, footer) stays put — only the model chain underneath
 * changes — so the user's scroll position is preserved within
 * reason though their previous selection is dropped.
 *
 * Safe to call with NULL; safe to call with the same provider
 * the panel already has (no-op). */
extern void files_panel_set_provider (files_panel *p,
                                      HxFilesProvider *new_provider);

/* Returns the root widget for embedding into the browser's
 * GtkPaned / GtkBox. Owned by the panel; do not unref. */
extern GtkWidget *files_panel_get_widget (files_panel *p);

/* GtkColumnView accessor — the browser hooks the focus controller
 * here to detect "is this panel the active one". */
extern GtkWidget *files_panel_get_column_view (files_panel *p);

/* Provider accessor — the browser's action handlers operate
 * through this. */
extern HxFilesProvider *files_panel_get_provider (files_panel *p);

/* Mark / unmark the panel as the active one. Toggles the
 * "files-panel-active" CSS class on the panel's outer frame. */
extern void files_panel_set_active (files_panel *p, gboolean active);

/* If EXACTLY one entry is selected, return it (caller does NOT
 * own a ref — model still holds it). NULL if nothing or
 * multi-selected. Convenience over files_panel_get_selected_entries
 * for callers that only make sense on a singleton (rename in
 * place, get-info, etc.). */
extern HxFileEntry *files_panel_get_single_selected (files_panel *p);

/* All currently-selected entries. Returns a fresh GPtrArray of
 * HxFileEntry* (caller owns the refs — free with
 * g_ptr_array_unref, which invokes g_object_unref on each).
 * Empty array when nothing selected. Order is row-position
 * ascending (i.e. visual order). */
extern GPtrArray *files_panel_get_selected_entries (files_panel *p);

/* Free a panel built by files_panel_new. The widget tree is
 * destroyed via gtk_widget_unparent if needed. */
extern void files_panel_free (files_panel *p);

G_END_DECLS

#endif /* HX_FILES_PANEL_H */
