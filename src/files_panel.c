/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "config.h"

#include <gtk/gtk.h>

#include "hx.h"        /* the_session */
#include "debug.h"     /* debug_log — GTKHX_DEBUG=files for inline-rename trace */
#include "hl_access.h" /* HL_ACCESS_* + hl_access_has */
#include "files.h"     /* ICON_* */
#include "files_complete.h"
#include "files_entry.h"
#include "files_local_provider.h"
#include "files_provider.h"
#include "files_remote_provider.h" /* listing-error query for empty-state */
#include "files_panel.h"

/* hx.h pulls compat.h which defines _(s) as a passthrough; undef
 * before gi18n.h gives us the proper gettext() expansion without
 * the redefine warning. */
#undef _
#include <glib/gi18n.h>

struct _files_panel {
    GtkWidget *root;  /* GtkBox, top-level for embedding */
    GtkWidget *frame; /* GtkFrame around the column view —
	                              * carries the active-panel CSS class */

    GtkWidget *path_entry;    /* GtkEntry, current path text input */
    GtkWidget *up_btn;        /* one-shot up-one-level shortcut */
    GtkWidget *side_dropdown; /* GtkDropDown, Local / Remote.
	                              * NULL when swap_cb is NULL
	                              * (panel locked to its initial
	                              * provider). */
    gulong side_dropdown_handler;

    GtkWidget *column_view; /* GtkColumnView */
    GtkMultiSelection *selection;
    GtkSortListModel *sort_model;

    GtkWidget *status_label; /* footer: "N items" / "M of N selected" */

    HxFilesProvider *provider;
    gulong navigated_handler;
    gulong unavailable_handler;
    gulong items_changed_handler; /* on provider's listing */

    /* User callback for "I want this panel to switch sides".
	 * Browser-side: creates a fresh provider of the requested
	 * type and calls files_panel_set_provider. */
    files_panel_swap_cb swap_cb;
    gpointer swap_cb_user_data;

    /* Backing for the click-on-selected-name rename gesture.
	 * When the row-click gesture fires, we record the row's
	 * entry + its GtkEditableLabel widget and arm a 350 ms
	 * timer. If the timer fires without being cancelled by a
	 * subsequent click (the second click of a double-click
	 * cancels it via row activation), we call
	 * gtk_editable_label_start_editing on the label and the
	 * user gets a focused in-line text entry to retype the
	 * name. Commit/cancel is wired through the label's
	 * notify::editing signal in on_label_editing_changed.
	 *
	 * The 350 ms threshold is above GtkSettings's default
	 * double-click-time (250 ms in GNOME defaults) with a
	 * small safety margin, so a normal double-click never
	 * trips the inline-rename path.
	 *
	 * pending_label is g_object_ref'd while the timer is
	 * armed: between click and fire a provider swap or items-
	 * changed could destroy the row and free the label out
	 * from under us. The ref keeps the widget memory alive
	 * long enough for inline_rename_fire to safely inspect
	 * it; the pending_name check then rejects fire if the
	 * label was recycled to a different entry.
	 * inline_rename_cancel is also called from
	 * panel_detach_provider and on_items_changed so the
	 * timer doesn't outlive a listing swap.
	 *
	 * pending_name is a strdup of the entry's name at click
	 * time, used by inline_rename_fire to confirm the label
	 * is still bound to the same entry before kicking off
	 * editing — a defensive check against rebind-between-
	 * click-and-timer-fire. */
    guint inline_rename_timer_id;
    HxFileEntry *inline_rename_pending; /* full ref */
    GtkWidget *inline_rename_pending_label; /* full ref while armed */
    char *inline_rename_pending_name;

    /* The GtkEditableLabel currently in edit mode, or NULL.
	 * Tracked here (not queried from the model) so we can find
	 * and stop the previously-editing label when the user clicks
	 * a different row. Refreshed in on_label_editing_changed on
	 * every editing-state transition; held without a ref because
	 * the column view owns the label widget. */
    GtkWidget *editing_label;

    /* Per-panel record of the last primary-button click. The
	 * inline-rename gate compares the current click to this:
	 * the rename arms only when the user clicks the SAME row
	 * twice, with a pause between them long enough that it
	 * couldn't be a double-click. This is the Finder /
	 * Nautilus / GNOME Files gesture.
	 *
	 * Earlier drafts tried to gate by reading the selection
	 * model — both pre-click snapshot (couldn't reliably
	 * fire before the column view's own selection update,
	 * regardless of which ancestor widget we attached the
	 * snapshot gesture to) and post-click selection-changed
	 * time-stamping (selection-changed didn't always fire
	 * synchronously with the click event the way we needed).
	 * Tracking our own click history avoids both pitfalls —
	 * we don't depend on phase order or signal timing, only
	 * on the two presses arriving in the same widget. */
    gint last_clicked_pos;       /* -1 initially; row position of last click */
    gint64 last_click_time_us;   /* monotonic time of last click */

    /* Set TRUE when the user triggers a navigation FROM this
	 * panel (double-click row, Up button, path entry Enter).
	 * On the matching "navigated" reply we grab focus back to
	 * the column view, since populate_from_chunks (for remote
	 * providers) ends up destroying the row widgets that may
	 * have held focus during the descend — GTK's focus-fallback
	 * normally lands on the window's last-focused widget, which
	 * is the other panel. Without this nudge, focus shifts to
	 * the inactive panel on every directory change. */
    gboolean wants_focus_restore;

    /* Cached row icons keyed by ICON_* id. Lazy-populated via
	 * lookup_icon_paintable on first row that needs each icon;
	 * dropped on panel_free. Holding the GdkPaintable refs on
	 * the panel sidesteps the Adwaita gtk_image_set_from_resource
	 * path that renders blank for our small bundled PNGs (the
	 * same workaround used by news_browser and the toolbar
	 * buttons). */
    GHashTable *icons; /* guint16 icon_id → GdkPaintable (1.5x scaled) */

    /* Path-completion popover, local-provider panels only. NULL
	 * on remote panels (we can't synchronously enumerate without
	 * an RPC round-trip, so we don't try). See files_complete.c. */
    hx_path_complete *path_complete;
};

/* Map an ICON_* id to a gresource path. Returns NULL for ids
 * we don't have a dedicated icon for — caller falls back to
 * ICON_FILE (or ICON_FOLDER for folders, already resolved at
 * entry-construction time). */
static const char *
icon_resource_for_id (guint16 icon_id)
{
    switch (icon_id) {
    case ICON_FOLDER:
        return "/com/nasledov/gtkhx/pixmaps/folder.png";
    case ICON_FOLDER_IN:
        return "/com/nasledov/gtkhx/pixmaps/folder_dropbox.png";
    case ICON_FILE:
        return "/com/nasledov/gtkhx/pixmaps/file.png";
    case ICON_FILE_HTft:
        return "/com/nasledov/gtkhx/pixmaps/file_html.png";
    case ICON_FILE_SIT:
    case ICON_FILE_SITP:
        return "/com/nasledov/gtkhx/pixmaps/file_sit.png";
    case ICON_FILE_IMAGE:
        return "/com/nasledov/gtkhx/pixmaps/file_image.png";
    case ICON_FILE_APPL:
        return "/com/nasledov/gtkhx/pixmaps/file_app.png";
    case ICON_FILE_alis:
        return "/com/nasledov/gtkhx/pixmaps/file_alias.png";
    case ICON_FILE_DISK:
        return "/com/nasledov/gtkhx/pixmaps/file_disk.png";
    case ICON_FILE_NOTE:
        return "/com/nasledov/gtkhx/pixmaps/file_note.png";
    case ICON_FILE_MOOV:
        return "/com/nasledov/gtkhx/pixmaps/file_movie.png";
    case ICON_FILE_TEXT:
        return "/com/nasledov/gtkhx/pixmaps/file_text.png";
    case ICON_FILE_ZIP:
        return "/com/nasledov/gtkhx/pixmaps/file_zip.png";
    default:
        return NULL;
    }
}

