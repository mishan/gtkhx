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

/* Construct a panel bound to `provider`. The panel takes a ref
 * on the provider and releases it on panel_free. Initial list
 * is triggered here so the widget is populated by the time the
 * browser presents it. */
extern files_panel *files_panel_new (HxFilesProvider *provider);

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
