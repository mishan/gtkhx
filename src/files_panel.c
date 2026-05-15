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
#include <glib/gi18n.h>

#include "files.h" /* ICON_* */
#include "files_complete.h"
#include "files_entry.h"
#include "files_local_provider.h"
#include "files_provider.h"
#include "files_panel.h"

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

static int
cmp_size (gconstpointer a_p, gconstpointer b_p, gpointer user_data)
{
    HxFileEntry *a = (HxFileEntry *)a_p;
    HxFileEntry *b = (HxFileEntry *)b_p;
    guint64 as = hx_file_entry_get_size (a);
    guint64 bs = hx_file_entry_get_size (b);
    (void)user_data;
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
    gint64 am = hx_file_entry_get_modified (a);
    gint64 bm = hx_file_entry_get_modified (b);
    (void)user_data;
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
    (void)user_data;
    return g_utf8_collate (hx_file_entry_get_kind (a),
                           hx_file_entry_get_kind (b));
}

/* ---- Column factories ---- */

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
    GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *icon = gtk_image_new ();
    GtkWidget *lbl = gtk_label_new (NULL);
    (void)f;
    (void)d;

    /* XPMs are 16x16; scaled 1.5x = 24x24. Match that with
	 * pixel_size so GtkImage's icon-size clamp doesn't shrink
	 * them back down. */
    gtk_image_set_pixel_size (GTK_IMAGE (icon), 24);
    gtk_label_set_xalign (GTK_LABEL (lbl), 0.0f);
    gtk_label_set_ellipsize (GTK_LABEL (lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand (lbl, TRUE);

    gtk_box_append (GTK_BOX (row), icon);
    gtk_box_append (GTK_BOX (row), lbl);
    gtk_list_item_set_child (item, row);

    g_object_set_data (G_OBJECT (row), "icon", icon);
    g_object_set_data (G_OBJECT (row), "label", lbl);
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

    if (!e) {
        gtk_image_clear (icon);
        gtk_label_set_text (lbl, "");
        return;
    }

    paintable = lookup_icon_paintable (p, hx_file_entry_get_icon_id (e));
    if (paintable) {
        gtk_image_set_from_paintable (icon, paintable);
    } else {
        gtk_image_clear (icon);
    }
    gtk_label_set_text (lbl, hx_file_entry_get_name (e));
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

    if (n_sel == 0) {
        text = g_strdup_printf (
            g_dngettext (NULL, "%u item", "%u items", n_total), n_total);
    } else {
        text = g_strdup_printf (_ ("%u of %u selected"), n_sel, n_total);
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
    (void)m;
    (void)pos;
    (void)rem;
    (void)add;
    update_status (user_data);
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

    scrolled = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                    GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand (scrolled, TRUE);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled),
                                   p->column_view);

    /* Wrap the scrolled view in a frame so the active-panel CSS
	 * class has somewhere to put an accent border. */
    p->frame = gtk_frame_new (NULL);
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