/* Load an icon resource (XPM or PNG) and wrap it in a GdkPaintable
 * scaled 1.5x with nearest-neighbor interpolation. Same treatment
 * as news_browser.c so both browsers' row chrome looks consistent;
 * the cicn-derived PNGs we extract from icons.rsrc are also 16x16,
 * so they scale the same way the XPMs do. Returns NULL silently
 * on a missing resource — callers null-check. */
static GdkPaintable *
load_icon_paintable (const char *resource)
{
    GdkPixbuf *pb, *scaled;
    GdkTexture *tex;
    int w, h;

    pb = gdk_pixbuf_new_from_resource (resource, NULL);
    if (!pb) {
        return NULL;
    }
    w = (gdk_pixbuf_get_width (pb) * 3) / 2;
    h = (gdk_pixbuf_get_height (pb) * 3) / 2;
    scaled = gdk_pixbuf_scale_simple (pb, w, h, GDK_INTERP_NEAREST);
    g_object_unref (pb);
    if (!scaled) {
        return NULL;
    }

    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    tex = gdk_texture_new_for_pixbuf (scaled);
    G_GNUC_END_IGNORE_DEPRECATIONS
    g_object_unref (scaled);
    return GDK_PAINTABLE (tex);
}

/* Lazy-cache lookup. Returns a borrowed GdkPaintable* (panel owns
 * the ref via p->icons). NULL if neither the requested id nor the
 * ICON_FILE fallback could be loaded. */
static GdkPaintable *
lookup_icon_paintable (files_panel *p, guint16 icon_id)
{
    GdkPaintable *cached;
    const char *resource;

    if (!p || !p->icons) {
        return NULL;
    }

    cached = g_hash_table_lookup (p->icons, GUINT_TO_POINTER ((guint)icon_id));
    if (cached) {
        return cached;
    }

    resource = icon_resource_for_id (icon_id);
    if (!resource) {
        /* Unknown id → fall back to ICON_FILE (or ICON_FOLDER for
		 * the not-meaningful case of icon_id==0 sneaking through). */
        return lookup_icon_paintable (p, ICON_FILE);
    }

    cached = load_icon_paintable (resource);
    if (cached) {
        g_hash_table_insert (p->icons, GUINT_TO_POINTER ((guint)icon_id),
                             cached);
    }
    return cached;
}

/* ---- Custom sorters ---- */

/* Name comparator. Folders bubble to the top (orthodox FM
 * convention) so the user can drill in without scanning past
 * mixed-up files. Among same-kind rows we sort case-insensitive
 * for a more intuitive A→Z. */
static int
cmp_name (gconstpointer a_p, gconstpointer b_p, gpointer user_data)
{
    HxFileEntry *a = (HxFileEntry *)a_p;
    HxFileEntry *b = (HxFileEntry *)b_p;
    gboolean ad, bd;
    (void)user_data;

    ad = hx_file_entry_is_dir (a);
    bd = hx_file_entry_is_dir (b);
    if (ad != bd) {
        return ad ? -1 : 1;
    }
    return g_utf8_collate (hx_file_entry_get_name (a),
                           hx_file_entry_get_name (b));
}

/* Helper: returns non-zero if a and b are different kinds
 * (folder vs file). Used by the size/modified/kind comparators
 * below to bubble folders to the top in the same orthodox-FM
 * way cmp_name does. Doing this consistently across all
 * columns matters more now that folder size is a child count
 * rather than a byte count — sorting a 7-item folder against a
 * 7-byte file otherwise produces a comparison that has no
 * meaning to the user. */
static int
dir_first (HxFileEntry *a, HxFileEntry *b)
{
    gboolean ad = hx_file_entry_is_dir (a);
    gboolean bd = hx_file_entry_is_dir (b);
    if (ad != bd) {
        return ad ? -1 : 1;
    }
    return 0;
}

static int
cmp_size (gconstpointer a_p, gconstpointer b_p, gpointer user_data)
{
    HxFileEntry *a = (HxFileEntry *)a_p;
    HxFileEntry *b = (HxFileEntry *)b_p;
    guint64 as, bs;
    int dirs;
    (void)user_data;

    dirs = dir_first (a, b);
    if (dirs) {
        return dirs;
    }
    as = hx_file_entry_get_size (a);
    bs = hx_file_entry_get_size (b);
    if (as < bs) {
        return -1;
    }
    if (as > bs) {
        return 1;
    }
    return 0;
}

static int
cmp_modified (gconstpointer a_p, gconstpointer b_p, gpointer user_data)
{
    HxFileEntry *a = (HxFileEntry *)a_p;
    HxFileEntry *b = (HxFileEntry *)b_p;
    gint64 am, bm;
    int dirs;
    (void)user_data;

    dirs = dir_first (a, b);
    if (dirs) {
        return dirs;
    }
    am = hx_file_entry_get_modified (a);
    bm = hx_file_entry_get_modified (b);
    if (am < bm) {
        return -1;
    }
    if (am > bm) {
        return 1;
    }
    return 0;
}

static int
cmp_kind (gconstpointer a_p, gconstpointer b_p, gpointer user_data)
{
    HxFileEntry *a = (HxFileEntry *)a_p;
    HxFileEntry *b = (HxFileEntry *)b_p;
    int dirs;
    (void)user_data;

    dirs = dir_first (a, b);
    if (dirs) {
        return dirs;
    }
    return g_utf8_collate (hx_file_entry_get_kind (a),
                           hx_file_entry_get_kind (b));
}

/* ---- Column factories ---- */

/* Cancel any pending inline-rename timer. Called when the
 * gesture is superseded — a double-click that activates the row,
 * a different row click, panel teardown, items-changed on the
 * underlying listing, or a provider swap. */
static void
inline_rename_cancel (files_panel *p)
{
    if (p->inline_rename_timer_id) {
        g_source_remove (p->inline_rename_timer_id);
        p->inline_rename_timer_id = 0;
    }
    g_clear_object (&p->inline_rename_pending);
    /* The label is owned via g_object_ref while the timer is
	 * armed (see arming site at the end of on_name_label_pressed
	 * and the field comment in struct _files_panel). Release that
	 * ref here. g_clear_object on the GtkWidget* is safe — GTK
	 * widgets are GObjects. */
    g_clear_object (&p->inline_rename_pending_label);
    g_clear_pointer (&p->inline_rename_pending_name, g_free);
}

static gboolean
inline_rename_fire (gpointer user_data)
{
    files_panel *p = user_data;
    HxFileEntry *e;
    GtkWidget *label;
    char *expected_name;

    p->inline_rename_timer_id = 0;

    /* Drain the pending state into locals before doing anything that
	 * could re-enter (the start-editing call below transfers focus
	 * which can fire other signal handlers). */
    e = p->inline_rename_pending;
    label = p->inline_rename_pending_label;
    expected_name = p->inline_rename_pending_name;
    p->inline_rename_pending = NULL;
    p->inline_rename_pending_label = NULL;
    p->inline_rename_pending_name = NULL;

    debug_log ("files", "inline_rename_fire: expected=%s",
               expected_name ? expected_name : "(null)");

    if (label && GTK_IS_EDITABLE_LABEL (label) && expected_name) {
        /* Defensive: confirm the label is still bound to the same
		 * entry we armed against. The column view recycles row
		 * widgets, so a rebind during the 350 ms window could leave
		 * this label pointing at a different file. The "old-name"
		 * data refreshed in name_bind is our anchor. */
        const char *current
            = g_object_get_data (G_OBJECT (label), "old-name");
        debug_log ("files",
                   "  current_label_name=%s editing=%d",
                   current ? current : "(null)",
                   gtk_editable_label_get_editing (GTK_EDITABLE_LABEL (label)));
        if (g_strcmp0 (current, expected_name) == 0
            && !gtk_editable_label_get_editing (GTK_EDITABLE_LABEL (label))) {
            debug_log ("files", "  START editing");
            /* Flip editable on first so start_editing isn't a no-op
			 * (GtkEditableLabel won't enter edit mode when the
			 * underlying GtkEditable.editable is FALSE). The leave-
			 * edit handler flips it back to FALSE.
			 *
			 * Also flip can_target back to TRUE so the in-place
			 * GtkEntry can receive clicks for cursor positioning
			 * and text selection while the user is editing. The
			 * leave-edit handler flips it back to FALSE so the
			 * post-edit display state is click-through again. */
            gtk_editable_set_editable (GTK_EDITABLE (label), TRUE);
            gtk_widget_set_can_target (label, TRUE);
            gtk_editable_label_start_editing (GTK_EDITABLE_LABEL (label));
            /* Select all so the user can immediately type a
			 * replacement, matching Finder / Nautilus behaviour. */
            gtk_editable_select_region (GTK_EDITABLE (label), 0, -1);
        }
    }
    g_clear_object (&e);
    /* The label held a g_object_ref taken at arm time; release it
	 * now (drain transferred ownership of the ref to `label`). */
    g_clear_object (&label);
    g_free (expected_name);
    return G_SOURCE_REMOVE;
}

/* Stop any in-progress inline rename without committing.
 *
 * Order matters: gtk_editable_label_stop_editing(FALSE) must run
 * BEFORE we move focus off the entry. If grab_focus on the column
 * view ran first, label_A's internal GtkText would lose focus,
 * which triggers GtkEditableLabel's own focus-out path — that
 * commits and finalizes through a different code path that
 * doesn't always swap the internal GtkStack back to label mode
 * promptly (the entry chrome visibly sticks around in that case,
 * which is exactly the symptom we're working around). Calling our
 * explicit stop_editing(FALSE) first triggers the direct
 * stack-swap path; the focus move afterwards is a defensive
 * cleanup for builds where stop_editing alone leaves focus
 * orphaned on the now-hidden entry.
 *
 * Safe to call when no edit is in progress (early-returns on
 * editing_label==NULL). */
static void
panel_stop_inline_edit (files_panel *p)
{
    GtkWidget *label;

    if (!p || !p->editing_label) {
        return;
    }
    label = p->editing_label;
    if (!GTK_IS_EDITABLE_LABEL (label)) {
        p->editing_label = NULL;
        return;
    }
    debug_log ("files", "panel_stop_inline_edit: cancelling edit");

    /* Direct programmatic exit — switches the internal stack from
	 * entry to label mode synchronously. Triggers notify::editing
	 * (FALSE) which on_label_editing_changed handles: clears
	 * p->editing_label, flips editable back to FALSE, handles the
	 * commit/revert decision (no commit here since FALSE). */
    gtk_editable_label_stop_editing (GTK_EDITABLE_LABEL (label),
                                     /*commit=*/FALSE);

    /* Move focus off the (now-hidden) entry. After stop_editing
	 * the entry is no longer the stack's visible-child, but focus
	 * may still be on it logically. Without this, subsequent
	 * keyboard input goes nowhere useful. */
    if (p->column_view) {
        gtk_widget_grab_focus (p->column_view);
    }

    /* Defensive: on_label_editing_changed normally handles the
	 * cleanup, but force it here too in case notify::editing didn't
	 * fire (some GTK builds skip the notify when called from inside
	 * another focus transition). */
    if (p->editing_label == label) {
        p->editing_label = NULL;
    }
    gtk_editable_set_editable (GTK_EDITABLE (label), FALSE);
    gtk_widget_set_can_target (label, FALSE);
}

/* notify::editing handler — fires twice per inline-rename
 * session: once when the user enters editing (we use that to
 * select-all the text) and once when they leave (Enter, Escape,
 * or focus-out). On the leave-edit transition, commit if the
 * text changed; revert silently on empty or failed-rename.
 *
 * Errors get a g_warning rather than a toast because the panel
 * doesn't have access to the browser's AdwToastOverlay — adding
 * an error callback through files_panel_new is overkill for what
 * should be a rare failure path. */
static void
on_label_editing_changed (GObject *obj, GParamSpec *pspec, gpointer user_data)
{
    files_panel *p = user_data;
    GtkEditableLabel *el = GTK_EDITABLE_LABEL (obj);
    gboolean editing;
    const char *new_name, *old_name;
    GError *err = NULL;
    (void)pspec;

    editing = gtk_editable_label_get_editing (el);
    if (editing) {
        p->editing_label = GTK_WIDGET (el);
        gtk_editable_select_region (GTK_EDITABLE (el), 0, -1);
        return;
    }

    /* Editing finished. Flip editable back to FALSE so the next
	 * click on the now-display-mode label doesn't reopen edit
	 * mode via GtkEditableLabel's built-in click handler — our
	 * rename gate is the only thing that should re-enter edit.
	 * Also restore can_target=FALSE so the label is once again
	 * transparent to pointer picking — that's what lets clicks
	 * pass through to the row's gestures (selection, drag-source,
	 * double-click row-activate). See the name_setup docstring
	 * for the full rationale. */
    gtk_editable_set_editable (GTK_EDITABLE (el), FALSE);
    gtk_widget_set_can_target (GTK_WIDGET (el), FALSE);
    if (p->editing_label == GTK_WIDGET (el)) {
        p->editing_label = NULL;
    }

    new_name = gtk_editable_get_text (GTK_EDITABLE (el));
    old_name = g_object_get_data (obj, "old-name");
    if (!old_name) {
        return;
    }
    if (!new_name || !*new_name || g_strcmp0 (new_name, old_name) == 0) {
        /* Empty or unchanged — revert the displayed text to the
		 * known-good old name in case the user blanked the entry. */
        gtk_editable_set_text (GTK_EDITABLE (el), old_name);
        return;
    }

    if (!hx_files_provider_rename (p->provider, old_name, new_name, &err)) {
        g_warning ("files: inline rename %s -> %s failed: %s",
                   old_name, new_name,
                   err ? err->message : "(no message)");
        gtk_editable_set_text (GTK_EDITABLE (el), old_name);
        g_clear_error (&err);
        return;
    }
    /* Success: update the stashed old-name so a quick second edit
	 * compares against the new name. The provider's reload-on-
	 * navigated path will eventually rebind the row with the fresh
	 * name from the listing, which would also refresh old-name, but
	 * that's an async round-trip for the remote case. */
    g_object_set_data_full (obj, "old-name", g_strdup (new_name), g_free);
}

/* Click on the row. Arm the inline-rename timer if and only if
 * this click is the SECOND primary click on the same row, with
 * enough pause between the two to rule out a double-click. The
 * two-click-with-pause is the Finder / Nautilus / GNOME Files
 * rename gesture.
 *
 * We track the clicks ourselves (last_clicked_pos +
 * last_click_time_us) rather than reading the selection model
 * because earlier attempts to gate on selection state lost the
 * phase race — the column view updates its selection model
 * before any CAPTURE-phase observer we could attach was reached,
 * and selection-changed didn't always fire in time-based ways
 * we could use either. Tracking our own clicks is phase-order-
 * independent.
 *
 * We deliberately don't claim the gesture — the column view's
 * own click handler still runs alongside, so single-click selects
 * the row and double-click activates as usual. */
static void
on_name_label_pressed (GtkGestureClick *gesture, int n_press, double x,
                       double y, gpointer user_data)
{
    files_panel *p = user_data;
    GtkWidget *row, *label;
    GtkListItem *item;
    HxFileEntry *e;
    guint pos;
    gint64 now_us, delta_us;
    gboolean same_row, paused_enough;
    (void)x;
    (void)y;

    debug_log ("files", "on_name_label_pressed: n_press=%d", n_press);

    /* Only the very first press in a press-sequence is interesting;
	 * the second press of a double-click is what fires row activation
	 * and we want that to win. */
    if (n_press != 1) {
        debug_log ("files", "  cancel: n_press != 1");
        inline_rename_cancel (p);
        return;
    }
    row = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
    item = g_object_get_data (G_OBJECT (row), "list-item");
    if (!item) {
        debug_log ("files", "  bail: no list-item on row");
        return;
    }
    pos = gtk_list_item_get_position (item);
    e = gtk_list_item_get_item (item);
    debug_log ("files", "  pos=%u entry=%s",
               pos, e ? hx_file_entry_get_name (e) : "(null)");

    /* Synthesize an exclusive selection on plain clicks. With the
	 * label now set to can_target=FALSE (see name_setup), clicks
	 * already pass through to the column view's selection gesture,
	 * so this is mostly belt-and-suspenders — but keeping it makes
	 * sure the row visibly highlights as the first click of the
	 * two-click rename gesture even if any future GTK behaviour
	 * change re-routes the press elsewhere. Idempotent on already-
	 * selected rows.
	 *
	 * Only synthesize on a plain click. With Ctrl or Shift held the
	 * user means to toggle or extend the existing selection; an
	 * exclusive-select here would clobber GtkMultiSelection's
	 * modifier-aware logic running in the column view's own gesture
	 * (and break Ctrl/Shift-click multi-select on name-column
	 * clicks). On modified clicks we skip the synth so the column
	 * view's gesture is the sole arbiter of multi-select state. */
    {
        GdkEvent *ev = gtk_event_controller_get_current_event (
            GTK_EVENT_CONTROLLER (gesture));
        GdkModifierType mods = ev ? gdk_event_get_modifier_state (ev) : 0;
        if (!(mods & (GDK_CONTROL_MASK | GDK_SHIFT_MASK))) {
            gtk_selection_model_select_item (
                GTK_SELECTION_MODEL (p->selection), pos,
                /*exclusive=*/TRUE);
        }
    }

    /* If a different label was in edit mode and the user just
	 * clicked elsewhere, cancel that edit. The synthesized
	 * selection above will fire on_selection_changed which also
	 * calls panel_stop_inline_edit, but only when the click is
	 * on a row OTHER than the editing one — and we want to be
	 * safe across all phase-order outcomes, so call it directly
	 * here when this_label != editing_label. Idempotent. */
    {
        GtkWidget *this_label = g_object_get_data (G_OBJECT (row), "label");
        if (p->editing_label && p->editing_label != this_label) {
            panel_stop_inline_edit (p);
        }
    }

    if (!e) {
        debug_log ("files", "  cancel: null entry");
        inline_rename_cancel (p);
        return;
    }
    /* Earlier code gated this branch on hx_file_entry_is_dir(e)
	 * out of concern that users meant to navigate. That worry was
	 * load-bearing back when GtkEditableLabel's pointer-event
	 * eating made the click behaviour ambiguous (clicks on the
	 * label area sometimes reached the activate gesture, sometimes
	 * didn't). With name_setup's can_target=FALSE fix the gestures
	 * are now deterministic — single-click selects, double-click
	 * activates (opens the folder), two clicks with a >=350ms pause
	 * arm rename. These are well-separated, so inline-rename on
	 * directories is safe and matches Finder / Nautilus behaviour.
	 * Directories rename through the same hx_files_provider_rename
	 * call files do. */
    label = g_object_get_data (G_OBJECT (row), "label");
    if (!label || !GTK_IS_EDITABLE_LABEL (label)) {
        debug_log ("files", "  bail: no/invalid label widget");
        return;
    }

    /* If the row is already in edit mode, this click is the user
	 * trying to reposition the cursor inside the entry. Don't arm
	 * a new rename timer; the entry's own click handler will take
	 * care of cursor placement. */
    if (gtk_editable_label_get_editing (GTK_EDITABLE_LABEL (label))) {
        debug_log ("files", "  cancel: label already editing");
        inline_rename_cancel (p);
        return;
    }

    /* Two-click-with-pause gate. */
    {
        gint prev_pos = p->last_clicked_pos;
        now_us = g_get_monotonic_time ();
        same_row = (prev_pos == (gint) pos);
        delta_us = now_us - p->last_click_time_us;
        /* Update history for the next click BEFORE evaluating
		 * the arm — even if we don't arm here, this click is the
		 * baseline for the next one. */
        p->last_clicked_pos = (gint) pos;
        p->last_click_time_us = now_us;

        /* 350 ms is well above GtkSettings's default double-click
		 * time (250 ms) so a fast second click that forms a
		 * double-click doesn't arm; it falls through to
		 * on_row_activated. */
        paused_enough = (delta_us >= 350000);

        debug_log ("files",
                   "  gate: prev_pos=%d cur_pos=%u same_row=%d "
                   "delta_us=%lld paused_enough=%d",
                   prev_pos, pos, same_row, (long long) delta_us,
                   paused_enough);
    }

    if (!same_row || !paused_enough) {
        debug_log ("files", "  cancel: gate failed");
        inline_rename_cancel (p);
        return;
    }
    debug_log ("files", "  ARM rename timer (350ms) for pos=%u", pos);

    /* Same row, with a real pause between clicks → arm the
	 * 350 ms inline-rename timer. The timer's threshold matches
	 * the paused_enough comparison above so the gating and the
	 * fire delay use the same constant; a quick subsequent click
	 * inside that window will be on_row_activated (the second
	 * press of a double-click), which calls inline_rename_cancel
	 * to defuse this timer. */
    inline_rename_cancel (p);
    p->inline_rename_pending = g_object_ref (e);
    /* g_object_ref the label so a row-recycle / listing swap
	 * during the 350 ms window can't free it out from under
	 * inline_rename_fire. The fire path's pending_name check
	 * still rejects fire if the label was reassigned to a
	 * different entry. */
    p->inline_rename_pending_label = g_object_ref (label);
    p->inline_rename_pending_name = g_strdup (hx_file_entry_get_name (e));
    p->inline_rename_timer_id
        = g_timeout_add (350, inline_rename_fire, p);
}

/* Name column: icon + label. Icon comes from the panel-cached
 * XPM paintables (folder vs. generic file), passed through to
 * the bind callback via the factory's user_data slot.
 *
 * Phase 2 will look at HxFileEntry's `kind` to pick richer icons
 * for known Hotline file types (text, image, archive) — for now
 * the folder/file binary is enough. */
static void
name_setup (GtkSignalListItemFactory *f, GtkListItem *item, gpointer d)
{
    files_panel *p = d;
    GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *icon = gtk_image_new ();
    GtkWidget *lbl;
    GtkGesture *click;
    (void)f;
    (void)item;

    /* GtkEditableLabel renders as a plain label until edit mode is
	 * entered, then swaps in a GtkEntry-like in-place editor. The
	 * stop conditions (Enter / Escape / focus-out) are built in;
	 * we observe via notify::editing to commit the new name.
	 *
	 * IMPORTANT: GtkEditableLabel has built-in click-to-edit
	 * behaviour (clicking on the focused label starts editing).
	 * We don't want that — our own row gesture is the only thing
	 * that should trigger editing. Setting editable=FALSE here
	 * suppresses the built-in trigger; inline_rename_fire flips it
	 * to TRUE just before calling gtk_editable_label_start_editing,
	 * and the leave-edit handler flips it back to FALSE so a stray
	 * click on the now-back-to-label widget doesn't reopen edit
	 * mode behind our back.
	 *
	 * ALSO IMPORTANT: editable=FALSE alone doesn't stop the
	 * GtkEditableLabel from consuming pointer events — the inner
	 * GtkLabel still picks up presses for its own text-selection
	 * handling, which blocks the column view's row-activation
	 * gesture (double-click to open folders) and the drag-source
	 * controller (drag a file to copy / move) from ever seeing
	 * those events. set_can_target=FALSE makes the whole label
	 * subtree transparent to pointer picking, so clicks pass
	 * through to the row underneath. Our own inline-rename click
	 * gesture lives on the row's outer GtkBox (see the click
	 * controller attached below) so it still fires from the row
	 * side regardless of where in the row the user clicked.
	 * inline_rename_fire flips can_target back to TRUE before
	 * start_editing so the in-place GtkEntry can receive clicks
	 * for cursor positioning + text selection; the leave-edit
	 * handler flips it back to FALSE. */
    lbl = gtk_editable_label_new ("");
    gtk_editable_set_editable (GTK_EDITABLE (lbl), FALSE);
    gtk_widget_set_can_target (lbl, FALSE);
    gtk_widget_set_hexpand (lbl, TRUE);
    gtk_widget_set_halign (lbl, GTK_ALIGN_START);
    gtk_widget_set_valign (lbl, GTK_ALIGN_CENTER);

    /* XPMs are 16x16; scaled 1.5x = 24x24. Match that with
	 * pixel_size so GtkImage's icon-size clamp doesn't shrink
	 * them back down. */
    gtk_image_set_pixel_size (GTK_IMAGE (icon), 24);

    gtk_box_append (GTK_BOX (row), icon);
    gtk_box_append (GTK_BOX (row), lbl);
    gtk_list_item_set_child (item, row);

    g_object_set_data (G_OBJECT (row), "icon", icon);
    g_object_set_data (G_OBJECT (row), "label", lbl);

    /* notify::editing → commit on edit-end. The handler reads the
	 * pre-edit name from the "old-name" data slot name_bind keeps
	 * fresh, so it doesn't need its own state to know what changed. */
    g_signal_connect (lbl, "notify::editing",
                      G_CALLBACK (on_label_editing_changed), p);

    /* Click-on-selected-row → inline rename. The gesture sits on
	 * the row's outer GtkBox in CAPTURE phase so it sees the click
	 * before GtkColumnView's descendant click handlers can claim
	 * it (an earlier draft attached on the label in BUBBLE phase
	 * and never fired). The gate inside on_name_label_pressed is
	 * per-panel click history (last_clicked_pos + last_click_time_us):
	 * the rename arms only when the user clicks the SAME row twice
	 * with a pause long enough to rule out a double-click. Earlier
	 * drafts gated on selection state but lost the phase race —
	 * tracking our own clicks is phase-order-independent.
	 *
	 * We don't claim the gesture, so the column view's own
	 * selection / activation behaviour still works as expected
	 * even with our handler in the capture path. */
    click = gtk_gesture_click_new ();
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), GDK_BUTTON_PRIMARY);
    gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (click),
                                                GTK_PHASE_CAPTURE);
    g_signal_connect (click, "pressed", G_CALLBACK (on_name_label_pressed), p);
    gtk_widget_add_controller (row, GTK_EVENT_CONTROLLER (click));
}

static void
name_bind (GtkSignalListItemFactory *f, GtkListItem *item, gpointer d)
{
    files_panel *p = d;
    GtkWidget *row = gtk_list_item_get_child (item);
    HxFileEntry *e = gtk_list_item_get_item (item);
    GtkImage *icon = g_object_get_data (G_OBJECT (row), "icon");
    GtkLabel *lbl = g_object_get_data (G_OBJECT (row), "label");
    GdkPaintable *paintable;
    (void)f;

    /* Stash the GtkListItem on the row so on_name_label_pressed
	 * can recover the row's position and bound entry. Stored as a
	 * weak (non-ref) pointer — the column view owns the lifetime
	 * and re-binds the row when the underlying model changes. The
	 * gesture handler always re-reads from the list item rather
	 * than caching, so a recycled row carries the right state. */
    g_object_set_data (G_OBJECT (row), "list-item", item);

    if (!e) {
        gtk_image_clear (icon);
        gtk_editable_set_text (GTK_EDITABLE (lbl), "");
        /* set_data_full (not set_data) so the prior destroy_notify
		 * runs against the OLD strdup — otherwise the old "old-name"
		 * string leaks (set_data overwrites the qdata slot without
		 * invoking the destroy registered by the previous
		 * set_data_full call). */
        g_object_set_data_full (G_OBJECT (lbl), "old-name", NULL, NULL);
        return;
    }

    paintable = lookup_icon_paintable (p, hx_file_entry_get_icon_id (e));
    if (paintable) {
        gtk_image_set_from_paintable (icon, paintable);
    } else {
        gtk_image_clear (icon);
    }

    /* If a rebind lands while the user is mid-edit on this widget,
	 * cancel the edit before swapping the text out from under them
	 * — otherwise GtkEditableLabel's notify::editing-leave will
	 * commit garbage. Listings rarely change mid-edit but a server
	 * push or a sort-order change technically could. */
    if (gtk_editable_label_get_editing (GTK_EDITABLE_LABEL (lbl))) {
        gtk_editable_label_stop_editing (GTK_EDITABLE_LABEL (lbl),
                                         /*commit=*/FALSE);
    }
    gtk_editable_set_text (GTK_EDITABLE (lbl), hx_file_entry_get_name (e));
    /* old-name is what the inline-rename commit handler compares
	 * the post-edit text against to decide whether to call rename.
	 * Refreshed every rebind so a recycled row's data matches its
	 * currently-displayed entry. */
    g_object_set_data_full (G_OBJECT (lbl), "old-name",
                            g_strdup (hx_file_entry_get_name (e)), g_free);
}

/* Generic right-aligned label column shared by Size + Modified. */
static void
text_setup_right (GtkSignalListItemFactory *f, GtkListItem *item, gpointer d)
{
    GtkWidget *lbl = gtk_label_new (NULL);
    (void)f;
    (void)d;
    gtk_label_set_xalign (GTK_LABEL (lbl), 1.0f);
    gtk_label_set_ellipsize (GTK_LABEL (lbl), PANGO_ELLIPSIZE_END);
    gtk_list_item_set_child (item, lbl);
}

/* Generic left-aligned label column (Kind). */
static void
text_setup_left (GtkSignalListItemFactory *f, GtkListItem *item, gpointer d)
{
    GtkWidget *lbl = gtk_label_new (NULL);
    (void)f;
    (void)d;
    gtk_label_set_xalign (GTK_LABEL (lbl), 0.0f);
    gtk_label_set_ellipsize (GTK_LABEL (lbl), PANGO_ELLIPSIZE_END);
    gtk_list_item_set_child (item, lbl);
}

static void
size_bind (GtkSignalListItemFactory *f, GtkListItem *item, gpointer d)
{
    GtkLabel *lbl = GTK_LABEL (gtk_list_item_get_child (item));
    HxFileEntry *e = gtk_list_item_get_item (item);
    char *txt;
    (void)f;
    (void)d;

    txt = e ? hx_file_entry_format_size (e) : g_strdup ("");
    gtk_label_set_text (lbl, txt);
    g_free (txt);
}

static void
modified_bind (GtkSignalListItemFactory *f, GtkListItem *item, gpointer d)
{
    GtkLabel *lbl = GTK_LABEL (gtk_list_item_get_child (item));
    HxFileEntry *e = gtk_list_item_get_item (item);
    char *txt;
    (void)f;
    (void)d;

    txt = e ? hx_file_entry_format_modified (e) : g_strdup ("");
    gtk_label_set_text (lbl, txt);
    g_free (txt);
}

static void
kind_bind (GtkSignalListItemFactory *f, GtkListItem *item, gpointer d)
{
    GtkLabel *lbl = GTK_LABEL (gtk_list_item_get_child (item));
    HxFileEntry *e = gtk_list_item_get_item (item);
    (void)f;
    (void)d;
    gtk_label_set_text (lbl, e ? hx_file_entry_get_kind (e) : "");
}

/* ---- Status footer ---- */

static void
update_status (files_panel *p)
{
    GtkBitset *sel;
    guint n_total, n_sel;
    char *text;

    if (!p->status_label) {
        return;
    }

    n_total = g_list_model_get_n_items (G_LIST_MODEL (p->selection));

    /* GtkMultiSelection exposes its selection as a GtkBitset of
	 * row positions. Sized is the cardinality. The bitset is
	 * owned by the selection model; we don't need to free it. */
    sel = gtk_selection_model_get_selection (
        GTK_SELECTION_MODEL (p->selection));
    n_sel = sel ? (guint)gtk_bitset_get_size (sel) : 0;
    if (sel) {
        gtk_bitset_unref (sel);
    }

    /* If the remote provider's most recent FILE_LIST came back as
	 * a task error, the rows are empty and the user would just see
	 * "0 items" — which doesn't tell them what went wrong.
	 * Differentiate between drop-box and other listing failures by
	 * cross-referencing the user's access bits:
	 *
	 *   UPLOAD_FILES set + VIEW_DROP_BOXES unset
	 *     → almost certainly a drop-box; tell the user they can
	 *       still upload here.
	 *   anything else
	 *     → generic "can't list" message.
	 *
	 * Selection count short-circuits both because the user might
	 * have selected rows on a prior listing before navigating
	 * into the dropbox — but the listing is empty now so n_sel
	 * is always 0 on this path anyway. */
    if (p->provider && HX_IS_REMOTE_FILES_PROVIDER (p->provider)
        && hx_remote_files_provider_has_listing_error (
            HX_REMOTE_FILES_PROVIDER (p->provider))) {
        const guint8 *bits = (const guint8 *)&the_session.htlc.access;
        gboolean can_upload = hl_access_has (bits, HL_ACCESS_UPLOAD_FILES);
        gboolean can_view_dropbox
            = hl_access_has (bits, HL_ACCESS_VIEW_DROP_BOXES);
        if (the_session.htlc.fd && can_upload && !can_view_dropbox) {
            text = g_strdup (
                _ ("Folder is upload-only — drop files here to upload"));
        } else {
            text = g_strdup (_ ("Can't list this folder."));
        }
        gtk_widget_add_css_class (p->status_label, "warning");
        gtk_label_set_text (GTK_LABEL (p->status_label), text);
        g_free (text);
        return;
    }

    /* Successful listing — drop any sticky warning style. */
    gtk_widget_remove_css_class (p->status_label, "warning");

    if (n_sel == 0) {
        text = g_strdup_printf (
            g_dngettext (NULL, "%u item", "%u items", n_total), n_total);
    } else {
        text = g_strdup_printf (_ ("%1$u of %2$u selected"), n_sel, n_total);
    }

    gtk_label_set_text (GTK_LABEL (p->status_label), text);
    g_free (text);
}

/* ---- Event handlers ---- */

static void
on_navigated (HxFilesProvider *prov, const char *new_path, gpointer user_data)
{
    files_panel *p = user_data;
    (void)prov;
    gtk_editable_set_text (GTK_EDITABLE (p->path_entry),
                           new_path ? new_path : "");
    update_status (p);

    /* User just changed this panel's directory — restore focus
	 * to the column view in case the listing rebuild yanked it
	 * away. The flag is set by the navigation triggers below
	 * (on_row_activated, on_up_clicked, on_path_entry_activate)
	 * and cleared here, so refreshes that aren't user-driven
	 * (e.g. the auto-reload when the remote provider gains
	 * availability on connect) don't steal focus from whatever
	 * the user is currently working in. */
    if (p->wants_focus_restore) {
        p->wants_focus_restore = FALSE;
        if (p->column_view) {
            gtk_widget_grab_focus (p->column_view);
        }
    }
}

/* Provider's availability flipped (remote provider on login /
 * disconnect). Re-list when becoming available so the panel
 * shows real content instead of a stale "Not connected" pane. */
static void
on_unavailable_changed (HxFilesProvider *prov, gpointer user_data)
{
    files_panel *p = user_data;
    (void)prov;
    if (!hx_files_provider_get_unavailable_reason (p->provider)) {
        hx_files_provider_reload (p->provider);
    }
    update_status (p);
}

static void
on_selection_changed (GtkSelectionModel *sel, guint position, guint n_items,
                      gpointer user_data)
{
    (void)sel;
    (void)position;
    (void)n_items;
    update_status (user_data);
}

static void
on_items_changed (GListModel *m, guint pos, guint rem, guint add,
                  gpointer user_data)
{
    files_panel *p = user_data;
    (void)m;
    (void)pos;
    (void)rem;
    (void)add;
    /* Listing changed underneath us — any pending inline-rename
	 * arm is now suspect (the row's label may have been recycled
	 * to a different entry or destroyed outright). Cancel the
	 * timer to avoid firing rename against a stale binding. The
	 * label ref is dropped by inline_rename_cancel. */
    inline_rename_cancel (p);
    update_status (p);
}

/* Double-click / Enter on a row → descend if folder, no-op
 * otherwise. The browser-level Enter shortcut also routes through
 * here. */
static void
on_row_activated (GtkColumnView *view, guint pos, gpointer user_data)
{
    files_panel *p = user_data;
    HxFileEntry *e;
    (void)view;

    debug_log ("files", "on_row_activated: pos=%u", pos);

    /* Activation cancels any pending inline-rename — a double-click
	 * on the name labels arms the rename timer on its first press
	 * (the row is already selected), then the second press fires
	 * activation; if we didn't cancel here the rename dialog would
	 * pop up immediately after the activation. */
    inline_rename_cancel (p);

    e = g_list_model_get_item (G_LIST_MODEL (gtk_column_view_get_model (view)),
                               pos);
    if (!e) {
        return;
    }

    if (hx_file_entry_is_dir (e)) {
        const char *cur = hx_files_provider_get_current_path (p->provider);
        char *child;
        /* Path join: GIO-style "/" is the universal separator
		 * for both local (POSIX) and remote (Hotline) paths.
		 * g_build_filename does the right thing on both. */
        child = g_build_filename (cur ? cur : "/", hx_file_entry_get_name (e),
                                  NULL);
        p->wants_focus_restore = TRUE;
        hx_files_provider_navigate (p->provider, child);
        g_free (child);
    } else {
        /* Files: ask the provider to do its default action.
		 * Local launches the OS default app (xdg-open style);
		 * remote streams into the preview window. Phase 4
		 * polish item — gates inside each provider's
		 * activate_entry impl. */
        hx_files_provider_activate_entry (p->provider, e);
    }
    /* Plain files: no-op here in Phase 1. Phase 3 wires F5 / Copy
	 * to download/upload across panels; Phase 4 wires Enter on a
	 * file to a default action (preview on remote, xdg-open on
	 * local). */

    g_object_unref (e);
}

static void
on_path_entry_activate (GtkEntry *entry, gpointer user_data)
{
    files_panel *p = user_data;
    const char *txt = gtk_editable_get_text (GTK_EDITABLE (entry));
    if (!txt || !*txt) {
        return;
    }
    p->wants_focus_restore = TRUE;
    hx_files_provider_navigate (p->provider, txt);
}

static void
on_up_clicked (GtkButton *btn, gpointer user_data)
{
    files_panel *p = user_data;
    (void)btn;
    p->wants_focus_restore = TRUE;
    hx_files_provider_navigate_up (p->provider);
}

/* ---- Construction ---- */

static void
add_column (GtkColumnView *view, const char *title,
            void (*setup) (GtkSignalListItemFactory *, GtkListItem *, gpointer),
            void (*bind) (GtkSignalListItemFactory *, GtkListItem *, gpointer),
            gpointer factory_user_data, GCompareDataFunc cmp, int fixed_width,
            gboolean expand, gboolean is_default_sort)
{
    GtkListItemFactory *factory;
    GtkColumnViewColumn *col;
    GtkSorter *sorter;

    factory = gtk_signal_list_item_factory_new ();
    g_signal_connect (factory, "setup", G_CALLBACK (setup), factory_user_data);
    g_signal_connect (factory, "bind", G_CALLBACK (bind), factory_user_data);

    col = gtk_column_view_column_new (title, factory);
    if (fixed_width > 0) {
        gtk_column_view_column_set_fixed_width (col, fixed_width);
    }
    gtk_column_view_column_set_expand (col, expand);
    gtk_column_view_column_set_resizable (col, TRUE);

    sorter = GTK_SORTER (gtk_custom_sorter_new (cmp, NULL, NULL));
    gtk_column_view_column_set_sorter (col, sorter);
    g_object_unref (sorter);

    gtk_column_view_append_column (view, col);

    if (is_default_sort) {
        gtk_column_view_sort_by_column (view, col, GTK_SORT_ASCENDING);
    }

    g_object_unref (col);
}

/* Forward decls — these live below files_panel_new so they can
 * use the file-static helpers (cmp_*, name_*, etc.) without
 * needing their own forward decls in turn. */
static void panel_detach_provider (files_panel *p);
static void panel_attach_provider (files_panel *p, HxFilesProvider *provider);
static void on_side_dropdown_changed (GObject *obj, GParamSpec *pspec,
                                      gpointer user_data);

files_panel *
files_panel_new (HxFilesProvider *provider, files_panel_swap_cb swap_cb,
                 gpointer swap_cb_user_data)
{
    files_panel *p = g_new0 (files_panel, 1);
    GtkWidget *path_row, *scrolled, *footer;

    p->swap_cb = swap_cb;
    p->swap_cb_user_data = swap_cb_user_data;

    /* Row icons are loaded lazily by lookup_icon_paintable from
	 * the gresource (pre-extracted from icons.rsrc via
	 * tools/cicndump). The hashtable owns the GdkPaintable refs
	 * and drops them when the panel is freed. */
    p->icons = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL,
                                      (GDestroyNotify)g_object_unref);

    /* ---- Root box ---- */
    p->root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand (p->root, TRUE);
    gtk_widget_set_vexpand (p->root, TRUE);

    /* ---- Path row: [side dropdown] [Up] [path entry] ---- */
    path_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start (path_row, 6);
    gtk_widget_set_margin_end (path_row, 6);
    gtk_widget_set_margin_top (path_row, 6);
    gtk_widget_set_margin_bottom (path_row, 4);

    /* Side selector — only present when the caller wired a swap
	 * callback. Two fixed options: "Local" (idx 0) and "Remote"
	 * (idx 1). The initial selection is set by panel_attach_provider
	 * below from the actual provider type, so this widget tracks
	 * provider identity rather than driving it. */
    if (p->swap_cb) {
        const char *labels[] = { N_ ("Local"), N_ ("Remote"), NULL };
        p->side_dropdown
            = gtk_drop_down_new_from_strings ((const char *const *)labels);
        gtk_widget_set_tooltip_text (p->side_dropdown,
                                     _ ("Switch this panel between local "
                                        "filesystem and remote server"));
        p->side_dropdown_handler
            = g_signal_connect (p->side_dropdown, "notify::selected",
                                G_CALLBACK (on_side_dropdown_changed), p);
        gtk_box_append (GTK_BOX (path_row), p->side_dropdown);
    }

    p->up_btn = gtk_button_new_from_icon_name ("go-up-symbolic");
    gtk_widget_set_tooltip_text (p->up_btn, _ ("Up one level"));
    g_signal_connect (p->up_btn, "clicked", G_CALLBACK (on_up_clicked), p);
    gtk_box_append (GTK_BOX (path_row), p->up_btn);

    p->path_entry = gtk_entry_new ();
    gtk_widget_set_hexpand (p->path_entry, TRUE);
    g_signal_connect (p->path_entry, "activate",
                      G_CALLBACK (on_path_entry_activate), p);
    gtk_box_append (GTK_BOX (path_row), p->path_entry);

    gtk_box_append (GTK_BOX (p->root), path_row);

    /* ---- Column view ----
	 *
	 * The model chain is sort_model → selection → column_view.
	 * sort_model starts wrapping NULL — panel_attach_provider
	 * (called at the bottom of this function) plugs in the real
	 * provider's listing. The widget tree below stays put across
	 * provider swaps; only the underlying GListModel changes. */
    {
        GtkSorter *header_sorter;

        p->sort_model = gtk_sort_list_model_new (NULL, NULL);

        /* MultiSelection: Ctrl-click toggles, Shift-click extends,
		 * plain click replaces — standard orthodox-FM idiom. We
		 * pass our sort_model directly; the selection model rides
		 * on top and the column view's row factory does click
		 * handling. The earlier GtkSingleSelection bound only
		 * "0 or 1 row selected"; multi-select lets the user batch
		 * Copy / Delete the way classic Norton-style file managers
		 * do. */
        p->selection = gtk_multi_selection_new (G_LIST_MODEL (p->sort_model));
        /* gtk_multi_selection_new takes ownership of one ref on
		 * the underlying model. Re-add a ref for ours. */
        g_object_ref (p->sort_model);

        p->column_view
            = gtk_column_view_new (GTK_SELECTION_MODEL (p->selection));
        gtk_column_view_set_show_row_separators (
            GTK_COLUMN_VIEW (p->column_view), FALSE);
        gtk_column_view_set_show_column_separators (
            GTK_COLUMN_VIEW (p->column_view), FALSE);

        add_column (GTK_COLUMN_VIEW (p->column_view), _ ("Name"), name_setup,
                    name_bind, p, cmp_name, 240, TRUE, TRUE);
        add_column (GTK_COLUMN_VIEW (p->column_view), _ ("Size"),
                    text_setup_right, size_bind, NULL, cmp_size, 96, FALSE,
                    FALSE);
        add_column (GTK_COLUMN_VIEW (p->column_view), _ ("Modified"),
                    text_setup_right, modified_bind, NULL, cmp_modified, 120,
                    FALSE, FALSE);
        add_column (GTK_COLUMN_VIEW (p->column_view), _ ("Kind"),
                    text_setup_left, kind_bind, NULL, cmp_kind, 120, FALSE,
                    FALSE);

        /* Hand the column view's sort model to our GtkSortListModel
		 * so header clicks re-sort the model the selection sits
		 * on top of. */
        header_sorter
            = gtk_column_view_get_sorter (GTK_COLUMN_VIEW (p->column_view));
        gtk_sort_list_model_set_sorter (p->sort_model, header_sorter);

        g_signal_connect (p->column_view, "activate",
                          G_CALLBACK (on_row_activated), p);
        g_signal_connect (p->selection, "selection-changed",
                          G_CALLBACK (on_selection_changed), p);
    }
    /* Sentinels: -1 means "no prior click recorded". The very
	 * first click anywhere can't satisfy same_row (no prior pos)
	 * so the rename gate correctly never arms on a single click. */
    p->last_clicked_pos = -1;
    p->last_click_time_us = 0;

    scrolled = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                    GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand (scrolled, TRUE);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled),
                                   p->column_view);

    /* Wrap the scrolled view in a frame so the active-panel CSS
	 * class has somewhere to put an accent border. The base
	 * ".files-panel" class is always present (so the rounded
	 * corners + inactive-state styling apply) and
	 * files_panel_set_active toggles ".files-panel-active" on
	 * top of it. */
    p->frame = gtk_frame_new (NULL);
    gtk_widget_add_css_class (p->frame, "files-panel");
    gtk_widget_set_vexpand (p->frame, TRUE);
    gtk_frame_set_child (GTK_FRAME (p->frame), scrolled);
    gtk_widget_set_margin_start (p->frame, 6);
    gtk_widget_set_margin_end (p->frame, 6);
    gtk_box_append (GTK_BOX (p->root), p->frame);

    /* ---- Status footer ---- */
    footer = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_margin_start (footer, 12);
    gtk_widget_set_margin_end (footer, 12);
    gtk_widget_set_margin_top (footer, 4);
    gtk_widget_set_margin_bottom (footer, 6);
    p->status_label = gtk_label_new ("");
    gtk_label_set_xalign (GTK_LABEL (p->status_label), 0.0f);
    gtk_widget_add_css_class (p->status_label, "dim-label");
    gtk_widget_add_css_class (p->status_label, "caption");
    gtk_widget_set_hexpand (p->status_label, TRUE);
    gtk_box_append (GTK_BOX (footer), p->status_label);
    gtk_box_append (GTK_BOX (p->root), footer);

    /* Plug in the initial provider — wires up signal handlers,
	 * connects the model chain, configures path completion, and
	 * fires the first reload. */
    panel_attach_provider (p, provider);

    return p;
}

/* ---- Provider attach / detach (used by both files_panel_new
 * and files_panel_set_provider) ----
 *
 * panel_attach_provider takes a fresh ref on `provider` and
 * connects every per-provider signal handler. panel_detach_provider
 * disconnects them and drops the ref. files_panel_set_provider is
 * the public detach-then-attach combo. */

static void
panel_detach_provider (files_panel *p)
{
    if (!p || !p->provider) {
        return;
    }
    /* Drop any armed inline-rename — the new provider's listing
	 * will not have the row widget the timer is pointing at. The
	 * label ref taken at arm time is released here. */
    inline_rename_cancel (p);
    if (p->navigated_handler) {
        g_signal_handler_disconnect (p->provider, p->navigated_handler);
        p->navigated_handler = 0;
    }
    if (p->unavailable_handler) {
        g_signal_handler_disconnect (p->provider, p->unavailable_handler);
        p->unavailable_handler = 0;
    }
    if (p->items_changed_handler) {
        GListModel *listing = hx_files_provider_get_listing (p->provider);
        if (listing) {
            g_signal_handler_disconnect (listing, p->items_changed_handler);
        }
        p->items_changed_handler = 0;
    }
    g_clear_object (&p->provider);
}

static void
panel_attach_provider (files_panel *p, HxFilesProvider *provider)
{
    GListModel *listing;

    if (!p || !provider) {
        return;
    }

    p->provider = g_object_ref (provider);

    /* Swap the model under sort_model. The column view + selection
	 * sit on top of sort_model and ride along — items-changed events
	 * propagate up and the column view redraws. */
    listing = hx_files_provider_get_listing (provider);
    gtk_sort_list_model_set_model (p->sort_model, listing);

    p->items_changed_handler = g_signal_connect (
        listing, "items-changed", G_CALLBACK (on_items_changed), p);

    /* Path entry text reflects the new provider's current path. */
    gtk_editable_set_text (GTK_EDITABLE (p->path_entry),
                           hx_files_provider_get_current_path (provider));

    /* Path completion (popover with smart-case subdirectory
	 * suggestions as the user types). Local provider only —
	 * remote synchronous enumeration would block the UI thread on
	 * the network. We rebuild on every attach so a swap from
	 * remote→local enables completion and the reverse disables it. */
    if (p->path_complete) {
        hx_path_complete_free (p->path_complete);
        p->path_complete = NULL;
    }
    if (HX_IS_LOCAL_FILES_PROVIDER (provider)) {
        p->path_complete = hx_path_complete_attach (GTK_ENTRY (p->path_entry));
    }

    /* Side-dropdown selection mirrors the actual provider type.
	 * We block the change handler so the programmatic update
	 * doesn't fire the swap callback. */
    if (p->side_dropdown) {
        g_signal_handler_block (p->side_dropdown, p->side_dropdown_handler);
        gtk_drop_down_set_selected (GTK_DROP_DOWN (p->side_dropdown),
                                    HX_IS_LOCAL_FILES_PROVIDER (provider) ? 0
                                                                          : 1);
        g_signal_handler_unblock (p->side_dropdown, p->side_dropdown_handler);
    }

    p->navigated_handler = g_signal_connect (provider, "navigated",
                                             G_CALLBACK (on_navigated), p);
    p->unavailable_handler
        = g_signal_connect (provider, "unavailable-changed",
                            G_CALLBACK (on_unavailable_changed), p);

    /* Initial fetch — fires "navigated" after we connected so the
	 * path entry + status footer get filled. Remote provider skips
	 * the actual RPC pre-login (get_unavailable_reason gates it);
	 * the panel will catch up via on_unavailable_changed when the
	 * connection comes up. */
    hx_files_provider_reload (provider);
    update_status (p);
}

void
files_panel_set_provider (files_panel *p, HxFilesProvider *new_provider)
{
    if (!p || !new_provider || p->provider == new_provider) {
        return;
    }
    panel_detach_provider (p);
    panel_attach_provider (p, new_provider);
}

/* Side-dropdown callback. The user picked Local (idx 0) or Remote
 * (idx 1); ask the browser-supplied swap callback to build the
 * new provider and apply it. If the user picked the side we're
 * already on, the swap_cb is expected to no-op (and our dropdown
 * stays as-is). */
static void
on_side_dropdown_changed (GObject *obj, GParamSpec *pspec, gpointer user_data)
{
    files_panel *p = user_data;
    GtkDropDown *dd = GTK_DROP_DOWN (obj);
    guint selected;
    gboolean want_local;
    (void)pspec;

    if (!p || !p->swap_cb) {
        return;
    }
    selected = gtk_drop_down_get_selected (dd);
    want_local = (selected == 0);

    /* No-op if the dropdown's claim matches the actual provider —
	 * panel_attach_provider drives the dropdown from the provider
	 * type, but this guard makes the early-return explicit. */
    if (p->provider) {
        gboolean cur_local = HX_IS_LOCAL_FILES_PROVIDER (p->provider);
        if (cur_local == want_local) {
            return;
        }
    }
    p->swap_cb (p, want_local, p->swap_cb_user_data);
}

GtkWidget *
files_panel_get_widget (files_panel *p)
{
    return p ? p->root : NULL;
}

GtkWidget *
files_panel_get_column_view (files_panel *p)
{
    return p ? p->column_view : NULL;
}

HxFilesProvider *
files_panel_get_provider (files_panel *p)
{
    return p ? p->provider : NULL;
}

void
files_panel_set_active (files_panel *p, gboolean active)
{
    if (!p || !p->frame) {
        return;
    }
    if (active) {
        gtk_widget_add_css_class (p->frame, "files-panel-active");
    } else {
        gtk_widget_remove_css_class (p->frame, "files-panel-active");
    }
}

HxFileEntry *
files_panel_get_single_selected (files_panel *p)
{
    GtkBitset *sel;
    HxFileEntry *e = NULL;
    guint pos, n_sel;

    if (!p || !p->selection) {
        return NULL;
    }
    sel = gtk_selection_model_get_selection (
        GTK_SELECTION_MODEL (p->selection));
    if (!sel) {
        return NULL;
    }
    n_sel = (guint)gtk_bitset_get_size (sel);
    if (n_sel == 1) {
        pos = gtk_bitset_get_minimum (sel);
        e = g_list_model_get_item (G_LIST_MODEL (p->selection), pos);
        if (e) {
            g_object_unref (e); /* model still holds a ref */
        }
    }
    gtk_bitset_unref (sel);
    return e;
}

GPtrArray *
files_panel_get_selected_entries (files_panel *p)
{
    GtkBitset *sel;
    GtkBitsetIter iter;
    GPtrArray *out;
    guint pos;
    gboolean ok;

    if (!p || !p->selection) {
        return NULL;
    }

    /* Return value: GPtrArray of HxFileEntry* with one ref per
	 * entry (steal-the-ref ownership transfer to the caller).
	 * Caller frees via g_ptr_array_unref — the free_func runs
	 * g_object_unref on each. */
    out = g_ptr_array_new_with_free_func (g_object_unref);

    sel = gtk_selection_model_get_selection (
        GTK_SELECTION_MODEL (p->selection));
    if (!sel) {
        return out;
    }

    for (ok = gtk_bitset_iter_init_first (&iter, sel, &pos); ok;
         ok = gtk_bitset_iter_next (&iter, &pos)) {
        HxFileEntry *e
            = g_list_model_get_item (G_LIST_MODEL (p->selection), pos);
        if (e) {
            g_ptr_array_add (out, e); /* steal ref */
        }
    }
    gtk_bitset_unref (sel);
    return out;
}

void
files_panel_free (files_panel *p)
{
    if (!p) {
        return;
    }
    /* Cancel the pending inline-rename timer + drop the held ref
	 * so the closure callback never fires after the panel is gone. */
    inline_rename_cancel (p);
    if (p->path_complete) {
        hx_path_complete_free (p->path_complete);
        p->path_complete = NULL;
    }
    /* Drops the items-changed/navigated/unavailable handlers and
	 * the provider ref. */
    panel_detach_provider (p);
    if (p->icons) {
        g_hash_table_destroy (p->icons);
        p->icons = NULL;
    }
    /* p->root is owned by its parent widget and gets unparented
	 * when the parent is destroyed; we don't free it directly. */
    g_free (p);
}
